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

#include <algorithm>
#include <map>

// Callback for libcurl to write response data
static size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    ((std::string *)userp)->append((char *)contents, size * nmemb);
    return size * nmemb;
}

// Callback for libcurl to capture response headers
static size_t HeaderCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total_size = size * nmemb;
    std::string header((char *)contents, total_size);
    auto colon_pos = header.find(':');
    if (colon_pos != std::string::npos) {
        std::string key = header.substr(0, colon_pos);
        std::string value = header.substr(colon_pos + 1);
        // Trim whitespace
        key.erase(0, key.find_first_not_of(" \t\r\n"));
        key.erase(key.find_last_not_of(" \t\r\n") + 1);
        value.erase(0, value.find_first_not_of(" \t\r\n"));
        value.erase(value.find_last_not_of(" \t\r\n") + 1);
        
        // Convert key to lowercase for easier lookup
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        
        ((std::map<std::string, std::string> *)userp)->emplace(key, value);
    }
    return total_size;
}

static std::string GetNextPageLink(const std::map<std::string, std::string>& headers) {
    auto it = headers.find("link");
    if (it == headers.end()) return "";
    
    std::string link = it->second;
    // Format: <url>; rel="next"
    auto next_pos = link.find("rel=\"next\"");
    if (next_pos == std::string::npos) return "";
    
    auto start_pos = link.find('<');
    auto end_pos = link.find('>');
    if (start_pos != std::string::npos && end_pos != std::string::npos && start_pos < end_pos) {
        return link.substr(start_pos + 1, end_pos - start_pos - 1);
    }
    return "";
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

    // Set header callback
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);

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
    return PerformPaginatedGet("/shares", max_results, page_token);
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
    return PerformPaginatedGet("/shares/" + share_name + "/schemas", max_results, page_token);
}

JsonValue DeltaSharingClient::ListTables(const std::string &share_name, const std::string &schema_name, int max_results, const std::string &page_token) {
    return PerformPaginatedGet("/shares/" + share_name + "/schemas/" + schema_name + "/tables", max_results, page_token);
}

