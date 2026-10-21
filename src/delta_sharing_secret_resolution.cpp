#include "delta_sharing_secret_resolution.hpp"

#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

#include <algorithm>

namespace duckdb {

static constexpr const char *DELTA_SHARING_SECRET_TYPE = "delta_sharing";
static constexpr const char *ENDPOINT_PARAMETER = "endpoint";
static constexpr const char *SECRET_PARAMETER = "secret";
static constexpr const char *NO_SECRET_MESSAGE =
    "LoadProfile error: Please configure Delta Sharing via a secret: CREATE SECRET (TYPE delta_sharing, PROVIDER "
    "config, ENDPOINT '...', BEARER_TOKEN '...') or CREATE SECRET (TYPE delta_sharing, PROVIDER env)";

static string RequireNonEmpty(const Value &value, const string &parameter) {
    if (value.IsNull() || value.ToString().empty()) {
        throw InvalidConfigurationException("delta_sharing: %s := must be a non-empty string", parameter);
    }
    return value.ToString();
}

DeltaSharingSecretRequest DeltaSharingSecretRequest::FromNamedParameters(const named_parameter_map_t &named_parameters) {
    DeltaSharingSecretRequest request;
    auto endpoint = named_parameters.find(ENDPOINT_PARAMETER);
    if (endpoint != named_parameters.end()) {
        request.endpoint = RequireNonEmpty(endpoint->second, ENDPOINT_PARAMETER);
    }
    auto secret = named_parameters.find(SECRET_PARAMETER);
    if (secret != named_parameters.end()) {
        request.secret = RequireNonEmpty(secret->second, SECRET_PARAMETER);
    }
    return request;
}

named_parameter_map_t DeltaSharingSecretRequest::WithoutRequestParameters(const named_parameter_map_t &named_parameters) {
    auto result = named_parameters;
    result.erase(ENDPOINT_PARAMETER);
    result.erase(SECRET_PARAMETER);
    return result;
}

void DeltaSharingSecretRequest::AddNamedParameters(TableFunction &function) {
    function.named_parameters[ENDPOINT_PARAMETER] = LogicalType::VARCHAR;
    function.named_parameters[SECRET_PARAMETER] = LogicalType::VARCHAR;
}

static string WithoutTrailingSlashes(const string &url) {
    auto end = url.find_last_not_of('/');
    return end == string::npos ? string() : url.substr(0, end + 1);
}

// Whether `base` has a scheme and a non-empty host, e.g. `https://host`.
//
// Why: `ENDPOINT 'https://'` trims (via WithoutTrailingSlashes) to `https:`,
// and `base + "/"` would then equal `https:/`, a prefix of every https URL.
// Require at least one character after `://` so a scheme-only ENDPOINT can
// never cover anything.
static bool HasSchemeAndHost(const string &base) {
    auto scheme_end = base.find("://");
    if (scheme_end == string::npos) {
        return false;
    }
    auto host_start = scheme_end + 3;
    return host_start < base.size() && base[host_start] != '/';
}

// Whether a single path segment is `.` or `..` once lower-cased and
// %2e-decoded (case-insensitively) to `.`.
//
// Why: libcurl strips dot segments per RFC 3986 §5.2.4 unless
// CURLOPT_PATH_AS_IS is set, so `base/sales/../ops` passes a literal-prefix
// check for `base/sales` but the request actually goes to `base/ops`. Catch
// the percent-encoded spellings too (`%2e`, `%2E`, `.%2e`, …), not just the
// literal dots.
static bool IsDotSegment(const string &segment) {
    auto lowered = StringUtil::Lower(segment);
    string decoded;
    decoded.reserve(lowered.size());
    for (idx_t i = 0; i < lowered.size();) {
        if (i + 3 <= lowered.size() && lowered.compare(i, 3, "%2e") == 0) {
            decoded += '.';
            i += 3;
        } else {
            decoded += lowered[i];
            i += 1;
        }
    }
    return decoded == "." || decoded == "..";
}

// Whether `requested` is `secret_endpoint` or extends it at a '/' boundary,
// ignoring trailing slashes on both, with no `.`/`..` segment in the
// extension (see IsDotSegment) and no scheme-only ENDPOINT (see
// HasSchemeAndHost).
static bool EndpointCovers(const string &secret_endpoint, const string &requested) {
    auto base = WithoutTrailingSlashes(secret_endpoint);
    auto target = WithoutTrailingSlashes(requested);
    if (base.empty() || !HasSchemeAndHost(base)) {
        return false;
    }
    if (target == base) {
        return true;
    }
    if (!StringUtil::StartsWith(target, base + "/")) {
        return false;
    }
    auto extension = target.substr(base.size() + 1);
    for (auto &segment : StringUtil::Split(extension, '/')) {
        if (IsDotSegment(segment)) {
            return false;
        }
    }
    return true;
}

const KeyValueSecret &ResolvedDeltaSharingSecret::Secret() const {
    auto key_value = dynamic_cast<const KeyValueSecret *>(entry->secret.get());
    if (!key_value) {
        throw InvalidConfigurationException("delta_sharing: secret '%s' is not a key-value secret",
                                            entry->secret->GetName());
    }
    return *key_value;
}

static bool IsDeltaSharing(const SecretEntry &entry) {
    return entry.secret && StringUtil::CIEquals(entry.secret->GetType(), DELTA_SHARING_SECRET_TYPE);
}

// The default rule, used when a call names neither an endpoint nor a secret.
//
//   ① the best unscoped secret: `LookupSecret("")` matches only secrets without
//     a SCOPE, ranked by DuckDB (storage tie-break offset, then name)
//   ② else the sole delta_sharing secret, scoped or not
//   ③ else refuse — picking among scoped secrets would be a guess
static unique_ptr<SecretEntry> ResolveDefault(SecretManager &manager, CatalogTransaction transaction) {
    auto unscoped = manager.LookupSecret(transaction, "", DELTA_SHARING_SECRET_TYPE);
    if (unscoped.HasMatch()) {
        return std::move(unscoped.secret_entry);
    }
    vector<SecretEntry> candidates;
    for (auto &entry : manager.AllSecrets(transaction)) {
        if (IsDeltaSharing(entry)) {
            candidates.push_back(entry);
        }
    }
    if (candidates.empty()) {
        throw InvalidConfigurationException(NO_SECRET_MESSAGE);
    }
    if (candidates.size() == 1) {
        return make_uniq<SecretEntry>(candidates[0]);
    }
    vector<string> names;
    for (auto &candidate : candidates) {
        names.push_back(candidate.secret->GetName());
    }
    std::sort(names.begin(), names.end());
    throw InvalidConfigurationException(
        "delta_sharing: %s delta_sharing secrets are scoped and none is unscoped (%s); pass endpoint := or "
        "secret := to choose one",
        to_string(names.size()), StringUtil::Join(names, ", "));
}

static unique_ptr<SecretEntry> ResolveByName(SecretManager &manager, CatalogTransaction transaction,
                                             const string &name) {
    auto entry = manager.GetSecretByName(transaction, name);
    if (!entry) {
        throw InvalidConfigurationException("delta_sharing: secret := '%s' does not exist", name);
    }
    if (!IsDeltaSharing(*entry)) {
        throw InvalidConfigurationException("delta_sharing: secret := '%s' is a %s secret, not delta_sharing", name,
                                            entry->secret->GetType());
    }
    return entry;
}

static unique_ptr<SecretEntry> ResolveByEndpoint(SecretManager &manager, CatalogTransaction transaction,
                                                 const string &endpoint) {
    auto match = manager.LookupSecret(transaction, endpoint, DELTA_SHARING_SECRET_TYPE);
    if (!match.HasMatch()) {
        throw InvalidConfigurationException("delta_sharing: no delta_sharing secret's SCOPE matches endpoint := '%s'",
                                            endpoint);
    }
    return std::move(match.secret_entry);
}

static string SecretEndpoint(const KeyValueSecret &secret) {
    auto value = secret.TryGetValue("endpoint", false);
    if (value.IsNull()) {
        return string();
    }
    return value.ToString();
}

ResolvedDeltaSharingSecret ResolveDeltaSharingSecret(ClientContext &context, const DeltaSharingSecretRequest &request) {
    auto &manager = SecretManager::Get(context);
    auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);

