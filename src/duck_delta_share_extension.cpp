#define DUCKDB_EXTENSION_MAIN

#include "duck_delta_share_extension.hpp"
#include "duck_delta_share_functions.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/main/materialized_query_result.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include <nlohmann/json.hpp>
#include <unordered_set>
#include "duckdb/common/vector/list_vector.hpp"
#include "delta_share_multi_file_reader.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"

namespace duckdb {

using json = nlohmann::json;

// Section: Function data binds
// Data binds used by delta_share functions

static unique_ptr<FunctionData> ListBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<string> &names) {

    auto result = make_uniq<ListBindData>();

    try {
        DeltaSharingProfile profile = DeltaSharingProfile::FromConfig(context);
        DeltaSharingClient client(profile);

        // Argument arity determines which function is called
        if (input.inputs.size() == 0) { // Shares
            result->list_type = 0;
            result->items = client.ListShares();
            names.push_back("name");
            names.push_back("id");
            return_types.push_back(LogicalType::VARCHAR);
            return_types.push_back(LogicalType::VARCHAR);

        } else if (input.inputs.size() == 1) { // Schemas
            result->list_type = 1;
            string share_name = input.inputs[0].GetValue<string>();
            result->items = client.ListSchemas(share_name);
            names.push_back("name");
            names.push_back("share");
            names.push_back("id");
            return_types.push_back(LogicalType::VARCHAR);
            return_types.push_back(LogicalType::VARCHAR);
            return_types.push_back(LogicalType::VARCHAR);

        } else if (input.inputs.size() == 2) { // Tables
            result->list_type = 2;
            string share_name = input.inputs[0].GetValue<string>();
            string schema_name = input.inputs[1].GetValue<string>();
            result->items = client.ListTables(share_name, schema_name);
            names.push_back("name");
            names.push_back("schema");
            names.push_back("share");
            names.push_back("id");
            return_types.push_back(LogicalType::VARCHAR);
            return_types.push_back(LogicalType::VARCHAR);
            return_types.push_back(LogicalType::VARCHAR);
            return_types.push_back(LogicalType::VARCHAR);

        } else {
            throw BinderException("ListBind error: function accepts 0, 1, 2 arguments");
        }
    } catch (const std::exception &e) {
        throw IOException("ListBind error: " + std::string(e.what()));
    }

    return std::move(result);
}

static void ListFunction(
    ClientContext &context,
    TableFunctionInput &data_p,
    DataChunk &output) {

    auto &bind_data = data_p.bind_data->CastNoConst<ListBindData>();

    idx_t count = 0;
    // Consider: architectural tradeoff to accept NULL json fields?
    // Convert JsonValue to json for internal processing
    auto* items_json = static_cast<json*>(bind_data.items.GetInternalPtr());
    while (bind_data.current_idx < items_json->size() && count < STANDARD_VECTOR_SIZE) {
        auto &item = (*items_json)[bind_data.current_idx];
        idx_t col = 0;

        output.SetValue(col++, count, Value(item["name"].get<string>()));

        if (bind_data.list_type == 1) {
            output.SetValue(col++, count, Value(item["share"].get<string>()));
        } else if (bind_data.list_type == 2) {
            output.SetValue(col++, count, Value(item["schema"].get<string>()));
            output.SetValue(col++, count, Value(item["share"].get<string>()));
        }

        // This should not be empty but our Delta Sharing server hides this from the response ._.
        output.SetValue(col++, count, Value(item["id"].is_null() ? "" : item["id"].get<string>()));
        bind_data.current_idx++;
        count++;
    }

    output.SetCardinality(count);
}

// -----------------------------------------------------------------------------
// ReadDeltaShare - MultiFileReader Parquet Overlay Pattern
// -----------------------------------------------------------------------------

static unique_ptr<MultiFileReader> CreateDeltaShareMultiFileReader(const TableFunction &function) {
    return make_uniq<DeltaShareMultiFileReader>();
}

