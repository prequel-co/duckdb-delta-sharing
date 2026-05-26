#define DUCKDB_EXTENSION_MAIN

#include "duckdb_delta_sharing_extension.hpp"
#include "duckdb_delta_sharing_functions.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/main/materialized_query_result.hpp"
#ifndef DUCKDB_CPP_EXTENSION_ENTRY
#include "duckdb/main/extension_util.hpp"
#endif
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include <nlohmann/json.hpp>
#include <unordered_set>
#include "duckdb/common/types/vector.hpp"
#include "delta_share_multi_file_reader.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#if defined(__has_include)
#if __has_include("duckdb/common/multi_file/multi_file_function.hpp")
#include "duckdb/common/multi_file/multi_file_function.hpp"
#endif
#endif

#if defined(__has_include)
#if __has_include("duckdb/common/vector/list_vector.hpp")
#include "duckdb/common/vector/list_vector.hpp"
#define DUCK_DELTA_GET_DATA_MUTABLE FlatVector::GetDataMutable
#endif
#if __has_include("duckdb/common/vector/string_vector.hpp")
#include "duckdb/common/vector/string_vector.hpp"
#endif
#endif

#ifndef DUCK_DELTA_GET_DATA_MUTABLE
#define DUCK_DELTA_GET_DATA_MUTABLE FlatVector::GetData
#endif

#ifdef DUCKDB_CPP_EXTENSION_ENTRY
#define DUCKDB_REGISTER_FUNCTION(loader, func) loader.RegisterFunction(func)
#define DUCKDB_GET_DATABASE_INSTANCE(loader) loader.GetDatabaseInstance()
#define DUCKDB_GET_TABLE_FUNCTION(loader, con, name) loader.GetTableFunction(name)
#else
#define DUCKDB_REGISTER_FUNCTION(loader, func) ExtensionUtil::RegisterFunction(*loader.instance, func)
#define DUCKDB_GET_DATABASE_INSTANCE(loader) *loader.instance
#define DUCKDB_GET_TABLE_FUNCTION(loader, con, name) Catalog::GetSystemCatalog(*loader.instance).GetEntry<TableFunctionCatalogEntry>(*con.context, DEFAULT_SCHEMA, name)
#endif

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

struct DeltaShareEmptyInterface : public MultiFileReaderInterface {
	virtual ~DeltaShareEmptyInterface() {}
	unique_ptr<MultiFileReaderInterface> Copy() override {
		return make_uniq<DeltaShareEmptyInterface>();
	}
	void InitializeInterface(ClientContext &context, MultiFileReader &reader, MultiFileList &file_list) override {}
	unique_ptr<BaseFileReaderOptions> InitializeOptions(ClientContext &context, optional_ptr<TableFunctionInfo> info) override {
		return make_uniq<BaseFileReaderOptions>();
	}
	bool ParseCopyOption(ClientContext &context, const string &key, const vector<Value> &values, BaseFileReaderOptions &options,
	                     vector<string> &expected_names, vector<LogicalType> &expected_types) override {
		return false;
	}
	bool ParseOption(ClientContext &context, const string &key, const Value &val, MultiFileOptions &file_options,
	                 BaseFileReaderOptions &options) override {
		return false;
	}
	unique_ptr<TableFunctionData> InitializeBindData(MultiFileBindData &multi_file_data,
	                                                 unique_ptr<BaseFileReaderOptions> options) override {
		return make_uniq<TableFunctionData>();
	}
	void BindReader(ClientContext &context, vector<LogicalType> &return_types, vector<string> &names,
	                MultiFileBindData &bind_data) override {}
	unique_ptr<GlobalTableFunctionState> InitializeGlobalState(ClientContext &context, MultiFileBindData &bind_data,
	                                                         MultiFileGlobalState &global_state) override {
		return make_uniq<GlobalTableFunctionState>();
	}
	unique_ptr<LocalTableFunctionState> InitializeLocalState(ExecutionContext &, GlobalTableFunctionState &) override {
		return make_uniq<LocalTableFunctionState>();
	}
	shared_ptr<BaseFileReader> CreateReader(ClientContext &context, GlobalTableFunctionState &gstate, BaseUnionData &union_data,
	                                      const MultiFileBindData &bind_data_p) override {
		return nullptr;
	}
	shared_ptr<BaseFileReader> CreateReader(ClientContext &context, GlobalTableFunctionState &gstate, const OpenFileInfo &file,
	                                      idx_t file_idx, const MultiFileBindData &bind_data) override {
		return nullptr;
	}
	void GetVirtualColumns(ClientContext &context, MultiFileBindData &bind_data, virtual_column_map_t &result) override {
		// No virtual columns for empty share
	}
};