JsonValue DeltaSharingClient::ListAllTables(const std::string &share_name, int max_results, const std::string &page_token) {
    return PerformPaginatedGet("/shares/" + share_name + "/all-tables", max_results, page_token);
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

void DeltaSharingClient::ParseFileAction(const JsonValue &json_obj, FileAction &file, const std::string &action_type) {
    const json& obj = *static_cast<const json*>(json_obj.GetInternalPtr());
    file.cdf_action_type = action_type;
    file.url = obj.contains("url") ? obj.at("url").get<std::string>() : obj.at("path").get<std::string>();
    file.id = obj.value("id", "");
    auto part_vals = obj.value("partitionValues", json::object());
    file.partition_values = JsonValue::FromInternal(&part_vals);
    file.size = obj.at("size").get<int64_t>();
    if (obj.contains("stats")) {
        auto s = obj.at("stats");
        file.stats = JsonValue::FromInternal(&s);
    }
    if (obj.contains("version")) {
        file.version = obj.at("version").get<int64_t>();
    }
    if (obj.contains("timestamp")) {
        file.timestamp = obj.at("timestamp").get<int64_t>();
    } else if (obj.contains("modificationTime")) {
        file.timestamp = obj.at("modificationTime").get<int64_t>();
    }
    if (obj.contains("deletionVector")) {
        file.has_deletion_vector = true;
        auto &dv_obj = obj.at("deletionVector");
        file.deletion_vector.storage_type = dv_obj.at("storageType").get<std::string>();
        file.deletion_vector.path_or_inline_dv = dv_obj.at("pathOrInlineDv").get<std::string>();
        file.deletion_vector.offset = dv_obj.value("offset", 0);
        file.deletion_vector.size_in_bytes = dv_obj.value("sizeInBytes", 0);
        file.deletion_vector.cardinality = dv_obj.value("cardinality", (int64_t)0);
    }
}
bool DeltaSharingClient::ParseProtocolAndMetadata(
    const std::vector<JsonValue> &lines,
    Protocol &protocol,
    TableMetadata &metadata,
    bool &found_protocol,
    bool &found_metadata) {

    bool changed = false;
    if (!found_protocol || !found_metadata) {
        for (size_t i = 0; i < std::min<size_t>(lines.size(), 10); i++) {
            auto* line_json = static_cast<const json*>(lines[i].GetInternalPtr());
            if (!line_json || !line_json->is_object()) continue;

            if (!found_protocol && line_json->contains("protocol")) {
                auto &protocol_obj = line_json->at("protocol");
                if (protocol_obj.contains("deltaProtocol")) {
                    auto &delta_proto = protocol_obj.at("deltaProtocol");
                    protocol.min_reader_version = delta_proto.at("minReaderVersion").get<int>();
                } else if (protocol_obj.contains("minReaderVersion")) {
                    protocol.min_reader_version = protocol_obj.at("minReaderVersion").get<int>();
                }
                found_protocol = true;
                changed = true;
            } else if (!found_metadata && line_json->contains("metaData")) {
                auto &metadata_top = line_json->at("metaData");
                const json* metadata_obj = &metadata_top;
                if (metadata_top.contains("deltaMetadata")) {
                    metadata_obj = &metadata_top.at("deltaMetadata");
                }

                metadata.id = metadata_obj->at("id").get<std::string>();
                metadata.schema_string = metadata_obj->at("schemaString").get<std::string>();
                auto part_cols = metadata_obj->at("partitionColumns");
                metadata.partition_columns = JsonValue::FromInternal(&part_cols);
                if (metadata_obj->contains("configuration")) {
                    auto config = metadata_obj->at("configuration");
                    metadata.configuration = JsonValue::FromInternal(&config);
                }
                auto &format_obj = metadata_obj->at("format");
                metadata.format.provider = format_obj.at("provider").get<std::string>();
                if (format_obj.contains("options")) {
                    auto opts = format_obj.at("options");
                    metadata.format.options = JsonValue::FromInternal(&opts);
                }

                if (metadata_obj->contains("accessModes")) {
                    for (auto &mode : metadata_obj->at("accessModes")) {
                        metadata.access_modes.push_back(mode.get<std::string>());
                    }
                }
                found_metadata = true;
                changed = true;
            }
        }
    }
    return changed;
}

DeltaSharingClient::QueryTableResult DeltaSharingClient::QueryTable(
    const std::string &share_name,
    const std::string &schema_name,
    const std::string &table_name,
    const JsonValue &predicate_hints,
    int64_t limit_hint,
    int64_t version,
    const std::string &timestamp) {

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
    if (!timestamp.empty()) {
        request_body["timestamp"] = timestamp;
    }

    std::string post_data = request_body.dump();
    {
        std::ofstream debug_file("/tmp/duck_delta_share_request.json");
        debug_file << post_data;
    }

    QueryTableResult result;
    bool found_protocol = false;
    bool found_metadata = false;
    std::string next_url = "";
    bool first_page = true;

    while (true) {
        HttpResponse response;
        if (first_page) {
            response = PerformRequest("POST", "/shares/" + share_name + "/schemas/" + schema_name + "/tables/" + table_name + "/query", "", post_data);
        } else {
            response = PerformRequest("GET", next_url);
        }

        if (!response.success) {
            throw HTTPException("QueryTable error: request failed. " + response.error_message);
        }

        try {
            auto lines = ParseNDJson(response.body);
            if (lines.empty() && first_page) {
                throw SerializationException("QueryTable error: empty response body from server.");
            }

            ParseProtocolAndMetadata(lines, result.protocol, result.metadata, found_protocol, found_metadata);

            // Action parsing
            for (auto &line : lines) {
                auto* line_json = static_cast<json*>(line.GetInternalPtr());
                if (!line_json || !line_json->is_object()) continue;

                if (line_json->contains("file")) {
                    auto &file_top = line_json->at("file");
                    if (file_top.contains("deltaSingleAction")) {
                        auto &single_action = file_top.at("deltaSingleAction");
                        if (single_action.contains("add")) {
                            FileAction file;
                            ParseFileAction(JsonValue::FromInternal(&single_action.at("add")), file, "");
                            file.id = file_top.value("id", "");
                            result.files.push_back(file);
                        }
                    } else {
                        FileAction file;
                        ParseFileAction(JsonValue::FromInternal(&file_top), file, "");
                        result.files.push_back(file);
                    }
                } else if (line_json->contains("add")) {
                    FileAction file;
                    ParseFileAction(JsonValue::FromInternal(&line_json->at("add")), file, "add");
                    result.files.push_back(file);
                }
            }
        } catch (const std::exception &e) {
            throw SerializationException("QueryTable error: failed to parse response page. " + std::string(e.what()));
        }

        // Check for next page
        next_url = GetNextPageLink(response.headers);
        if (next_url.empty()) {
            break; 
        }
        first_page = false;
    }

    if (!found_protocol || !found_metadata) {
        throw SerializationException("QueryTable error: missing protocol or metadata in response.");
    }

    return result;
}

DeltaSharingClient::QueryTableResult DeltaSharingClient::QueryTableChanges(
    const std::string &share_name,
    const std::string &schema_name,
    const std::string &table_name,
    int64_t starting_version,
    int64_t ending_version,
    const std::string &starting_timestamp,
    const std::string &ending_timestamp) {

    std::string path = "/shares/" + share_name + "/schemas/" + schema_name + "/tables/" + table_name + "/changes";
    std::string query_params;
    if (starting_version >= 0) {
        query_params += "startingVersion=" + std::to_string(starting_version);
    } else if (!starting_timestamp.empty()) {
        query_params += "startingTimestamp=" + starting_timestamp;
    } else {
        throw BinderException("QueryTableChanges error: startingVersion or startingTimestamp must be provided.");
    }

    if (ending_version >= 0) {
        query_params += "&endingVersion=" + std::to_string(ending_version);
    } else if (!ending_timestamp.empty()) {
        query_params += "&endingTimestamp=" + ending_timestamp;
    }

    QueryTableResult result;
    bool found_protocol = false;
    bool found_metadata = false;

    std::string next_url = path;
    bool first_page = true;

    while (true) {
        HttpResponse response;
        if (first_page) {
            response = PerformRequest("GET", next_url, query_params);
        } else {
            response = PerformRequest("GET", next_url);
        }

        if (!response.success) {
            throw HTTPException("QueryTableChanges error: request failed. " + response.error_message);
        }

        auto lines = ParseNDJson(response.body);

        try {
            ParseProtocolAndMetadata(lines, result.protocol, result.metadata, found_protocol, found_metadata);

            // Action parsing
            for (auto &line : lines) {
                auto* line_json = static_cast<json*>(line.GetInternalPtr());
                if (!line_json || !line_json->is_object()) continue;

                if (line_json->contains("add")) {
                    FileAction file;
                    ParseFileAction(JsonValue::FromInternal(&line_json->at("add")), file, "add");
                    result.files.push_back(file);
                } else if (line_json->contains("cdf")) {
                    FileAction file;
                    ParseFileAction(JsonValue::FromInternal(&line_json->at("cdf")), file, "cdf");
                    result.files.push_back(file);
                } else if (line_json->contains("remove")) {
                    FileAction file;
                    ParseFileAction(JsonValue::FromInternal(&line_json->at("remove")), file, "remove");
                    result.files.push_back(file);
                }
            }
        } catch (const std::exception &e) {
            throw SerializationException("QueryTableChanges error: failed to parse response page. " + std::string(e.what()));
        }

        // Check for next page
        next_url = GetNextPageLink(response.headers);
        if (next_url.empty()) {
            break; 
        }
        first_page = false;
    }

    if (!found_protocol || !found_metadata) {
        throw SerializationException("QueryTableChanges error: missing protocol or metadata in response.");
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

JsonValue DeltaSharingClient::PerformPaginatedGet(const std::string &path, int max_results, const std::string &page_token) {
    json all_items = json::array();
    std::string current_token = page_token;

    while (true) {
        std::string query_params;
        if (max_results > 0) {
            query_params += "maxResults=" + std::to_string(max_results);
        }
        if (!current_token.empty()) {
            if (!query_params.empty()) query_params += "&";
            query_params += "pageToken=" + current_token;
        }

        auto response = PerformRequest("GET", path, query_params);
        if (!response.success) {
            throw HTTPException("Paginated GET error on " + path + ": request failed. " + response.error_message);
        }

        try {
            auto j = json::parse(response.body);
            if (j.contains("items") && j["items"].is_array()) {
                for (auto &item : j["items"]) {
                    all_items.push_back(item);
                }
            }

            if (j.contains("nextPageToken") && j["nextPageToken"].is_string() && !j["nextPageToken"].get<std::string>().empty()) {
                current_token = j["nextPageToken"].get<std::string>();
            } else {
                break;
            }
        } catch (const std::exception &e) {
            throw SerializationException("Paginated GET error on " + path + ": failed to parse response. " + std::string(e.what()));
        }
    }

    return JsonValue::FromInternal(&all_items);
}

void DeltaSharingClient::ParseSparkSchema(const std::string &schema_string, vector<LogicalType> &return_types, vector<string> &names) {
    try {
        if (schema_string.empty()) return;

        json schema_json = json::parse(schema_string);
        if (!schema_json.contains("fields") || !schema_json.at("fields").is_array()) {
            return;
        }

        auto &fields = schema_json.at("fields");
        for (auto &field : fields) {
            if (!field.contains("name") || !field.contains("type")) continue;

            string name = field.at("name").get<string>();
            json type_json = field.at("type");
            string type = "";
            if (type_json.is_string()) {
                type = type_json.get<string>();
            } else {
                // Complex or nested types not fully supported yet in empty scan, but we'll try to get the type name if it's an object
                if (type_json.is_object() && type_json.contains("type")) {
                    type = type_json.at("type").dump();
                }
            }

            names.push_back(name);
            if (type == "string") {
                return_types.push_back(LogicalType::VARCHAR);
            } else if (type == "integer") {
                return_types.push_back(LogicalType::INTEGER);
            } else if (type == "long") {
                return_types.push_back(LogicalType::BIGINT);
            } else if (type == "double") {
                return_types.push_back(LogicalType::DOUBLE);
            } else if (type == "float") {
                return_types.push_back(LogicalType::FLOAT);
            } else if (type == "boolean") {
                return_types.push_back(LogicalType::BOOLEAN);
            } else if (type == "timestamp") {
                return_types.push_back(LogicalType::TIMESTAMP);
            } else if (type == "date") {
                return_types.push_back(LogicalType::DATE);
            } else if (type == "binary") {
                return_types.push_back(LogicalType::BLOB);
            } else if (type == "byte") {
                return_types.push_back(LogicalType::TINYINT);
            } else if (type == "short") {
                return_types.push_back(LogicalType::SMALLINT);
            } else {
                return_types.push_back(LogicalType::VARCHAR); // Default fallback
            }
        }
    } catch (...) {
        // Fallback: names/types will stay as they were (likely empty)
    }
}

} // namespace duckdb
