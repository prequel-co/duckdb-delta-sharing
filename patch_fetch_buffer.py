import os

def patch_fetch_buffer():
    f_stmt = 'src/odbc_driver/statement/statement_functions.cpp'
    with open(f_stmt, 'r') as f:
        c = f.read()

    # 1. Fix signedness underflow comparison
    old_check = "if (null_terminate && buffer_length < sizeof(CHAR_TYPE)) {"
    new_check = "if (null_terminate && buffer_length < static_cast<SQLLEN>(sizeof(CHAR_TYPE))) {"
    
    # 2. Fix unaligned out_len copy
    old_align = """\tif (out_len > buffer_effective_size) {
\t\tout_len = buffer_effective_size;
\t\tret = duckdb::SetDiagnosticRecord("""
    
    new_align = """\tif (out_len > buffer_effective_size) {
\t\tout_len = buffer_effective_size;
\t\tout_len -= (out_len % sizeof(CHAR_TYPE)); // Align to character boundary
\t\tret = duckdb::SetDiagnosticRecord("""

    if old_check in c:
        c = c.replace(old_check, new_check)
    
    if old_align in c:
        c = c.replace(old_align, new_align)

    with open(f_stmt, 'w') as f:
        f.write(c)
    print("Patched statement_functions.cpp")

if __name__ == '__main__':
    patch_fetch_buffer()
