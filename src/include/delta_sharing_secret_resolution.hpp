#pragma once

#include "duckdb.hpp"
#include "duckdb/common/named_parameter_map.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

namespace duckdb {

// Which delta_sharing secret a delta_share_* call asked for, from its
// `endpoint :=` and `secret :=` named parameters. Both empty = the default rule.
struct DeltaSharingSecretRequest {
    string endpoint;
    string secret;

    static DeltaSharingSecretRequest FromNamedParameters(const named_parameter_map_t &named_parameters);
    // A copy without `endpoint`/`secret`, for binds that forward to read_parquet.
    static named_parameter_map_t WithoutRequestParameters(const named_parameter_map_t &named_parameters);
    static void AddNamedParameters(TableFunction &function);
};

struct ResolvedDeltaSharingSecret {
    unique_ptr<SecretEntry> entry;
    // The requested endpoint when one was given, otherwise the secret's ENDPOINT.
    string endpoint;

    const KeyValueSecret &Secret() const;
};

ResolvedDeltaSharingSecret ResolveDeltaSharingSecret(ClientContext &context, const DeltaSharingSecretRequest &request);

} // namespace duckdb
