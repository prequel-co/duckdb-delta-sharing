#include "delta_share_multi_file_reader.hpp"
#include "duckdb_delta_sharing_functions.hpp" // For ParseDeltaSchema if needed
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/selection_vector.hpp"
#include "z85.hpp"
#include "roaring/roaring.h"

namespace duckdb {

// -----------------------------------------------------------------------------
// DeltaShareMultiFileList
// -----------------------------------------------------------------------------

DeltaShareMultiFileList::~DeltaShareMultiFileList() = default;

DeltaShareDeleteFilter::~DeltaShareDeleteFilter() {
    if (dv) {
        roaring::api::roaring_bitmap_free(dv);
    }
}

DeltaShareMultiFileList::DeltaShareMultiFileList(vector<OpenFileInfo> paths, vector<FileAction> files, TableMetadata metadata) 
    : SimpleMultiFileList(std::move(paths)), files(std::move(files)), metadata(std::move(metadata)) {
    // Build the partition columns hash set
    auto* partition_cols_json = static_cast<const json*>(this->metadata.partition_columns.GetInternalPtr());
    if (partition_cols_json && partition_cols_json->is_array()) {
        for (const auto& col : *partition_cols_json) {
            this->partition_columns.insert(col.get<string>());
        }
    }

    // Parse column mapping from schema string
    this->column_mapping = DeltaSharingClient::ParseColumnMapping(this->metadata.schema_string);
}

// -----------------------------------------------------------------------------
// DeltaShareDeleteFilter
// -----------------------------------------------------------------------------

idx_t DeltaShareDeleteFilter::Filter(row_t start_row_index, idx_t count, SelectionVector &result_sel) {
    if (count == 0) return 0;
    idx_t current_select = 0;
    for (idx_t i = 0; i < count; i++) {
        auto row_id = i + start_row_index;

        // Roaring bitmap contains DELETED rows.
        bool is_deleted = roaring::api::roaring_bitmap_contains(dv, (uint32_t)row_id);
        
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

    // 1. DuckDB's Phase 1 is FinalizeBind. This produces the default mapping, capturing missing columns
    //    and pushing them into reader_data.constant_map. We call it here explicitly.
    FinalizeBind(reader_data, bind_data.file_options, bind_data.reader_bind, global_columns, global_column_ids, context, gstate.multi_file_reader_state.get());

    // 2. NOW we inject our Constants BEFORE DuckDB creates the mapping expression tree!
    auto *share_file_list = dynamic_cast<const DeltaShareMultiFileList*>(bind_data.file_list.get());
    if (share_file_list && reader_data.reader && reader_data.reader->file_list_idx.IsValid()) {
        idx_t file_index = reader_data.reader->file_list_idx.GetIndex();
        if (file_index < share_file_list->files.size()) {
            const auto &delta_file = share_file_list->files[file_index];
            auto* partitionValuesJson = static_cast<const nlohmann::json*>(delta_file.partition_values.GetInternalPtr());

            if (partitionValuesJson && partitionValuesJson->is_object()) {
                for (idx_t i = 0; i < global_column_ids.size(); i++) {
                    auto global_idx = MultiFileGlobalIndex(i);
                    column_t col_id = global_column_ids[i].GetPrimaryIndex();

                    if (IsVirtualColumn(col_id)) {
                        continue;
                    }

                    const string &col_name = global_columns[col_id].name;
                    bool should_add = false;
                    Value val;

                    if (partitionValuesJson->contains(col_name)) {
                        const auto &val_entry = (*partitionValuesJson)[col_name];
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
                        should_add = true;
                    } else if (col_name == "_change_type" && !delta_file.cdf_action_type.empty()) {
                        val = Value(delta_file.cdf_action_type);
                        should_add = true;
                    } else if (col_name == "_commit_version" && delta_file.version >= 0) {
                        val = Value::BIGINT(delta_file.version);
                        should_add = true;
                    } else if (col_name == "_commit_timestamp" && delta_file.timestamp > 0) {
                        int64_t ms = delta_file.timestamp;
                        val = Value::TIMESTAMP(duckdb::timestamp_t(ms * 1000));
                        should_add = true;
                    }

                    if (should_add) {
                        bool found = false;
                        for (auto &entry : reader_data.constant_map) {
                            if (entry.column_idx.GetIndex() == global_idx.GetIndex()) {
                                entry.value = val;
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            reader_data.constant_map.Add(global_idx, std::move(val));
                        }
                    }
                }
            }

            if (delta_file.has_deletion_vector) {
                const auto &dv_meta = delta_file.deletion_vector;
                roaring::api::roaring_bitmap_t *dv_bitmap = nullptr;

                if (dv_meta.storage_type == "u" || dv_meta.storage_type == "i") { 
                    try {
                        auto decoded = Z85::Decode(dv_meta.path_or_inline_dv);
                        if (decoded.size() > 4) {
                            dv_bitmap = roaring::api::roaring_bitmap_portable_deserialize((const char*)decoded.data() + 4);
                        } else if (decoded.size() > 0) {
                             dv_bitmap = roaring::api::roaring_bitmap_portable_deserialize((const char*)decoded.data());
                        }
                    } catch (const std::exception &e) {
                        throw IOException("Failed to decode inline Deletion Vector: " + std::string(e.what()));
                    }
                } else if (dv_meta.storage_type == "p") {
                    try {
                        auto &fs = FileSystem::GetFileSystem(context);
                        auto handle = fs.OpenFile(dv_meta.path_or_inline_dv, FileFlags::FILE_FLAGS_READ);
                        auto size = handle->GetFileSize();
                        string raw_data;
                        raw_data.resize(size);
                        handle->Read((void *)raw_data.data(), size);
                        
                        if (size > 4) {
                            // Skip the 4-byte size preamble
                            dv_bitmap = roaring::api::roaring_bitmap_portable_deserialize((const char*)raw_data.data() + 4);
                        } else {
                            throw IOException("Remote Deletion Vector file too small");
                        }
                    } catch (const std::exception &e) {
                         throw IOException("Failed to fetch remote Deletion Vector: " + std::string(e.what()));
                    }
                } else {
                    throw NotImplementedException("Remote Deletion Vectors (storageType: " + dv_meta.storage_type + ") are not supported.");
                }

                if (dv_bitmap) {
                    reader_data.reader->deletion_filter = make_uniq<DeltaShareDeleteFilter>(dv_bitmap);
                }
            }
        }
    }

    // 3. DuckDB's Phase 2 is CreateMapping, mapping physical variables to expression pointers referencing constant_map
    return CreateMapping(context, reader_data, global_columns, global_column_ids, table_filters, gstate.file_list, bind_data.reader_bind, bind_data.virtual_columns);
}

void DeltaShareMultiFileReader::FinalizeBind(
        MultiFileReaderData &reader_data, const MultiFileOptions &file_options,
        const MultiFileReaderBindData &options,
        const vector<MultiFileColumnDefinition> &global_columns,
        const vector<ColumnIndex> &global_column_ids, ClientContext &context,
        optional_ptr<MultiFileReaderGlobalState> global_state) {

    if (!global_state || !global_state->file_list) {
        MultiFileReader::FinalizeBind(reader_data, file_options, options, global_columns, global_column_ids, context, global_state);
        return;
    }

    auto *share_file_list = dynamic_cast<const DeltaShareMultiFileList*>(global_state->file_list.get());
    
    if (share_file_list && !share_file_list->column_mapping.empty()) {
        vector<MultiFileColumnDefinition> mapped_columns = global_columns;
        for (auto &col : mapped_columns) {
            auto it = share_file_list->column_mapping.find(col.name);
            if (it != share_file_list->column_mapping.end()) {
                col.name = it->second;
            }
        }
        MultiFileReader::FinalizeBind(reader_data, file_options, options, mapped_columns, global_column_ids, context, global_state);
    } else {
        // Note: We leave FinalizeBind just for the column_mapping name rewrites.
        // The constant map injection logic is moving entirely back to InitializeReader where it belongs.
        MultiFileReader::FinalizeBind(reader_data, file_options, options, global_columns, global_column_ids, context, global_state);
    }
}

} // namespace duckdb