static table_function_t base_parquet_scan_function = nullptr;

static void ReadDeltaShareFunctionWrapper(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &bind_data = data_p.bind_data->Cast<MultiFileBindData>();
    auto &ds_file_list = bind_data.file_list->Cast<DeltaShareMultiFileList>();
    if (ds_file_list.files.empty()) {
        output.SetCardinality(0);
        return;
    }
    if (base_parquet_scan_function) {
        base_parquet_scan_function(context, data_p, output);
    }
}

static unique_ptr<MultiFileReader> CreateDeltaShareMultiFileReader(const TableFunction &function) {
    return make_uniq<DeltaShareMultiFileReader>();
}

static unique_ptr<FunctionData> ReadDeltaShareBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<string> &names) {

    if (input.inputs.size() < 3) {
        throw BinderException("ReadDeltaShareBind usage: delta_share_read('share_name', 'schema_name', 'table_name'[, timestamp])");
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
    string timestamp_str = "";
    if (input.inputs.size() >= 4) {
        auto val = input.inputs[3];
        if (!val.IsNull()) {
            timestamp_str = val.ToString();
            // Convert DuckDB format "YYYY-MM-DD HH:MM:SS" to ISO 8601 "YYYY-MM-DDTHH:MM:SSZ"
            std::replace(timestamp_str.begin(), timestamp_str.end(), ' ', 'T');
            if (timestamp_str.find('Z') == string::npos && timestamp_str.find('+') == string::npos) {
                timestamp_str += "Z";
            }
        }
    }
    
    auto query_result = client.QueryTable(share_name, schema_name, table_name, predicate_hints, -1, -1, timestamp_str);

    if (query_result.files.empty()) {
    vector<string> physical_names;
    DeltaSharingClient::ParseSparkSchema(query_result.metadata.schema_string, return_types, names, physical_names);
        // Still handle partition columns
        auto* partition_cols_json = static_cast<json*>(query_result.metadata.partition_columns.GetInternalPtr());
        for (const auto& col_json : *partition_cols_json) {
            string col = col_json.get<string>();
            if (std::find(names.begin(), names.end(), col) == names.end()) {
                names.push_back(col);
                return_types.push_back(LogicalType::VARCHAR);
            }
        }
        auto ds_file_list = shared_ptr<DeltaShareMultiFileList>(new DeltaShareMultiFileList({}, {}, std::move(query_result.metadata)));
        auto bind_data = make_uniq<MultiFileBindData>();
        bind_data->types = return_types;
        bind_data->names = names;
        bind_data->columns.clear();
        for (size_t i = 0; i < names.size(); i++) {
            string phys_name = (i < physical_names.size() && !physical_names[i].empty()) ? physical_names[i] : names[i];
            MultiFileColumnDefinition col_def(phys_name, return_types[i]);
            bind_data->columns.push_back(col_def);
        }
        bind_data->reader_bind.schema = bind_data->columns;
        bind_data->file_list = std::move(ds_file_list);
        bind_data->multi_file_reader = make_uniq<DeltaShareMultiFileReader>();
        bind_data->interface = make_uniq<DeltaShareEmptyInterface>();
        bind_data->bind_data = make_uniq<TableFunctionData>();
        return std::move(bind_data);
    }

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

    // Overwrite the returned schema with the LOGICAL schema from Delta Share!
    return_types.clear();
    names.clear();
    vector<string> physical_names;
    DeltaSharingClient::ParseSparkSchema(ds_file_list->metadata.schema_string, return_types, names, physical_names);

    auto &multi_file_bind = bind_data->Cast<MultiFileBindData>();
    multi_file_bind.types = return_types;
    multi_file_bind.names = names;
    multi_file_bind.columns.clear();
    for (size_t i = 0; i < names.size(); i++) {
        string phys_name = (i < physical_names.size() && !physical_names[i].empty()) ? physical_names[i] : names[i];
        MultiFileColumnDefinition col_def(phys_name, return_types[i]);
        multi_file_bind.columns.push_back(col_def);
    }

    // 5. Populate our MultiFileList so that the MultiFileReader has access to DeletionVectors and partition metadata!
    
    // Disable inference to respect Delta Sharing strict json typings
    multi_file_bind.file_options.auto_detect_hive_partitioning = false;
    multi_file_bind.file_options.hive_partitioning = false;
    multi_file_bind.file_options.union_by_name = true;
    
    for (const auto& col : ds_file_list->partition_columns) {
        auto it = std::find(names.begin(), names.end(), col);
        if (it == names.end()) {
            names.push_back(col);
            return_types.push_back(LogicalType::VARCHAR);
            MultiFileColumnDefinition col_def(col, LogicalType::VARCHAR);
            multi_file_bind.columns.push_back(col_def);
            multi_file_bind.names.push_back(col);
            multi_file_bind.types.push_back(LogicalType::VARCHAR);
        }
    }
    
    multi_file_bind.reader_bind.schema = multi_file_bind.columns;
    
    multi_file_bind.file_list = std::move(ds_file_list);
    multi_file_bind.multi_file_reader = make_uniq<DeltaShareMultiFileReader>();

    return bind_data;
}

static unique_ptr<FunctionData> ReadDeltaShareCdfBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<string> &names) {

    if (input.inputs.size() < 3) {
        throw BinderException("ReadDeltaShareCdfBind usage: delta_share_change_data_feed('share_name', 'schema_name', 'table_name'[, startingVersion[, endingVersion]])");
    }

    string share_name = input.inputs[0].GetValue<string>();
    string schema_name = input.inputs[1].GetValue<string>();
    string table_name = input.inputs[2].GetValue<string>();

    int64_t starting_version = -1;
    int64_t ending_version = -1;
    string starting_timestamp = "";
    string ending_timestamp = "";

    if (input.inputs.size() >= 4) {
        if (input.inputs[3].type().id() == LogicalTypeId::TIMESTAMP || input.inputs[3].type().id() == LogicalTypeId::TIMESTAMP_TZ) {
            starting_timestamp = input.inputs[3].ToString();
            std::replace(starting_timestamp.begin(), starting_timestamp.end(), ' ', 'T');
            if (starting_timestamp.find('Z') == string::npos && starting_timestamp.find('+') == string::npos) {
                starting_timestamp += "Z";
            }
        } else {
            starting_version = input.inputs[3].GetValue<int64_t>();
        }
    } else {
        starting_version = 0; // Default to 0
    }

    if (input.inputs.size() >= 5) {
        if (input.inputs[4].type().id() == LogicalTypeId::TIMESTAMP || input.inputs[4].type().id() == LogicalTypeId::TIMESTAMP_TZ) {
            ending_timestamp = input.inputs[4].ToString();
            std::replace(ending_timestamp.begin(), ending_timestamp.end(), ' ', 'T');
            if (ending_timestamp.find('Z') == string::npos && ending_timestamp.find('+') == string::npos) {
                ending_timestamp += "Z";
            }
        } else {
            ending_version = input.inputs[4].GetValue<int64_t>();
        }
    }

    DeltaSharingProfile profile = DeltaSharingProfile::FromConfig(context);
    DeltaSharingClient client(profile);
    
    auto query_result = client.QueryTableChanges(share_name, schema_name, table_name, starting_version, ending_version, starting_timestamp, ending_timestamp);

    if (query_result.files.empty()) {
        vector<string> physical_names;
        DeltaSharingClient::ParseSparkSchema(query_result.metadata.schema_string, return_types, names, physical_names);
        
        auto* partition_cols_json = static_cast<json*>(query_result.metadata.partition_columns.GetInternalPtr());
        for (const auto& col_json : *partition_cols_json) {
            string col = col_json.get<string>();
            if (std::find(names.begin(), names.end(), col) == names.end()) {
                names.push_back(col);
                return_types.push_back(LogicalType::VARCHAR);
            }
        }

        vector<string> cdf_cols = {"_change_type", "_commit_version", "_commit_timestamp"};
        vector<LogicalType> cdf_types = {LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::TIMESTAMP};
        for (size_t i = 0; i < cdf_cols.size(); i++) {
            if (std::find(names.begin(), names.end(), cdf_cols[i]) == names.end()) {
                names.push_back(cdf_cols[i]);
                return_types.push_back(cdf_types[i]);
            }
        }

        auto ds_file_list = shared_ptr<DeltaShareMultiFileList>(new DeltaShareMultiFileList({}, {}, std::move(query_result.metadata)));
        auto bind_data = make_uniq<MultiFileBindData>();
        bind_data->types = return_types;
        bind_data->names = names;
        bind_data->columns.clear();
        for (size_t i = 0; i < names.size(); i++) {
            string phys_name = (i < physical_names.size() && !physical_names[i].empty()) ? physical_names[i] : names[i];
            MultiFileColumnDefinition col_def(phys_name, return_types[i]);
            bind_data->columns.push_back(col_def);
        }
        bind_data->reader_bind.schema = bind_data->columns;
        bind_data->file_list = std::move(ds_file_list);
        bind_data->multi_file_reader = make_uniq<DeltaShareMultiFileReader>();
        bind_data->interface = make_uniq<DeltaShareEmptyInterface>();
        bind_data->bind_data = make_uniq<TableFunctionData>();
        return std::move(bind_data);
    }

    vector<Value> parquet_urls;
    vector<OpenFileInfo> open_file_infos;
    
    // Process actions to synthesize CDF columns if needed
    for (auto& file : query_result.files) {
        parquet_urls.push_back(Value(file.url));
        open_file_infos.push_back({file.url});
        
        if (file.cdf_action_type == "add" || file.cdf_action_type == "remove") {
            // No manual metadata appending to JSON needed, InitializeReader handles them natively.
        }
    }

    auto ds_file_list = shared_ptr<DeltaShareMultiFileList>(new DeltaShareMultiFileList(std::move(open_file_infos), std::move(query_result.files), std::move(query_result.metadata)));

    // Delegate to read_parquet
    auto &catalog = Catalog::GetSystemCatalog(context);
    auto &func_entry = catalog.GetEntry<TableFunctionCatalogEntry>(context, DEFAULT_SCHEMA, "read_parquet");
    auto read_parquet = func_entry.functions.GetFunctionByArguments(context, {LogicalType::LIST(LogicalType::VARCHAR)});

    read_parquet.get_multi_file_reader = CreateDeltaShareMultiFileReader;

    vector<Value> inputs_list;
    inputs_list.push_back(Value::LIST(LogicalType::VARCHAR, parquet_urls));

    TableFunctionBindInput inner_input(inputs_list, input.named_parameters, input.input_table_types, input.input_table_names, read_parquet.function_info.get(), input.binder, read_parquet, input.ref);
    
    // Do NOT pass modified return_types/names yet to prevent Parquet scanner column mismatch
    auto bind_data = read_parquet.bind(context, inner_input, return_types, names);

    // Overwrite the returned schema with the LOGICAL schema from Delta Share!
    return_types.clear();
    names.clear();
    vector<string> physical_names;
    DeltaSharingClient::ParseSparkSchema(ds_file_list->metadata.schema_string, return_types, names, physical_names);

    auto &multi_file_bind = bind_data->Cast<MultiFileBindData>();
    multi_file_bind.types = return_types;
    multi_file_bind.names = names;
    multi_file_bind.columns.clear();
    for (size_t i = 0; i < names.size(); i++) {
        string phys_name = (i < physical_names.size() && !physical_names[i].empty()) ? physical_names[i] : names[i];
        MultiFileColumnDefinition col_def(phys_name, return_types[i]);
        multi_file_bind.columns.push_back(col_def);
    }
    multi_file_bind.file_options.auto_detect_hive_partitioning = false;
    multi_file_bind.file_options.hive_partitioning = false;
    multi_file_bind.file_options.union_by_name = true;

    // Now safely append partition and CDF columns to schema
    for (const auto& col : ds_file_list->partition_columns) {
        if (std::find(names.begin(), names.end(), col) == names.end()) {
            names.push_back(col);
            return_types.push_back(LogicalType::VARCHAR);
            MultiFileColumnDefinition col_def(col, LogicalType::VARCHAR);
            multi_file_bind.columns.push_back(col_def);
            multi_file_bind.names.push_back(col);
            multi_file_bind.types.push_back(LogicalType::VARCHAR);
        }
    }

    vector<string> cdf_cols = {"_change_type", "_commit_version", "_commit_timestamp"};
    vector<LogicalType> cdf_types = {LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::TIMESTAMP};
    for (size_t i = 0; i < cdf_cols.size(); i++) {
        if (std::find(names.begin(), names.end(), cdf_cols[i]) == names.end()) {
            names.push_back(cdf_cols[i]);
            return_types.push_back(cdf_types[i]);
            MultiFileColumnDefinition col_def(cdf_cols[i], cdf_types[i]);
            multi_file_bind.columns.push_back(col_def);
            multi_file_bind.names.push_back(cdf_cols[i]);
            multi_file_bind.types.push_back(cdf_types[i]);
        }
    }

    multi_file_bind.reader_bind.schema = multi_file_bind.columns;

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
    auto list_data = DUCK_DELTA_GET_DATA_MUTABLE<list_entry_t>(result);
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
            auto child_data = DUCK_DELTA_GET_DATA_MUTABLE<string_t>(child_vector);

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

static unique_ptr<BaseSecret> CreateDeltaSharingSecretFromConfig(ClientContext &context, CreateSecretInput &input) {
    auto scope = input.scope;
    auto result = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);
    
    // endpoint is required
    if (input.options.find("endpoint") != input.options.end()) {
        result->secret_map["endpoint"] = input.options["endpoint"];
    } else {
        throw InvalidInputException("Delta Sharing secret requires 'endpoint' parameter");
    }
    
    // bearer_token is required
    if (input.options.find("bearer_token") != input.options.end()) {
        result->secret_map["bearer_token"] = input.options["bearer_token"];
    } else {
        throw InvalidInputException("Delta Sharing secret requires 'bearer_token' parameter");
    }

    result->redact_keys.insert("bearer_token");
    return std::move(result);
}

