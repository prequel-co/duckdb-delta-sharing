#include "delta_sharing_client.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/http_exception.hpp"
#include "duckdb/common/file_system.hpp"
#include <nlohmann/json.hpp>
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#ifndef __EMSCRIPTEN__
#include <curl/curl.h>
#else
#include <emscripten/fetch.h>
#endif
#include <sstream>
#include <fstream>

namespace duckdb {

using json = nlohmann::json;

#include <algorithm>
#include <map>

#ifndef __EMSCRIPTEN__
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
#endif

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

    auto &sm = SecretManager::Get(context);
    auto trans = CatalogTransaction::GetSystemCatalogTransaction(context);
    auto secrets = sm.AllSecrets(trans);
    const KeyValueSecret *ds_secret = nullptr;
    for (auto &sec : secrets) {
        if (sec.secret && sec.secret->GetType() == "delta_sharing") {
            ds_secret = dynamic_cast<const KeyValueSecret*>(sec.secret.get());
        }
    }

    if (!ds_secret) {
        throw InvalidConfigurationException("LoadProfile error: Please configure Delta Sharing via a secret: CREATE SECRET (TYPE delta_sharing, PROVIDER config, ENDPOINT '...', BEARER_TOKEN '...') or CREATE SECRET (TYPE delta_sharing, PROVIDER env)");
    }
    
    Value endpoint_value;
    bool has_endpoint = false;
    try {
        endpoint_value = ds_secret->TryGetValue("endpoint", false);
        has_endpoint = !endpoint_value.IsNull() && !endpoint_value.ToString().empty();
    } catch (...) {}

    if (has_endpoint) {
        profile.endpoint = endpoint_value.ToString();
    } else {
        throw InvalidConfigurationException("LoadProfile error: Please configure Delta Sharing via a secret: CREATE SECRET (TYPE delta_sharing, PROVIDER config, ENDPOINT '...', BEARER_TOKEN '...') or CREATE SECRET (TYPE delta_sharing, PROVIDER env)");
    }

    Value token_value;
    bool has_token = false;
    try {
        token_value = ds_secret->TryGetValue("bearer_token", false);
        has_token = !token_value.IsNull() && !token_value.ToString().empty();
    } catch (...) {}

    if (has_token) {
        profile.bearer_token = token_value.ToString();
    } else {
        throw InvalidConfigurationException("LoadProfile error: Please configure Delta Sharing via a secret: CREATE SECRET (TYPE delta_sharing, PROVIDER config, ENDPOINT '...', BEARER_TOKEN '...') or CREATE SECRET (TYPE delta_sharing, PROVIDER env)");
    }

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

    profile.query_telemetry_enabled = false;
    Value telemetry_enabled_value;
    if (context.TryGetCurrentSetting("delta_sharing_query_telemetry_enabled", telemetry_enabled_value) &&
        !telemetry_enabled_value.IsNull()) {
        profile.query_telemetry_enabled = telemetry_enabled_value.GetValue<bool>();
    }
    profile.current_query = "";
    if (profile.query_telemetry_enabled) {
        // Note: active_query is null during the Bind phase in database/sql, which throws an InternalException.
        // Due to C++ ABI boundaries, this exception often cannot be caught. Disable telemetry to bypass this safely.
        try {
            profile.current_query = context.GetCurrentQuery();
        } catch (...) {
            // active_query may be null during the bind phase
        }
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
#ifndef __EMSCRIPTEN__
    curl_ = curl_easy_init();
    if (!curl_) {
        throw InternalException("DeltaSharingClient error: Failed to initialize CURL");
    }
#else
    curl_ = nullptr;
#endif
}

DeltaSharingClient::~DeltaSharingClient() {
#ifndef __EMSCRIPTEN__
    if (curl_) {
        curl_easy_cleanup((CURL *)curl_);
    }
#endif
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

    std::string url = BuildUrl(path, query_params);
    std::string response_body;

#ifdef __EMSCRIPTEN__
    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);
    strcpy(attr.requestMethod, method.c_str());
    attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_SYNCHRONOUS;

    std::vector<const char*> headers;
    std::string auth_header = "Bearer " + profile_.bearer_token;
    headers.push_back("Authorization");
    headers.push_back(auth_header.c_str());
    headers.push_back("Content-Type");
    headers.push_back("application/json");
    headers.push_back("User-Agent");
    headers.push_back("delta-sharing-spark/3.1.0");
    headers.push_back("Accept");
    headers.push_back("application/x-ndjson,application/json");
    headers.push_back("delta-sharing-capabilities");
    headers.push_back("responseformat=delta;readerfeatures=deletionvectors,columnmapping,timestampntz");

