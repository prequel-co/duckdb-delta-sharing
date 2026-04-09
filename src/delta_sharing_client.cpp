#include "delta_sharing_client.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/http_exception.hpp"
#include "duckdb/common/file_system.hpp"
#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <sstream>
#include <fstream>

namespace duckdb {

using json = nlohmann::json;

// Callback for libcurl to write response data
static size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    ((std::string *)userp)->append((char *)contents, size * nmemb);
    return size * nmemb;
}

// DeltaSharingProfile implementation
DeltaSharingProfile DeltaSharingProfile::FromConfig(ClientContext &context) {
    DeltaSharingProfile profile;

    // Get endpoint from DuckDB configuration
    Value endpoint_value;
    if (!context.TryGetCurrentSetting("delta_sharing_endpoint", endpoint_value) ||
        endpoint_value.IsNull() || endpoint_value.ToString().empty()) {
        throw InvalidConfigurationException("LoadProfile error: Please initialize by running SET delta_sharing_endpoint='your_endpoint'");
    }
    profile.endpoint = endpoint_value.ToString();

    // Get bearer token from DuckDB configuration
    Value token_value;
    if (!context.TryGetCurrentSetting("delta_sharing_bearer_token", token_value) ||
        token_value.IsNull() || token_value.ToString().empty()) {
        throw InvalidConfigurationException("LoadProfile error: Please initialize by running SET delta_sharing_bearer_token='your_token'");
    }
    profile.bearer_token = token_value.ToString();

    // Get optional fields
    profile.share_credentials_version = 1;
    Value version_value;
    if (context.TryGetCurrentSetting("delta_sharing_credentials_version", version_value) &&
        !version_value.IsNull()) {
        profile.share_credentials_version = version_value.GetValue<int>();
    }

    profile.expiration_time = "";
    Value expiration_value;
    if (context.TryGetCurrentSetting("delta_sharing_expiration_time", expiration_value) &&
        !expiration_value.IsNull()) {
        profile.expiration_time = expiration_value.ToString();
    }

    // Remove trailing slash from endpoint if present
    if (!profile.endpoint.empty() && profile.endpoint.back() == '/') {
        profile.endpoint.pop_back();
    }

    return profile;
}

// DeltaSharingClient implementation
DeltaSharingClient::DeltaSharingClient(const DeltaSharingProfile &profile)
    : profile_(profile) {
    curl_ = curl_easy_init();
    if (!curl_) {
        throw InternalException("DeltaSharingClient error: Failed to initialize CURL");
    }
}

DeltaSharingClient::~DeltaSharingClient() {
    if (curl_) {
        curl_easy_cleanup((CURL *)curl_);
    }
}

std::string DeltaSharingClient::BuildUrl(const std::string &path, const std::string &query_params) {
    std::string url = profile_.endpoint + path;
    if (!query_params.empty()) {
        url += "?" + query_params;
    }
    return url;
}

HttpResponse DeltaSharingClient::PerformRequest(
    const std::string &method,
    const std::string &path,
    const std::string &query_params,
    const std::string &post_data) {

    HttpResponse response;
    response.success = false;
    response.status_code = 0;

    CURL *curl = (CURL *)curl_;
    std::string url = BuildUrl(path, query_params);
    std::string response_body;

    // Set URL
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

    // Set method
    if (method == "GET") {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        if (post_data.empty() || post_data == "null") {
            const char* empty_body = "{}";
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, empty_body);
        } else {
            curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, post_data.c_str());
        }

    } else if (method == "HEAD") {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    }

    // Set headers
    struct curl_slist *headers = nullptr;
    std::string auth_header = "Authorization: Bearer " + profile_.bearer_token;
    headers = curl_slist_append(headers, auth_header.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "User-Agent: delta-sharing-spark/3.1.0");
    headers = curl_slist_append(headers, "Accept: application/x-ndjson,application/json");
    headers = curl_slist_append(headers, "delta-sharing-capabilities: responseformat=delta;readerfeatures=deletionvectors,columnmapping");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    // Set write callback
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);

    // Perform request
    CURLcode res = curl_easy_perform(curl);

    // Get status code
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status_code);

    // Cleanup headers
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        response.error_message = curl_easy_strerror(res);
        response.success = false;
        return response;
    }

    response.body = response_body;
    response.success = (response.status_code >= 200 && response.status_code < 300);

    if (!response.success && !response_body.empty()) {
        try {
            auto error_json = json::parse(response_body);
            if (error_json.contains("message")) {
                response.error_message = error_json["message"].get<std::string>();
            }
        } catch (...) {
            response.error_message = response_body;
        }
    }

    return response;
}