static unique_ptr<BaseSecret> CreateDeltaSharingSecretFromEnv(ClientContext &context, CreateSecretInput &input) {
    auto scope = input.scope;
    auto result = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);
    
    const char* env_ep = std::getenv("DELTA_SHARING_ENDPOINT");
    if (env_ep) {
        result->secret_map["endpoint"] = string(env_ep);
    } else {
        throw InvalidInputException("DELTA_SHARING_ENDPOINT environment variable is not set");
    }
    
    const char* env_token = std::getenv("DELTA_SHARING_BEARER_TOKEN");
    if (env_token) {
        result->secret_map["bearer_token"] = string(env_token);
    } else {
        throw InvalidInputException("DELTA_SHARING_BEARER_TOKEN environment variable is not set");
    }

    result->redact_keys.insert("bearer_token");
    return std::move(result);
}

static void LoadInternal(DUCKDB_DELTA_SHARING_EXTENSION_LOAD_PARAM) {
    auto &instance = DUCKDB_GET_DATABASE_INSTANCE(db);
    auto &config = DBConfig::GetConfig(instance);

	// Delta Sharing required extensions
    Connection con(instance);
	auto result = con.Query("LOAD httpfs");
	if (result->HasError()) {
		con.Query("INSTALL httpfs");
		con.Query("LOAD httpfs");
	}

	auto result_parquet = con.Query("LOAD parquet");
	if (result_parquet->HasError()) {
		con.Query("INSTALL parquet");
		con.Query("LOAD parquet");
	}

    // Delta Sharing config
    config.AddExtensionOption("delta_sharing_query_telemetry_enabled", "Enable sending full SQL query to server for telemetry", 
        LogicalType::BOOLEAN, 
        Value::BOOLEAN(false));

    // Delta Sharing Secrets Registration
    SecretType secret_type;
    secret_type.name = "delta_sharing";
    secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
    secret_type.default_provider = "config";
    SecretManager::Get(instance).RegisterSecretType(secret_type);
    
    CreateSecretFunction ds_config_fun = {"delta_sharing", "config", CreateDeltaSharingSecretFromConfig};
    ds_config_fun.named_parameters["endpoint"] = LogicalType::VARCHAR;
    ds_config_fun.named_parameters["bearer_token"] = LogicalType::VARCHAR;
    SecretManager::Get(instance).RegisterSecretFunction(ds_config_fun, OnCreateConflict::REPLACE_ON_CONFLICT);

    CreateSecretFunction ds_env_fun = {"delta_sharing", "env", CreateDeltaSharingSecretFromEnv};
    SecretManager::Get(instance).RegisterSecretFunction(ds_env_fun, OnCreateConflict::REPLACE_ON_CONFLICT);

    // Delta Sharing Functions
    TableFunction list("delta_share_list", {}, ListFunction, ListBind);
    list.varargs = LogicalType::VARCHAR;
    DUCKDB_REGISTER_FUNCTION(db, list);

    // Register our read_parquet overlay!
    auto &parquet_scan_entry = DUCKDB_GET_TABLE_FUNCTION(db, con, "read_parquet");
    TableFunction base_read = parquet_scan_entry.functions.functions[0];
    base_parquet_scan_function = base_read.function;

    base_read.function = ReadDeltaShareFunctionWrapper;
    base_read.get_multi_file_reader = CreateDeltaShareMultiFileReader;
    base_read.bind = ReadDeltaShareBind; 
    base_read.get_partition_stats = nullptr;
    base_read.statistics = nullptr;
    base_read.cardinality = nullptr;
    base_read.named_parameters.erase("schema");

    TableFunctionSet delta_share_read("delta_share_read");
    
    // 3-argument version
    TableFunction read_3arg = base_read;
    read_3arg.name = "delta_share_read";
    read_3arg.arguments = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
    delta_share_read.AddFunction(read_3arg);

    // 4-argument version (Time Travel - TIMESTAMP)
    TableFunction read_4arg_ts = base_read;
    read_4arg_ts.name = "delta_share_read";
    read_4arg_ts.arguments = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::TIMESTAMP};
    delta_share_read.AddFunction(read_4arg_ts);

    // 4-argument version (Time Travel - TIMESTAMPTZ)
    TableFunction read_4arg_tstz = base_read;
    read_4arg_tstz.name = "delta_share_read";
    read_4arg_tstz.arguments = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::TIMESTAMP_TZ};
    delta_share_read.AddFunction(read_4arg_tstz);

    DUCKDB_REGISTER_FUNCTION(db, delta_share_read);

    // CDF Function
    TableFunctionSet delta_share_cdf("delta_share_change_data_feed");
    TableFunction base_cdf = base_read;
    base_cdf.bind = ReadDeltaShareCdfBind;

    // 3-arg (starts from version 0)
    TableFunction cdf_3arg = base_cdf;
    cdf_3arg.arguments = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
    delta_share_cdf.AddFunction(cdf_3arg);

    // 4-arg (start version or timestamp)
    TableFunction cdf_4arg = base_cdf;
    cdf_4arg.arguments = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::ANY};
    delta_share_cdf.AddFunction(cdf_4arg);

    // 5-arg (start and end version/timestamp)
    TableFunction cdf_5arg = base_cdf;
    cdf_5arg.arguments = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::ANY, LogicalType::ANY};
    delta_share_cdf.AddFunction(cdf_5arg);

    DUCKDB_REGISTER_FUNCTION(db, delta_share_cdf);
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
    DUCKDB_REGISTER_FUNCTION(db, list_files_set);

}

void DuckdbDeltaSharingExtension::Load(DUCKDB_DELTA_SHARING_EXTENSION_LOAD_PARAM) {
    LoadInternal(db);
}

std::string DuckdbDeltaSharingExtension::Name() {
    return "duckdb_delta_sharing";
}

std::string DuckdbDeltaSharingExtension::Version() const {
#ifdef EXT_VERSION_DUCKDB_DELTA_SHARING
    return EXT_VERSION_DUCKDB_DELTA_SHARING;
#else
    return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(duckdb_delta_sharing, loader) {
    duckdb::LoadInternal(loader);
}
}