    std::string telemetry_encoded;
    if (profile_.query_telemetry_enabled && !profile_.current_query.empty()) {
        std::string query = profile_.current_query;
        if (query.length() > 2048) {
            query = query.substr(0, 2048);
        }
        static const char* lookup = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        int val = 0, valb = -6;
        for (unsigned char c : query) {
            val = (val << 8) + c;
            valb += 8;
            while (valb >= 0) {
                telemetry_encoded.push_back(lookup[(val >> valb) & 0x3F]);
                valb -= 6;
            }
        }
        if (valb > -6) telemetry_encoded.push_back(lookup[((val << 8) >> (valb + 8)) & 0x3F]);
        while (telemetry_encoded.size() % 4) telemetry_encoded.push_back('=');

        headers.push_back("delta-sharing-query-sql");
        headers.push_back(telemetry_encoded.c_str());
    }
    headers.push_back(nullptr);
    attr.requestHeaders = headers.data();

    if (method == "POST") {
        if (post_data.empty() || post_data == "null") {
            attr.requestData = "{}";
            attr.requestDataSize = 2;
        } else {
            attr.requestData = post_data.c_str();
            attr.requestDataSize = post_data.length();
        }
    }

    emscripten_fetch_t *fetch = emscripten_fetch(&attr, url.c_str());
    response.status_code = fetch->status;
    response.success = (fetch->status >= 200 && fetch->status < 300);

    if (fetch->data && fetch->numBytes > 0) {
        response_body = std::string(fetch->data, fetch->numBytes);
    } else if (!response.success) {
        response.error_message = "HTTP error " + std::to_string(fetch->status);
    }

    size_t header_len = emscripten_fetch_get_response_headers_length(fetch);
    if (header_len > 0) {
        std::string all_headers(header_len, '\0');
        emscripten_fetch_get_response_headers(fetch, &all_headers[0], header_len);
        std::istringstream stream(all_headers);
        std::string header_line;
        while (std::getline(stream, header_line)) {
            auto colon_pos = header_line.find(':');
            if (colon_pos != std::string::npos) {
                std::string key = header_line.substr(0, colon_pos);
                std::string value = header_line.substr(colon_pos + 1);
                key.erase(0, key.find_first_not_of(" \t\r\n"));
                key.erase(key.find_last_not_of(" \t\r\n") + 1);
                value.erase(0, value.find_first_not_of(" \t\r\n"));
                value.erase(value.find_last_not_of(" \t\r\n") + 1);
                std::transform(key.begin(), key.end(), key.begin(), ::tolower);
                response.headers[key] = value;
            }
        }
    }

    emscripten_fetch_close(fetch);
    response.body = response_body;

#else
    CURL *curl = (CURL *)curl_;

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
    headers = curl_slist_append(headers, "delta-sharing-capabilities: responseformat=delta;readerfeatures=deletionvectors,columnmapping,timestampntz");

    if (profile_.query_telemetry_enabled && !profile_.current_query.empty()) {
        std::string query = profile_.current_query;
        if (query.length() > 2048) {
            query = query.substr(0, 2048);
        }

        // Simple Base64 Encode
        static const char* lookup = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string encoded;
        int val = 0, valb = -6;
        for (unsigned char c : query) {
            val = (val << 8) + c;
            valb += 8;
            while (valb >= 0) {
                encoded.push_back(lookup[(val >> valb) & 0x3F]);
                valb -= 6;
            }
        }
        if (valb > -6) encoded.push_back(lookup[((val << 8) >> (valb + 8)) & 0x3F]);
        while (encoded.size() % 4) encoded.push_back('=');

        std::string telemetry_header = "delta-sharing-query-sql: " + encoded;
        headers = curl_slist_append(headers, telemetry_header.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    // Set write callback
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);

    // Set header callback
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);

    char errbuf[CURL_ERROR_SIZE];
    errbuf[0] = 0;
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

    // Perform request
    CURLcode res = curl_easy_perform(curl);

    // Get status code
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status_code);

    // Cleanup headers
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        size_t len = strlen(errbuf);
        if (len) {
            response.error_message = std::string(curl_easy_strerror(res)) + " - " + std::string(errbuf);
        } else {
            response.error_message = curl_easy_strerror(res);
        }
        response.success = false;
        return response;
    }

    response.body = response_body;
    response.success = (response.status_code >= 200 && response.status_code < 300);
