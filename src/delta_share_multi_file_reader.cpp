#include "delta_share_multi_file_reader.hpp"
#include "duck_delta_share_functions.hpp" // For ParseDeltaSchema if needed
#include "duckdb/common/types/value.hpp"

namespace duckdb {

// -----------------------------------------------------------------------------
// DeltaShareMultiFileList
// -----------------------------------------------------------------------------

DeltaShareMultiFileList::~DeltaShareMultiFileList() = default;

DeltaShareMultiFileList::DeltaShareMultiFileList(vector<OpenFileInfo> paths, vector<FileAction> files, TableMetadata metadata) 
    : SimpleMultiFileList(std::move(paths)), files(std::move(files)), metadata(std::move(metadata)) {
    // Build the partition columns hash set
    auto* partition_cols_json = static_cast<const json*>(this->metadata.partition_columns.GetInternalPtr());
    if (partition_cols_json && partition_cols_json->is_array()) {
        for (const auto& col : *partition_cols_json) {
            this->partition_columns.insert(col.get<string>());
        }
    }
}

// -----------------------------------------------------------------------------
// DeltaShareDeleteFilter
// -----------------------------------------------------------------------------

idx_t DeltaShareDeleteFilter::Filter(row_t start_row_index, idx_t count, SelectionVector &result_sel) {
    if (count == 0) return 0;
    result_sel.Initialize(STANDARD_VECTOR_SIZE);

    idx_t current_select = 0;
    for (idx_t i = 0; i < count; i++) {
        auto row_id = i + start_row_index;

        // Roaring bitmap contains DELETED rows.
        bool is_deleted = roaring::api::roaring_bitmap_contains(dv, row_id);
        
        // Select it only if it is NOT deleted
        if (!is_deleted) {
            result_sel.set_index(current_select++, i);
        }
    }
    return current_select;
}


// -----------------------------------------------------------------------------
// DeltaShareMultiFileReader
// -----------------------------------------------------------------------------

ReaderInitializeType DeltaShareMultiFileReader::InitializeReader(
        MultiFileReaderData &reader_data,
        const MultiFileBindData &bind_data,
        const vector<MultiFileColumnDefinition> &global_columns,
        const vector<ColumnIndex> &global_column_ids,
        optional_ptr<TableFilterSet> table_filters,
        ClientContext &context, MultiFileGlobalState &gstate) {

    FinalizeBind(reader_data, bind_data.file_options, bind_data.reader_bind, global_columns, global_column_ids, context, gstate.multi_file_reader_state);

    return MultiFileReader::InitializeReader(reader_data, bind_data, global_columns, global_column_ids, table_filters, context, gstate);
}

void DeltaShareMultiFileReader::FinalizeBind(
        MultiFileReaderData &reader_data, const MultiFileOptions &file_options,
        const MultiFileReaderBindData &options,
        const vector<MultiFileColumnDefinition> &global_columns,
        const vector<ColumnIndex> &global_column_ids, ClientContext &context,
        optional_ptr<MultiFileReaderGlobalState> global_state) {

    // First do base bind
    MultiFileReader::FinalizeBind(reader_data, file_options, options, global_columns, global_column_ids, context, global_state);

    if (!global_state) return;
    
    // Safety check because MultiFileReader intercepts bindings
    if (!global_state->file_list) return;

    // Get the DeltaShareMultiFileList
    auto *share_file_list = dynamic_cast<const DeltaShareMultiFileList*>(global_state->file_list.get());
    if (!share_file_list) return;

    idx_t file_index = reader_data.reader->file_list_idx.GetIndex();
    if (file_index >= share_file_list->files.size()) return;

    const auto &delta_file = share_file_list->files[file_index];

    // Push Partition Values into reader_data.constant_map
    // file_metadata.partition_map isn't easily mapped. So we map our own JSON partitions!
    auto* partitionValuesJson = static_cast<const nlohmann::json*>(delta_file.partition_values.GetInternalPtr());

    if (partitionValuesJson && partitionValuesJson->is_object()) {
        for (idx_t i = 0; i < global_column_ids.size(); i++) {
            auto global_idx = MultiFileGlobalIndex(i);
            column_t col_id = global_column_ids[i].GetPrimaryIndex();

            if (IsVirtualColumn(col_id)) {
                continue;
            }

            const string &col_name = global_columns[col_id].name;
            if (partitionValuesJson->contains(col_name)) {
                const auto &val_entry = (*partitionValuesJson)[col_name];
                Value val;
                auto &current_type = global_columns[col_id].type;

                if (val_entry.is_null()) {
                    val = Value(current_type);
                } else if (val_entry.is_string()) {
                    val = Value(val_entry.get<string>()).DefaultCastAs(current_type);
                } else if (val_entry.is_boolean()) {
                    val = Value::BOOLEAN(val_entry.get<bool>()).DefaultCastAs(current_type);
                } else if (val_entry.is_number_integer()) {
                    val = Value::BIGINT(val_entry.get<int64_t>()).DefaultCastAs(current_type);
                } else if (val_entry.is_number_float()) {
                    val = Value::DOUBLE(val_entry.get<double>()).DefaultCastAs(current_type);
                } else {
                    val = Value(val_entry.dump()).DefaultCastAs(current_type);
                }
                
                reader_data.constant_map.Add(global_idx, val);
            }
        }
    }

    // Attach Deletion Vector
    if (delta_file.has_deletion_vector) {
        // Technically, you would download the inline or path DV into a roaring bitmap!
        // Mocked implementation: (For this exercise we provide an empty bitmap or fetch it)
        roaring::api::roaring_bitmap_t *bm = roaring::api::roaring_bitmap_create();
        
        // TODO: decode delta_file.deletion_vector properties to populate `bm`

        reader_data.reader->deletion_filter = make_uniq<DeltaShareDeleteFilter>(bm);
    }
}

} // namespace duckdb