static unique_ptr<FunctionData> ReadDeltaShareBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<string> &names) {

    if (input.inputs.size() < 3) {
        throw BinderException("ReadDeltaShareBind usage: delta_share_read('share_name', 'schema_name', 'table_name')");
    }

    string share_name = input.inputs[0].GetValue<string>();
    string schema_name = input.inputs[1].GetValue<string>();
    string table_name = input.inputs[2].GetValue<string>();

    vector<unique_ptr<Expression>> filters;

    // 1. Grab Parquet Scanner TableFunction
    auto &catalog = Catalog::GetSystemCatalog(context);
    auto &func_entry = catalog.GetEntry<TableFunctionCatalogEntry>(context, DEFAULT_SCHEMA, "read_parquet");
    auto read_parquet = func_entry.functions.GetFunctionByArguments(context, {LogicalType::LIST(LogicalType::VARCHAR)});

    // 2. Fetch URLs dynamically representing the Delta Share logical state
    DeltaSharingProfile profile = DeltaSharingProfile::FromConfig(context);
    DeltaSharingClient client(profile);
    
    JsonValue predicate_hints;
    auto query_result = client.QueryTable(share_name, schema_name, table_name, predicate_hints);

    vector<Value> parquet_urls;
    vector<OpenFileInfo> open_file_infos;
    for (const auto& file : query_result.files) {
        parquet_urls.push_back(Value(file.url));
        open_file_infos.push_back({file.url});
    }

    auto ds_file_list = shared_ptr<DeltaShareMultiFileList>(new DeltaShareMultiFileList(std::move(open_file_infos), std::move(query_result.files), std::move(query_result.metadata)));

    vector<Value> inputs_list;
    inputs_list.push_back(Value::LIST(LogicalType::VARCHAR, parquet_urls));

    // 3. Inject our MultiFileReader!
    read_parquet.get_multi_file_reader = CreateDeltaShareMultiFileReader;

    // 4. Delegate Bind to DuckDB's Native Parquet Scanner!
    TableFunctionBindInput inner_input(inputs_list, input.named_parameters, input.input_table_types, input.input_table_names, read_parquet.function_info.get(), input.binder, read_parquet, input.ref);
    auto bind_data = read_parquet.bind(context, inner_input, return_types, names);

    // 5. Populate our MultiFileList so that the MultiFileReader has access to DeletionVectors and partition metadata!
    auto &multi_file_bind = bind_data->Cast<MultiFileBindData>();
    
    // Disable inference to respect Delta Sharing strict json typings
    multi_file_bind.file_options.auto_detect_hive_partitioning = false;
    multi_file_bind.file_options.hive_partitioning = false;
    multi_file_bind.file_options.union_by_name = false;
    
    for (const auto& col : ds_file_list->partition_columns) {
        auto it = std::find(names.begin(), names.end(), col);
        if (it == names.end()) {
            names.push_back(col);
            return_types.push_back(LogicalType::VARCHAR);
            MultiFileColumnDefinition col_def(col, LogicalType::VARCHAR);
            multi_file_bind.reader_bind.schema.push_back(col_def); 
        }
    }
    
    multi_file_bind.file_list = std::move(ds_file_list);

    return bind_data;
}

// Section: delta_share_list_files scalar function

static unique_ptr<FunctionData> DeltaShareListFilesBind(
    ClientContext &context,
    ScalarFunction &bound_function,
    vector<unique_ptr<Expression>> &arguments) {

    auto result = make_uniq<DeltaShareListFilesBindData>();
    result->has_predicate_hints = (arguments.size() == 4);
    return std::move(result);
}

