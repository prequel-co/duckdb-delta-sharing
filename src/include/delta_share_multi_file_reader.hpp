#pragma once

#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "duckdb/common/multi_file/multi_file_list.hpp"
#include "delta_sharing_client.hpp"
#include "roaring/roaring.hh"
#include <mutex>
#include <unordered_map>

namespace duckdb {

class DeltaShareMultiFileReader;

struct DeltaShareMultiFileList : public SimpleMultiFileList {
public:
    ~DeltaShareMultiFileList() override;
    DeltaShareMultiFileList(vector<OpenFileInfo> paths, vector<FileAction> files, TableMetadata metadata);


    // To store Delta Share JSON metadata mapping
    vector<FileAction> files;
    TableMetadata metadata;

    // Cache the parsed partition column names
    std::unordered_set<std::string> partition_columns;

protected:

};

class DeltaShareMultiFileReader : public MultiFileReader {
public:
    DeltaShareMultiFileReader() {}
    
    unique_ptr<MultiFileReader> Copy() const override {
        return make_uniq<DeltaShareMultiFileReader>();
    }

    void FinalizeBind(MultiFileReaderData &reader_data, const MultiFileOptions &file_options,
                      const MultiFileReaderBindData &options,
                      const vector<MultiFileColumnDefinition> &global_columns,
                      const vector<ColumnIndex> &global_column_ids, ClientContext &context,
                      optional_ptr<MultiFileReaderGlobalState> global_state) override;

    ReaderInitializeType InitializeReader(MultiFileReaderData &reader_data,
                                          const MultiFileBindData &bind_data,
                                          const vector<MultiFileColumnDefinition> &global_columns,
                                          const vector<ColumnIndex> &global_column_ids,
                                          optional_ptr<TableFilterSet> table_filters,
                                          ClientContext &context, MultiFileGlobalState &gstate) override;
};

// Filter to apply croaring deletion vectors inside the DuckDB scanner!
class DeltaShareDeleteFilter : public DeleteFilter {
public:
    DeltaShareDeleteFilter(roaring::api::roaring_bitmap_t *dv) : dv(dv) {}
    ~DeltaShareDeleteFilter() override;
    idx_t Filter(row_t start_row_index, idx_t count, SelectionVector &result_sel) override;

private:
    roaring::api::roaring_bitmap_t *dv;
};

} // namespace duckdb