std::vector<JsonValue> DeltaSharingClient::ParseNDJson(const std::string &response) {
    std::vector<JsonValue> results;
    std::istringstream stream(response);
    std::string line;

    while (std::getline(stream, line)) {
        if (line.empty()) {
            continue;
        }
        try {
            auto j = json::parse(line);
            results.push_back(JsonValue::FromInternal(&j));
        } catch (const std::exception &e) {
            throw SerializationException("ParseNDJson error: " + std::string(e.what()));
        }
    }

    return results;
}

JsonValue DeltaSharingClient::ListShares(int max_results, const std::string &page_token) {
    std::string query_params;
    if (max_results > 0) {
        query_params += "maxResults=" + std::to_string(max_results);
    }
    if (!page_token.empty()) {
        if (!query_params.empty()) query_params += "&";
        query_params += "pageToken=" + page_token;
    }

    auto response = PerformRequest("GET", "/shares", query_params);
    if (!response.success) {
        throw HTTPException("ListShares error: request failed. " + response.error_message);
    }

    try {
        auto j = json::parse(response.body);
        if (j.contains("items")) {
            return JsonValue::FromInternal(&j["items"]);
        }
        return JsonValue::Array();
    } catch (const std::exception &e) {
        throw SerializationException("ListShares error: failed to parse response. " + std::string(e.what()));
    }
}

Share DeltaSharingClient::GetShare(const std::string &share_name) {
    auto response = PerformRequest("GET", "/shares/" + share_name);
    if (!response.success) {
        throw HTTPException("GetShare error: request failed. " + response.error_message);
    }

    Share share;
    try {
        auto j = json::parse(response.body);
        share.name = j.at("share").at("name").get<std::string>();
        share.id = j.at("share").value("id", "");
    } catch (const std::exception &e) {
        throw SerializationException("GetShare error: failed to parse response. " + std::string(e.what()));
    }

    return share;
}

JsonValue DeltaSharingClient::ListSchemas(const std::string &share_name, int max_results, const std::string &page_token) {
    std::string query_params;
    if (max_results > 0) {
        query_params += "maxResults=" + std::to_string(max_results);
    }
    if (!page_token.empty()) {
        if (!query_params.empty()) query_params += "&";
        query_params += "pageToken=" + page_token;
    }

    auto response = PerformRequest("GET", "/shares/" + share_name + "/schemas", query_params);
    if (!response.success) {
        throw HTTPException("ListSchemas error: request failed. " + response.error_message);
    }

    try {
        auto j = json::parse(response.body);
        if (j.contains("items")) {
            return JsonValue::FromInternal(&j["items"]);
        }
        return JsonValue::Array();
    } catch (const std::exception &e) {
        throw SerializationException("ListSchemas error: failed to parse response. " + std::string(e.what()));
    }
}

JsonValue DeltaSharingClient::ListTables(const std::string &share_name, const std::string &schema_name, int max_results, const std::string &page_token) {
    std::string query_params;
    if (max_results > 0) {
        query_params += "maxResults=" + std::to_string(max_results);
    }
    if (!page_token.empty()) {
        if (!query_params.empty()) query_params += "&";
        query_params += "pageToken=" + page_token;
    }

    auto response = PerformRequest("GET", "/shares/" + share_name + "/schemas/" + schema_name + "/tables", query_params);
    if (!response.success) {
        throw HTTPException("ListTables error: request failed. " + response.error_message);
    }

    try {
        auto j = json::parse(response.body);
        if (j.contains("items")) {
            return JsonValue::FromInternal(&j["items"]);
        }
        return JsonValue::Array();
    } catch (const std::exception &e) {
        throw SerializationException("ListTables error: failed to parse response. " + std::string(e.what()));
    }
}

