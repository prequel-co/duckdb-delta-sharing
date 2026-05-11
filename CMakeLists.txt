cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME duckdb_delta_sharing)
set(DUCKDB_DELTA_SHARING_EXTENSION_VERSION "0.2.0")

if("$ENV{DUCKDB_PLATFORM}" STREQUAL "windows_amd64")
    # DuckDB's CI tools sometimes configure windows_amd64 with the MSVC vcpkg triplet
    # but the environment actually uses MinGW GCC. Force the MinGW triplet to match the compiler.
    set(VCPKG_TARGET_TRIPLET "x64-mingw-static" CACHE STRING "Override vcpkg triplet" FORCE)
endif()

project(${TARGET_NAME})

# DuckDB's extension distribution supports vcpkg. As such, dependencies can be added in ./vcpkg.json and then
# used in cmake with find_package. Feel free to remove or replace with other dependencies.
# Note that it should also be removed from vcpkg.json to prevent needlessly installing it..
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

find_path(CROARING_INCLUDE_DIR roaring/roaring.h HINTS /opt/homebrew/include)
find_library(CROARING_LIBRARY NAMES roaring HINTS /opt/homebrew/lib)
if(CROARING_INCLUDE_DIR AND CROARING_LIBRARY)
    add_library(roaring UNKNOWN IMPORTED)
    set_target_properties(roaring PROPERTIES
        IMPORTED_LOCATION "${CROARING_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${CROARING_INCLUDE_DIR}"
    )
elseif(TARGET roaring::roaring)
    # Target already provided via VCPKG or config
else()
    include(FetchContent)
    FetchContent_Declare(
        roaring
        GIT_REPOSITORY https://github.com/RoaringBitmap/CRoaring.git
        GIT_TAG v4.6.1
    )
    FetchContent_MakeAvailable(roaring)
endif()

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

include_directories(src/include)

set(EXTENSION_SOURCES
    src/duckdb_delta_sharing_extension.cpp
    src/duckdb_delta_sharing_functions.cpp
    src/delta_share_multi_file_reader.cpp
    src/delta_sharing_client.cpp
    src/delta_sharing_json.cpp
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Link dependencies in both the static library as the loadable extension
target_link_libraries(${EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto CURL::libcurl nlohmann_json::nlohmann_json roaring)
target_link_libraries(${LOADABLE_EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto CURL::libcurl nlohmann_json::nlohmann_json roaring)

target_include_directories(${EXTENSION_NAME} PRIVATE "${CMAKE_SOURCE_DIR}/duckdb/extension/parquet/include")
target_include_directories(${LOADABLE_EXTENSION_NAME} PRIVATE "${CMAKE_SOURCE_DIR}/duckdb/extension/parquet/include")

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

if(NOT nlohmann_json_FOUND)
    install(TARGETS nlohmann_json EXPORT "${DUCKDB_EXPORT_SET}")
endif()

if(NOT CROARING_INCLUDE_DIR)
    install(TARGETS roaring 
        EXPORT "${DUCKDB_EXPORT_SET}"
        LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
        ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
endif()
