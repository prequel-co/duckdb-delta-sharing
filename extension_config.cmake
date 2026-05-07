# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(duckdb_delta_sharing
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

# Any extra extensions that should be built
# DuckDB >= 1.2+ automatically builds core extensions that are specified
# via the CORE_EXTENSIONS environment variable or CI tools, so we don't
# need to load them here.
# duckdb_extension_load(httpfs)
# duckdb_extension_load(parquet)
# duckdb_extension_load(json)
# duckdb_extension_load(icu)