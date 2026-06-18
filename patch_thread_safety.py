import os, re

# 1. Patch duckdb_odbc.hpp
f_hpp = 'include/duckdb_odbc.hpp'
with open(f_hpp, 'r') as f:
    c = f.read()

if 'std::atomic<int> active_calls' not in c:
    # Add active_calls and is_deleted to OdbcHandle
    c = c.replace('OdbcHandleType type;', 'OdbcHandleType type;\n\tstd::atomic<int> active_calls {0};\n\tstd::atomic<bool> is_deleted {false};')
    # Add #include <atomic>
    c = c.replace('#include "duckdb/common/shared_ptr.hpp"', '#include "duckdb/common/shared_ptr.hpp"\n#include <atomic>\n#include <thread>')

    # Add ApiGuard to duckdb namespace
    guard_code = """
void FreeHandleMemory(OdbcHandle *handle);

struct ApiGuard {
    OdbcHandle *handle;
    ApiGuard(OdbcHandle *h) : handle(h) {
        if (!handle) return;
        auto *curr = handle;
        while (curr) {
            curr->active_calls++;
            if (curr->type == OdbcHandleType::STMT) curr = static_cast<OdbcHandleStmt *>(curr)->dbc;
            else if (curr->type == OdbcHandleType::DESC) curr = static_cast<OdbcHandleDesc *>(curr)->dbc;
            else if (curr->type == OdbcHandleType::DBC) curr = static_cast<OdbcHandleDbc *>(curr)->env;
            else curr = nullptr;
        }
    }
    ~ApiGuard() {
        if (!handle) return;
        auto *curr = handle;
        while (curr) {
            OdbcHandle *parent = nullptr;
            if (curr->type == OdbcHandleType::STMT) parent = static_cast<OdbcHandleStmt *>(curr)->dbc;
            else if (curr->type == OdbcHandleType::DESC) parent = static_cast<OdbcHandleDesc *>(curr)->dbc;
            else if (curr->type == OdbcHandleType::DBC) parent = static_cast<OdbcHandleDbc *>(curr)->env;
            
            if (--curr->active_calls == 0 && curr->is_deleted.load()) {
                FreeHandleMemory(curr);
            }
            curr = parent;
        }
    }
    bool IsValid() { return handle && !handle->is_deleted.load(); }
};
"""
    c = c.replace('} // namespace duckdb', guard_code + '\n} // namespace duckdb')
    with open(f_hpp, 'w') as f:
        f.write(c)

# 2. Patch duckdb::FreeHandle in src/odbc_driver/driver.cpp
f_driver = 'src/odbc_driver/driver.cpp'
with open(f_driver, 'r') as f:
    c = f.read()

