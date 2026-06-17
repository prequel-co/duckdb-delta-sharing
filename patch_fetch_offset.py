import os

def patch_fetch_offset():
    f_fetch = 'src/odbc_driver/common/odbc_fetch.cpp'
    with open(f_fetch, 'r') as f:
        c = f.read()

    # 1. Patch ColumnWise Loop
    old_col_loop = """\tfor (SQLULEN row_idx = first_row_to_fetch; row_idx < last_row_to_fetch; ++row_idx) {
\t\t++chunk_row;
\t\tSetRowStatus(row_idx, SQL_SUCCESS);"""

    new_col_loop = """\tSQLULEN rows_fetched = 0;
\tfor (SQLULEN row_idx = first_row_to_fetch; row_idx < last_row_to_fetch; ++row_idx, ++rows_fetched) {
\t\t++chunk_row;
\t\tSetRowStatus(rows_fetched, SQL_SUCCESS);"""

    if old_col_loop in c:
        c = c.replace(old_col_loop, new_col_loop)

    # 2. Patch RowWise Loop
    old_row_loop = """\tSQLULEN rows_fetched = 0;
\tfor (SQLULEN row_idx = first_row_to_fetch; row_idx < last_row_to_fetch; ++row_idx, ++rows_fetched) {
\t\t++chunk_row;
\t\tSetRowStatus(row_idx, SQL_SUCCESS);
\t\tauto row_offset = row_size * row_idx;"""

    new_row_loop = """\tSQLULEN rows_fetched = 0;
\tfor (SQLULEN row_idx = first_row_to_fetch; row_idx < last_row_to_fetch; ++row_idx, ++rows_fetched) {
\t\t++chunk_row;
\t\tSetRowStatus(rows_fetched, SQL_SUCCESS);
\t\tauto row_offset = row_size * rows_fetched;"""

    if old_row_loop in c:
        c = c.replace(old_row_loop, new_row_loop)

    # 3. Patch pointer offsets
    c = c.replace("target_val_addr = (uint8_t *)target_val_addr + (row_idx * pointer_size);", "target_val_addr = (uint8_t *)target_val_addr + (rows_fetched * pointer_size);")
    c = c.replace("target_len_addr += row_idx;", "target_len_addr += rows_fetched;")

    # 4. Patch SetRowStatus error calls
    c = c.replace("SetRowStatus(row_idx, SQL_ROW_ERROR);", "SetRowStatus(rows_fetched, SQL_ROW_ERROR);")

    with open(f_fetch, 'w') as f:
        f.write(c)
    print("Patched odbc_fetch.cpp")

if __name__ == '__main__':
    patch_fetch_offset()
