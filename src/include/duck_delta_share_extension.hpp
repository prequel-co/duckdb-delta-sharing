#pragma once

#include "duckdb.hpp"
#include "delta_sharing_client.hpp"
#include <atomic>

namespace duckdb {

// Unified bind data for all list operations
struct ListBindData : public TableFunctionData {
    JsonValue items;  // Store items as JSON array
    idx_t current_idx = 0;
    int list_type; // 0=shares, 1=schemas, 2=tables
};


struct DeltaShareListFilesBindData : public FunctionData {
    string share_name;
    string schema_name;
    string table_name;
    string predicate_hints_string;
    bool has_predicate_hints = false;

    unique_ptr<FunctionData> Copy() const override {
        auto copy = make_uniq<DeltaShareListFilesBindData>();
        copy->share_name = share_name;
        copy->schema_name = schema_name;
        copy->table_name = table_name;
        copy->predicate_hints_string = predicate_hints_string;
        copy->has_predicate_hints = has_predicate_hints;
        return std::move(copy);
    }

    bool Equals(const FunctionData &other_p) const override {
        auto &other = other_p.Cast<DeltaShareListFilesBindData>();
        return share_name == other.share_name &&
               schema_name == other.schema_name &&
               table_name == other.table_name &&
               predicate_hints_string == other.predicate_hints_string;
    }
};

class DuckDeltaShareExtension : public Extension {
public:
	void Load(ExtensionLoader &db) override;
	std::string Name() override;
	std::string Version() const override;
};

} // namespace duckdb