JsonValue DeltaSharingClient::ListAllTables(const std::string &share_name, int max_results, const std::string &page_token) {
    std::string query_params;
    if (max_results > 0) {
        query_params += "maxResults=" + std::to_string(max_results);
    }
    if (!page_token.empty()) {
        if (!query_params.empty()) query_params += "&";
        query_params += "pageToken=" + page_token;
    }

    auto response = PerformRequest("GET", "/shares/" + share_name + "/all-tables", query_params);
    if (!response.success) {
        throw HTTPException("ListAllTables error: request failed. " + response.error_message);
    }

    try {
        auto j = json::parse(response.body);
        if (j.contains("items")) {
            return JsonValue::FromInternal(&j["items"]);
        }
        return JsonValue::Array();
    } catch (const std::exception &e) {
        throw SerializationException("ListAllTables error: failed to parse response. " + std::string(e.what()));
    }
}

DeltaSharingClient::TableMetadataResponse DeltaSharingClient::QueryTableMetadata(
    const std::string &share_name,
    const std::string &schema_name,
    const std::string &table_name) {

    auto response = PerformRequest("GET", "/shares/" + share_name + "/schemas/" + schema_name + "/tables/" + table_name + "/metadata");
    if (!response.success) {
        throw HTTPException("QueryTableMetadata error: request failed. " + response.error_message);
    }

    TableMetadataResponse result;
    try {
        auto lines = ParseNDJson(response.body);
        if (lines.size() < 2) {
            throw SerializationException("QueryTableMetadata error: malformed response body from server.");
        }

        // First line: protocol - get internal json object
        auto* line0_json = static_cast<json*>(lines[0].GetInternalPtr());
        auto &protocol_obj = line0_json->at("protocol");
        result.protocol.min_reader_version = protocol_obj.at("minReaderVersion").get<int>();

        // Second line: metadata
        auto* line1_json = static_cast<json*>(lines[1].GetInternalPtr());
        auto &metadata_obj = line1_json->at("metaData");
        result.metadata.id = metadata_obj.at("id").get<std::string>();
        result.metadata.name = metadata_obj.value("name", "");
        result.metadata.description = metadata_obj.value("description", "");
        result.metadata.schema_string = metadata_obj.at("schemaString").get<std::string>();

        // Convert json to JsonValue for member fields
        auto partition_cols = metadata_obj.value("partitionColumns", json::array());
        result.metadata.partition_columns = JsonValue::FromInternal(&partition_cols);

        auto config = metadata_obj.value("configuration", json::object());
        result.metadata.configuration = JsonValue::FromInternal(&config);

        result.metadata.version = metadata_obj.value("version", 0);

        auto &format_obj = metadata_obj.at("format");
        result.metadata.format.provider = format_obj.at("provider").get<std::string>();

        auto options = format_obj.value("options", json::object());
        result.metadata.format.options = JsonValue::FromInternal(&options);

        if (metadata_obj.contains("accessModes")) {
            bool has_url = false;
            for (auto &mode : metadata_obj.at("accessModes")) {
                std::string m = mode.get<std::string>();
                result.metadata.access_modes.push_back(m);
                if (m == "url") has_url = true;
            }
            if (!has_url) {
                throw HTTPException("Table " + share_name + "." + schema_name + "." + table_name + " does not support URL-based access mode.");
            }
        }

    } catch (const std::exception &e) {
        throw SerializationException("QueryTableMetadata error: Failed to parse response. " + std::string(e.what()));
    }

    return result;
}

int64_t DeltaSharingClient::QueryTableVersion(
    const std::string &share_name,
    const std::string &schema_name,
    const std::string &table_name) {

    auto response = PerformRequest("HEAD", "/shares/" + share_name + "/schemas/" + schema_name + "/tables/" + table_name);
    if (!response.success) {
        throw HTTPException("QueryTableVersion error: request failed. " + response.error_message);
    }

    auto metadata = QueryTableMetadata(share_name, schema_name, table_name);
    return metadata.metadata.version;
}