static void DeltaShareListFilesFunction(
    DataChunk &args,
    ExpressionState &state,
    Vector &result) {

    auto &bind_data = state.expr.Cast<BoundFunctionExpression>().bind_info->Cast<DeltaShareListFilesBindData>();

    idx_t count = args.size();

    // Setup result as LIST(VARCHAR)
    result.SetVectorType(VectorType::FLAT_VECTOR);
    auto list_data = FlatVector::GetData<list_entry_t>(result);
    auto &child_vector = ListVector::GetEntry(result);
    auto &result_validity = FlatVector::Validity(result);

    idx_t total_size = 0;

    // Process each row in the input
    for (idx_t row_idx = 0; row_idx < count; row_idx++) {
        // Get argument values
        auto share_name = args.data[0].GetValue(row_idx).ToString();
        auto schema_name = args.data[1].GetValue(row_idx).ToString();
        auto table_name = args.data[2].GetValue(row_idx).ToString();

        // Handle optional predicate hints
        JsonValue predicate_hints = JsonValue::Object();
        if (args.ColumnCount() >= 4) {
            auto pred_value = args.data[3].GetValue(row_idx);
            if (!pred_value.IsNull()) {
                string predicate_string = pred_value.ToString();
                auto predicate_json = ParsePredicateStringToJson(predicate_string);
                predicate_hints = JsonValue::FromInternal(&predicate_json);
            }
        }

        try {
            // Get client from context
            auto &context = state.GetContext();
            DeltaSharingProfile profile = DeltaSharingProfile::FromConfig(context);
            DeltaSharingClient client(profile);

            // Query table to get files
            auto query_result = client.QueryTable(
                share_name, schema_name, table_name, predicate_hints);

            // Set list entry metadata
            list_data[row_idx].offset = total_size;
            list_data[row_idx].length = query_result.files.size();

            // Reserve space in child vector
            ListVector::Reserve(result, total_size + query_result.files.size());
            auto child_data = FlatVector::GetData<string_t>(child_vector);

            // Add file URLs to child vector
            for (size_t i = 0; i < query_result.files.size(); i++) {
                child_data[total_size + i] = StringVector::AddString(child_vector, query_result.files[i].url);
            }

            total_size += query_result.files.size();

        } catch (const std::exception &e) {
            throw IOException("delta_share_list_files error: " + std::string(e.what()));
        }
    }

    ListVector::SetListSize(result, total_size);
}

static void LoadInternal(ExtensionLoader &loader) {
    auto &instance = loader.GetDatabaseInstance();
    auto &config = DBConfig::GetConfig(instance);

	// Delta Sharing required extensions
    Connection con(loader.GetDatabaseInstance());
	auto result = con.Query("LOAD httpfs");
	if (result->HasError()) {
		con.Query("INSTALL httpfs");
		con.Query("LOAD httpfs");
	}

    // Delta Sharing config
    const char* env_ep = std::getenv("DELTA_SHARING_ENDPOINT");
    const char* env_token = std::getenv("DELTA_SHARING_BEARER_TOKEN");
    config.AddExtensionOption("delta_sharing_endpoint", "URL of delta sharing server", 
        LogicalType::VARCHAR, 
        env_ep? std::string(env_ep) : "");
    config.AddExtensionOption("delta_sharing_bearer_token", "JWT Bearer token issued from server", 
        LogicalType::VARCHAR, 
        env_token? std::string(env_token) : "");

    // Delta Sharing Functions
    TableFunction list("delta_share_list", {}, ListFunction, ListBind);
    list.varargs = LogicalType::VARCHAR;
    loader.RegisterFunction(list);

    // Register our parquet_scan overlay!
    auto &parquet_scan_entry = loader.GetTableFunction("parquet_scan");
    TableFunction delta_share_read = parquet_scan_entry.functions.functions[0];

    delta_share_read.name = "delta_share_read";
    delta_share_read.arguments = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
    delta_share_read.get_multi_file_reader = CreateDeltaShareMultiFileReader;
    // MUST override the bind to parse share/schema/table and inject DeltaShareMultiFileList!
    delta_share_read.bind = ReadDeltaShareBind; 
    
    // Remove schema param to avoid duckdb binding errors
    delta_share_read.named_parameters.erase("schema");
    
    loader.RegisterFunction(delta_share_read);

    // Scalar function: delta_share_list_files
    ScalarFunctionSet list_files_set("delta_share_list_files");

    // 3-argument version (no predicate hints)
    ScalarFunction list_files_3arg(
        {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
        LogicalType::LIST(LogicalType::VARCHAR),
        DeltaShareListFilesFunction,
        DeltaShareListFilesBind);

    // 4-argument version (with predicate hints)
    ScalarFunction list_files_4arg(
        {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
        LogicalType::LIST(LogicalType::VARCHAR),
        DeltaShareListFilesFunction,
        DeltaShareListFilesBind);

    list_files_set.AddFunction(list_files_3arg);
    list_files_set.AddFunction(list_files_4arg);
    loader.RegisterFunction(list_files_set);

}

void DuckDeltaShareExtension::Load(ExtensionLoader &loader) {
    LoadInternal(loader);
}

std::string DuckDeltaShareExtension::Name() {
    return "duck_delta_share";
}

std::string DuckDeltaShareExtension::Version() const {
#ifdef EXT_VERSION_DUCK_DELTA_SHARE
    return EXT_VERSION_DUCK_DELTA_SHARE;
#else
    return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(duck_delta_share, loader) {
    duckdb::LoadInternal(loader);
}
}