#endif

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
        std::ofstream debug_file("/tmp/duckdb_delta_sharing_request.json");
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

                if (line_json->contains("file")) {
                    auto &file_top = line_json->at("file");
                    int64_t top_version = -1;
                    int64_t top_timestamp = -1;
                    if (file_top.contains("v")) {
                        top_version = file_top.at("v").get<int64_t>();
                    } else if (file_top.contains("version")) {
                        top_version = file_top.at("version").get<int64_t>();
                    }
                    
                    if (file_top.contains("timestamp")) {
                        top_timestamp = file_top.at("timestamp").get<int64_t>();
                    }

                    if (file_top.contains("deltaSingleAction")) {
                        auto &single_action = file_top.at("deltaSingleAction");
                        if (single_action.contains("add")) {
                            FileAction file;
                            ParseFileAction(JsonValue::FromInternal(&single_action.at("add")), file, "add");
                            if (top_version >= 0) file.version = top_version;
                            if (top_timestamp >= 0) file.timestamp = top_timestamp;
                            result.files.push_back(file);
                        } else if (single_action.contains("cdc") || single_action.contains("cdf")) {
                            std::string action_key = single_action.contains("cdc") ? "cdc" : "cdf";
                            FileAction file;
                            ParseFileAction(JsonValue::FromInternal(&single_action.at(action_key)), file, "cdf"); // Map both to 'cdf' / 'update' logic
                            if (top_version >= 0) file.version = top_version;
                            if (top_timestamp >= 0) file.timestamp = top_timestamp;
                            result.files.push_back(file);
                        } else if (single_action.contains("remove")) {
                            FileAction file;
                            ParseFileAction(JsonValue::FromInternal(&single_action.at("remove")), file, "remove");
                            if (top_version >= 0) file.version = top_version;
                            if (top_timestamp >= 0) file.timestamp = top_timestamp;
                            result.files.push_back(file);
                        }
                    } else {
                        FileAction file;
                        ParseFileAction(JsonValue::FromInternal(&file_top), file, "");
                        if (top_version >= 0) file.version = top_version;
                        if (top_timestamp >= 0) file.timestamp = top_timestamp;
                        result.files.push_back(file);
                    }
                } else if (line_json->contains("add")) {
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

static LogicalType ParseSparkType(const json &type_json) {
    if (type_json.is_string()) {
        std::string type = type_json.get<std::string>();
        if (type == "string") return LogicalType::VARCHAR;
        if (type == "integer") return LogicalType::INTEGER;
        if (type == "long") return LogicalType::BIGINT;
        if (type == "double") return LogicalType::DOUBLE;
        if (type == "float") return LogicalType::FLOAT;
        if (type == "boolean") return LogicalType::BOOLEAN;
        if (type == "timestamp") return LogicalType::TIMESTAMP_TZ;
        if (type == "timestamp_ntz") return LogicalType::TIMESTAMP;
        if (type == "date") return LogicalType::DATE;
        if (type == "binary") return LogicalType::BLOB;
        if (type == "byte") return LogicalType::TINYINT;
        if (type == "short") return LogicalType::SMALLINT;
        if (type == "variant") return LogicalType::VARIANT();
        // decimal(p,s)
        if (type.rfind("decimal(", 0) == 0) {
            size_t comma = type.find(',');
            size_t paren = type.find(')');
            if (comma != std::string::npos && paren != std::string::npos) {
                std::string p_str = type.substr(8, comma - 8);
                std::string s_str = type.substr(comma + 1, paren - comma - 1);
                try {
                    int p = std::stoi(p_str);
                    int s = std::stoi(s_str);
                    return LogicalType::DECIMAL(p, s);
                } catch (...) {
                    return LogicalType::VARCHAR;
                }
            }
        }
        return LogicalType::VARCHAR;
    } else if (type_json.is_object()) {
        std::string type = type_json.value("type", "");
        if (type == "struct") {
            child_list_t<LogicalType> children;
            if (type_json.contains("fields") && type_json.at("fields").is_array()) {
                for (auto &field : type_json.at("fields")) {
                    std::string name = field.value("name", "");
                    LogicalType child_type = LogicalType::VARCHAR;
                    if (field.contains("type")) {
                        child_type = ParseSparkType(field.at("type"));
                    }
                    children.push_back(make_pair(name, child_type));
                }
            }
            return LogicalType::STRUCT(children);
        } else if (type == "array") {
            LogicalType child_type = LogicalType::VARCHAR;
            if (type_json.contains("elementType")) {
                child_type = ParseSparkType(type_json.at("elementType"));
            }
            return LogicalType::LIST(child_type);
        } else if (type == "map") {
            LogicalType key_type = LogicalType::VARCHAR;
            LogicalType value_type = LogicalType::VARCHAR;
            if (type_json.contains("keyType")) {
                key_type = ParseSparkType(type_json.at("keyType"));
            }
            if (type_json.contains("valueType")) {
                value_type = ParseSparkType(type_json.at("valueType"));
            }
            return LogicalType::MAP(key_type, value_type);
        }
    }
    return LogicalType::VARCHAR;
}

void DeltaSharingClient::ParseSparkSchema(const std::string &schema_string, vector<LogicalType> &return_types, vector<string> &names, vector<string> &physical_names) {
    try {
        if (schema_string.empty()) return;

        json schema_json = json::parse(schema_string);
        if (!schema_json.contains("fields") || !schema_json.at("fields").is_array()) {
            return;
        }

        auto &fields = schema_json.at("fields");
        for (auto &field : fields) {
            if (!field.contains("name") || !field.contains("type")) {
                continue;
            }

            string name = field.at("name").get<string>();
            string physical_name = name;
            if (field.contains("metadata") && field.at("metadata").contains("delta.columnMapping.physicalName")) {
                physical_name = field.at("metadata").at("delta.columnMapping.physicalName").get<string>();
            }

            LogicalType parsed_type = ParseSparkType(field.at("type"));
            names.push_back(name);
            physical_names.push_back(physical_name);
            return_types.push_back(parsed_type);
        }
    } catch (...) {
        // Fallback: names/types will stay as they were (likely empty)
    }
}

} // namespace duckdb
