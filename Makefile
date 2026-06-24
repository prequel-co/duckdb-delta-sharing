PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=duckdb_delta_sharing
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
-include extension-ci-tools/makefiles/duckdb_extension.Makefile

.PHONY: demo
demo:
	cd demo && pnpm install && pnpm dev