DeltaSharingClient::QueryTableResult DeltaSharingClient::QueryTable(
    const std::string &share_name,
    const std::string &schema_name,
    const std::string &table_name,
    const JsonValue &predicate_hints,
    int64_t limit_hint,
    int64_t version) {

    // Build POST request body
    json request_body;
    request_body["responseFormat"] = "delta";
    request_body["predicateHints"] = json::array();
    if (!predicate_hints.IsEmpty()) {
        request_body["predicateHints"] = json::array();
        request_body["predicateHints"].push_back("string");
        request_body["jsonPredicateHints"] = predicate_hints.Dump();
    }
    if (limit_hint > 0) {
        request_body["limitHint"] = limit_hint;
    }
    if (version > 0) {
        request_body["version"] = version;
    }

    std::string post_data = request_body.dump();
    {
        std::ofstream debug_file("/tmp/duck_delta_share_request.json");
        debug_file << post_data;
    }

    auto response = PerformRequest("POST", "/shares/" + share_name + "/schemas/" + schema_name + "/tables/" + table_name + "/query", "", post_data);
    if (!response.success) {
        throw HTTPException("QueryTable error: request failed. " + response.error_message);
    }

    QueryTableResult result;
    try {
        auto lines = ParseNDJson(response.body);
        if (lines.empty()) {
            throw SerializationException("QueryTable error: empty response body from server.");
        }

        // Robust parsing: Find protocol and metadata in the first few lines
        bool found_protocol = false;
        bool found_metadata = false;

        for (size_t i = 0; i < std::min<size_t>(lines.size(), 10); i++) {
            auto* line_json = static_cast<json*>(lines[i].GetInternalPtr());
            if (!line_json || !line_json->is_object()) continue;

            if (!found_protocol && line_json->contains("protocol")) {
                auto &protocol_obj = line_json->at("protocol");
                if (protocol_obj.contains("deltaProtocol")) {
                    auto &delta_proto = protocol_obj.at("deltaProtocol");
                    result.protocol.min_reader_version = delta_proto.at("minReaderVersion").get<int>();
                } else if (protocol_obj.contains("minReaderVersion")) {
                    result.protocol.min_reader_version = protocol_obj.at("minReaderVersion").get<int>();
                }
                found_protocol = true;
            } else if (!found_metadata && line_json->contains("metaData")) {
                auto &metadata_top = line_json->at("metaData");
                // In 'delta' format, the metadata is often nested in 'deltaMetadata'
                const json* metadata_obj = &metadata_top;
                if (metadata_top.contains("deltaMetadata")) {
                    metadata_obj = &metadata_top.at("deltaMetadata");
                }

                result.metadata.id = metadata_obj->at("id").get<std::string>();
                result.metadata.schema_string = metadata_obj->at("schemaString").get<std::string>();
                // Convert to JsonValue
                auto part_cols = metadata_obj->at("partitionColumns");
                result.metadata.partition_columns = JsonValue::FromInternal(&part_cols);
                if (metadata_obj->contains("configuration")) {
                    auto config = metadata_obj->at("configuration");
                    result.metadata.configuration = JsonValue::FromInternal(&config);
                }
                auto &format_obj = metadata_obj->at("format");
                result.metadata.format.provider = format_obj.at("provider").get<std::string>();
                if (format_obj.contains("options")) {
                    auto opts = format_obj.at("options");
                    result.metadata.format.options = JsonValue::FromInternal(&opts);
                }

                if (metadata_obj->contains("accessModes")) {
                    bool has_url = false;
                    for (auto &mode : metadata_obj->at("accessModes")) {
                        std::string m = mode.get<std::string>();
                        result.metadata.access_modes.push_back(m);
                        if (m == "url") has_url = true;
                    }
                    if (!has_url) {
                        throw HTTPException("Table " + share_name + "." + schema_name + "." + table_name + " does not support URL-based access mode.");
                    }
                }

                found_metadata = true;
            }
        }

        if (!found_protocol || !found_metadata) {
            throw SerializationException("QueryTable error: missing protocol or metadata in response.");
        }

        // Action parsing
        for (auto &line : lines) {
            auto* line_json = static_cast<json*>(line.GetInternalPtr());
            if (!line_json || !line_json->is_object()) continue;

            if (line_json->contains("file")) {
                auto &file_top = line_json->at("file");
                
                // Check for 'delta' format nested structure
                if (file_top.contains("deltaSingleAction")) {
                    auto &single_action = file_top.at("deltaSingleAction");
                    if (single_action.contains("add")) {
                        auto &add_obj = single_action.at("add");
                        FileAction file;
                        // In delta format, the field is 'path', not 'url'
                        file.url = add_obj.at("path").get<std::string>();
                        file.id = file_top.value("id", "");
                        auto part_vals = add_obj.value("partitionValues", json::object());
                        file.partition_values = JsonValue::FromInternal(&part_vals);
                        file.size = add_obj.at("size").get<int64_t>();
                        if (add_obj.contains("stats")) {
                            auto s = add_obj.at("stats");
                            file.stats = JsonValue::FromInternal(&s);
                        }
                        if (add_obj.contains("version")) {
                            file.version = add_obj.at("version").get<int64_t>();
                        }
                        if (add_obj.contains("modificationTime")) {
                            file.timestamp = add_obj.at("modificationTime").get<int64_t>();
                        }
                        
                        // Handle deletion vectors
                        if (add_obj.contains("deletionVector")) {
                            file.has_deletion_vector = true;
                            auto &dv_obj = add_obj.at("deletionVector");
                            file.deletion_vector.storage_type = dv_obj.at("storageType").get<std::string>();
                            file.deletion_vector.path_or_inline_dv = dv_obj.at("pathOrInlineDv").get<std::string>();
                            file.deletion_vector.offset = dv_obj.value("offset", 0);
                            file.deletion_vector.size_in_bytes = dv_obj.value("sizeInBytes", 0);
                            file.deletion_vector.cardinality = dv_obj.value("cardinality", (int64_t)0);
                        }
                        result.files.push_back(file);
                    }
                } else {
                    // Legacy parquet format (responseformat=parquet) or top-level file action
                    FileAction file;
                    file.url = file_top.at("url").get<std::string>();
                    file.id = file_top.value("id", "");
                    auto part_vals = file_top.value("partitionValues", json::object());
                    file.partition_values = JsonValue::FromInternal(&part_vals);
                    file.size = file_top.at("size").get<int64_t>();
                    if (file_top.contains("stats")) {
                        auto s = file_top.at("stats");
                        file.stats = JsonValue::FromInternal(&s);
                    }
                    result.files.push_back(file);
                }
            } else if (line_json->contains("add")) {
                // Alternative top-level add format (less common in Sharing but possible)
                auto &add_obj = line_json->at("add");
                FileAction file;
                file.url = add_obj.contains("url") ? add_obj.at("url").get<std::string>() : add_obj.at("path").get<std::string>();
                file.id = add_obj.value("id", "");
                auto part_vals = add_obj.value("partitionValues", json::object());
                file.partition_values = JsonValue::FromInternal(&part_vals);
                file.size = add_obj.at("size").get<int64_t>();
                if (add_obj.contains("stats")) {
                    auto s = add_obj.at("stats");
                    file.stats = JsonValue::FromInternal(&s);
                }
                // Handle deletion vectors
                if (add_obj.contains("deletionVector")) {
                    file.has_deletion_vector = true;
                    auto &dv_obj = add_obj.at("deletionVector");
                    file.deletion_vector.storage_type = dv_obj.at("storageType").get<std::string>();
                    file.deletion_vector.path_or_inline_dv = dv_obj.at("pathOrInlineDv").get<std::string>();
                    file.deletion_vector.offset = dv_obj.value("offset", 0);
                    file.deletion_vector.size_in_bytes = dv_obj.value("sizeInBytes", 0);
                    file.deletion_vector.cardinality = dv_obj.value("cardinality", (int64_t)0);
                }
                result.files.push_back(file);
            }
        }
    } catch (const std::exception &e) {
        throw SerializationException("QueryTable error: failed to parse response. " + std::string(e.what()));
    }

    return result;
}

std::unordered_map<std::string, std::string> DeltaSharingClient::ParseColumnMapping(const std::string &schema_string) {
    std::unordered_map<std::string, std::string> mapping;
    try {
        if (schema_string.empty()) return mapping;

        json schema_json = json::parse(schema_string);
        if (!schema_json.contains("fields") || !schema_json.at("fields").is_array()) {
            return mapping;
        }

        auto &fields = schema_json.at("fields");
        for (auto &field : fields) {
            if (!field.contains("name") || !field.contains("metadata")) continue;

            std::string logical_name = field.at("name").get<std::string>();
            auto &metadata = field.at("metadata");

            if (metadata.contains("delta.columnMapping.physicalName")) {
                std::string physical_name = metadata.at("delta.columnMapping.physicalName").get<std::string>();
                mapping[logical_name] = physical_name;
            }
        }
    } catch (...) {
        // Fallback: mapping will be empty, which is fine (means no column mapping or invalid schema string)
    }
    return mapping;
}

} // namespace duckdb