if 'void duckdb::FreeHandleMemory' not in c:
    free_handle_replacement = """
void duckdb::FreeHandleMemory(OdbcHandle *handle) {
\tif (!handle) return;
\tswitch (handle->type) {
\tcase OdbcHandleType::DBC: {
\t\tauto *hdl = static_cast<duckdb::OdbcHandleDbc *>(handle);
\t\tdelete hdl;
\t\tbreak;
\t}
\tcase OdbcHandleType::DESC: {
\t\tauto *hdl = static_cast<duckdb::OdbcHandleDesc *>(handle);
\t\tif (hdl->dbc) hdl->dbc->ResetStmtDescriptors(hdl);
\t\tdelete hdl;
\t\tbreak;
\t}
\tcase OdbcHandleType::ENV: {
\t\tauto *hdl = static_cast<duckdb::OdbcHandleEnv *>(handle);
\t\tdelete hdl;
\t\tbreak;
\t}
\tcase OdbcHandleType::STMT: {
\t\tauto *hdl = static_cast<duckdb::OdbcHandleStmt *>(handle);
\t\tif (hdl && hdl->dbc) hdl->dbc->EraseStmtRef(hdl);
\t\tdelete hdl;
\t\tbreak;
\t}
\t}
}

SQLRETURN duckdb::FreeHandle(SQLSMALLINT handle_type, SQLHANDLE handle) {
\tif (!handle) {
\t\treturn SQL_INVALID_HANDLE;
\t}

\tauto *base_handle = static_cast<duckdb::OdbcHandle *>(handle);
\t
\t{
\t\tduckdb::ApiGuard guard(base_handle);
\t\tbase_handle->is_deleted = true;

\t\tif (handle_type == SQL_HANDLE_STMT) {
\t\t\tauto *hstmt = static_cast<duckdb::OdbcHandleStmt *>(handle);
\t\t\tif (hstmt->dbc && hstmt->dbc->conn) {
\t\t\t\thstmt->dbc->conn->Interrupt();
\t\t\t}
\t\t} else if (handle_type == SQL_HANDLE_DBC) {
\t\t\tauto *dbc = static_cast<duckdb::OdbcHandleDbc *>(handle);
\t\t\tif (dbc->conn) {
\t\t\t\tdbc->conn->Interrupt();
\t\t\t}
\t\t}
\t}

\treturn SQL_SUCCESS;
}
"""
    # Replace the existing FreeHandle entirely
    c = re.sub(
        r'SQLRETURN\s+duckdb::FreeHandle\([^\{]+\{[\s\S]*?return\s+SQL_INVALID_HANDLE;\s*\}[\s\n]*\}',
        free_handle_replacement,
        c
    )
    with open(f_driver, 'w') as f:
        f.write(c)

# 3. Inject ApiGuard into all ODBC API functions
api_dir = 'src/odbc_api'
for root, dirs, files in os.walk(api_dir):
    for filename in files:
        if filename.endswith('.cpp'):
            path = os.path.join(root, filename)
            with open(path, 'r') as f:
                content = f.read()
            
            # Replace ConvertHSTMT (with optional const)
            content = re.sub(
                r'((?:const\s+)?SQLRETURN\s+ret\s*=\s*ConvertHSTMT\w*\([^,]+,\s*([a-zA-Z0-9_]+)\);[\s\n]*if\s*\([^\{]+\{[\s\n]*return[^;]+;[\s\n]*\})',
                r'\1\n\tduckdb::ApiGuard guard(\2);\n\tif (\2 && \2->is_deleted) return SQL_INVALID_HANDLE;',
                content
            )

            # ConvertConnection (with optional const)
            content = re.sub(
                r'((?:const\s+)?SQLRETURN\s+ret\s*=\s*ConvertConnection\w*\([^,]+,\s*([a-zA-Z0-9_]+)\);[\s\n]*if\s*\([^\{]+\{[\s\n]*return[^;]+;[\s\n]*\})',
                r'\1\n\tduckdb::ApiGuard guard(\2);\n\tif (\2 && \2->is_deleted) return SQL_INVALID_HANDLE;',
                content
            )

            # ConvertEnvironment (with optional const)
            content = re.sub(
                r'((?:const\s+)?SQLRETURN\s+ret\s*=\s*ConvertEnvironment\w*\([^,]+,\s*([a-zA-Z0-9_]+)\);[\s\n]*if\s*\([^\{]+\{[\s\n]*return[^;]+;[\s\n]*\})',
                r'\1\n\tduckdb::ApiGuard guard(\2);\n\tif (\2 && \2->is_deleted) return SQL_INVALID_HANDLE;',
                content
            )

            # ConvertHandle (since NativeSQLInternal uses it)
            content = re.sub(
                r'((?:const\s+)?(?:auto|SQLRETURN)\s+ret\s*=\s*ConvertHandle\w*\([^,]+,\s*([a-zA-Z0-9_]+)\);[\s\n]*if\s*\([^\{]+\{[\s\n]*return[^;]+;[\s\n]*\})',
                r'\1\n\tduckdb::ApiGuard guard(\2);\n\tif (\2 && \2->is_deleted) return SQL_INVALID_HANDLE;',
                content
            )

            with open(path, 'w') as f:
                f.write(content)
