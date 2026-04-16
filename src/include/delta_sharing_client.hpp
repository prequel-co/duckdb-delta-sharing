#pragma once

#include "duckdb.hpp"
#include "delta_sharing_json.hpp"
#include <string>
#include <vector>
#include <memory>

namespace duckdb {

// Delta Sharing Profile structure
struct DeltaSharingProfile {
    int share_credentials_version;
    std::string endpoint;
    std::string bearer_token;
    std::string expiration_time; // Optional, ISO 8601 format
    bool query_telemetry_disabled;
    std::string current_query;

    static DeltaSharingProfile FromConfig(ClientContext &context);
};

// Delta Sharing API response structures
struct Share {
    std::string name;
    std::string id; // Optional
};

struct Schema {
    std::string name;
    std::string share;
    std::string id; // Optional
};

struct Table {
    std::string name;
    std::string schema;
    std::string share;
    std::string id; // Optional
    std::string share_id; // Optional
};

struct Protocol {
    int min_reader_version;
};

struct Format {
    std::string provider; // "parquet"
    JsonValue options;
};

struct DeletionVector {
    std::string storage_type;
    std::string path_or_inline_dv;
    int64_t offset;
    int64_t size_in_bytes;
    int64_t cardinality;
};

struct TableMetadata {
    std::string id;
    std::string name;
    std::string description;
    Format format;
    std::string schema_string;
    JsonValue partition_columns;
    JsonValue configuration;
    std::vector<std::string> access_modes;
    int version;
};

struct FileAction {
    std::string url;
    std::string id;
    JsonValue partition_values;
    int64_t size;
    JsonValue stats; // Optional
    int64_t version = -1; // Optional
    int64_t timestamp = -1; // Optional
    std::string expiration_timestamp; // Optional
    bool has_deletion_vector = false;
    DeletionVector deletion_vector;
    
    // CDF specific
    std::string cdf_action_type; // "add", "cdf", "remove"
};

// HTTP Response structure
struct HttpResponse {
    long status_code;
    std::string body;
    std::map<std::string, std::string> headers;
    std::string error_message;
    bool success;
};

// Delta Sharing Client
class DeltaSharingClient {
public:
    DeltaSharingClient(const DeltaSharingProfile &profile);
    ~DeltaSharingClient();

    // List all shares - returns JSON array of items
    JsonValue ListShares(int max_results = -1, const std::string &page_token = "");

    // Get a specific share
    Share GetShare(const std::string &share_name);

    // Parse Spark SQL JSON schema and extract logical -> physical column mapping
    static std::unordered_map<std::string, std::string> ParseColumnMapping(const std::string &schema_string);

    // Parse Spark SQL JSON schema to DuckDB types and names
    static void ParseSparkSchema(const std::string &schema_string, vector<LogicalType> &return_types, vector<string> &names);

    // List schemas in a share - returns JSON array of items
    JsonValue ListSchemas(const std::string &share_name, int max_results = -1, const std::string &page_token = "");

    // List tables in a schema - returns JSON array of items
    JsonValue ListTables(const std::string &share_name, const std::string &schema_name, int max_results = -1, const std::string &page_token = "");

    // List all tables in a share - returns JSON array of items
    JsonValue ListAllTables(const std::string &share_name, int max_results = -1, const std::string &page_token = "");

    // Get table metadata
    struct TableMetadataResponse {
        Protocol protocol;
        TableMetadata metadata;
    };
    TableMetadataResponse QueryTableMetadata(const std::string &share_name, const std::string &schema_name, const std::string &table_name);

    // Get table version
    int64_t QueryTableVersion(const std::string &share_name, const std::string &schema_name, const std::string &table_name);

    // Query table files
    struct QueryTableResult {
        Protocol protocol;
        TableMetadata metadata;
        std::vector<FileAction> files;
    };
    QueryTableResult QueryTable(
        const std::string &share_name,
        const std::string &schema_name,
        const std::string &table_name,
        const JsonValue &predicate_hints = JsonValue::Object(),
        int64_t limit_hint = -1,
        int64_t version = -1,
        const std::string &timestamp = "");

    // Query table changes (CDF)
    QueryTableResult QueryTableChanges(
        const std::string &share_name,
        const std::string &schema_name,
        const std::string &table_name,
        int64_t starting_version = -1,
        int64_t ending_version = -1,
        const std::string &starting_timestamp = "",
        const std::string &ending_timestamp = "");

private:
    DeltaSharingProfile profile_;
    void *curl_; // CURL handle (opaque pointer to avoid including curl.h in header)

    // HTTP request helper
    HttpResponse PerformRequest(
        const std::string &method,
        const std::string &path,
        const std::string &query_params = "",
        const std::string &post_data = "");

    // Build URL with query parameters
    std::string BuildUrl(const std::string &path, const std::string &query_params = "");

    // Helper to parse a FileAction from a JSON object (add, cdf, remove, or file)
    void ParseFileAction(const JsonValue &obj, FileAction &file, const std::string &action_type);

    // Helper to parse protocol and metadata from response lines
    bool ParseProtocolAndMetadata(const std::vector<JsonValue> &lines, Protocol &protocol, TableMetadata &metadata, bool &found_protocol, bool &found_metadata);

    // Parse newline-delimited JSON response
    std::vector<JsonValue> ParseNDJson(const std::string &response);

    // Consolidated helper for paginated JSON GET requests
    JsonValue PerformPaginatedGet(const std::string &path, int max_results, const std::string &page_token);
};

} // namespace duckdb