    ResolvedDeltaSharingSecret resolved;
    if (!request.secret.empty()) {
        resolved.entry = ResolveByName(manager, transaction, request.secret);
    } else if (!request.endpoint.empty()) {
        resolved.entry = ResolveByEndpoint(manager, transaction, request.endpoint);
    } else {
        resolved.entry = ResolveDefault(manager, transaction);
    }

    auto secret_endpoint = SecretEndpoint(resolved.Secret());
    if (secret_endpoint.empty()) {
        throw InvalidConfigurationException("delta_sharing: secret '%s' has no ENDPOINT",
                                            resolved.Secret().GetName());
    }
    if (request.endpoint.empty()) {
        resolved.endpoint = secret_endpoint;
        return resolved;
    }
    // Never send a secret's bearer to a URL outside its own ENDPOINT: an
    // unscoped secret matches every endpoint := at score 0.
    if (!EndpointCovers(secret_endpoint, request.endpoint)) {
        throw InvalidConfigurationException(
            "delta_sharing: secret '%s' has ENDPOINT '%s', which does not cover endpoint := '%s'; give each secret "
            "a SCOPE, or pass secret := to choose one",
            resolved.Secret().GetName(), secret_endpoint, request.endpoint);
    }
    resolved.endpoint = request.endpoint;
    return resolved;
}

} // namespace duckdb
