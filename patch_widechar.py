import os

def patch_widechar():
    f_utils = 'src/odbc_driver/common/odbc_utils.cpp'
    with open(f_utils, 'r') as f:
        c = f.read()

    old_align = """\t\tsize_t buf_len_even = static_cast<size_t>(buf_len_bytes);
\t\tif ((buf_len_bytes % 2) != 0) {
\t\t\tbuf_len_even -= 1;
\t\t}"""
    new_align = """\t\tsize_t buf_len_even = static_cast<size_t>(buf_len_bytes);
\t\tbuf_len_even -= (buf_len_even % sizeof(SQLWCHAR));"""

    if old_align in c:
        c = c.replace(old_align, new_align)
        with open(f_utils, 'w') as f:
            f.write(c)
        print("Patched odbc_utils.cpp")

    f_widechar = 'src/odbc_driver/widechar/widechar.cpp'
    with open(f_widechar, 'r') as f:
        c = f.read()

    old_coef = "static const size_t utf8_byte_len_coef = 3;"
    new_coef = "static const size_t utf8_byte_len_coef = sizeof(SQLWCHAR) == 4 ? 4 : 3;"
    
    old_invalid_check = "const SQLWCHAR *first_invalid_found = utf16_find_invalid(in_buf, in_buf_end);"
    new_invalid_check = """const SQLWCHAR *first_invalid_found;
\tif (sizeof(SQLWCHAR) == 4) {
\t\tfirst_invalid_found = in_buf_end;
\t} else {
\t\tfirst_invalid_found = utf16_find_invalid(in_buf, in_buf_end);
\t}"""

    old_utf16to8 = "utf8::unchecked::utf16to8(in_buf, in_buf_end, res_bi);"
    new_utf16to8 = """if (sizeof(SQLWCHAR) == 4) {
\t\t\tutf8::unchecked::utf32to8(in_buf, in_buf_end, res_bi);
\t\t} else {
\t\t\tutf8::unchecked::utf16to8(in_buf, in_buf_end, res_bi);
\t\t}"""

    old_replaced16to8 = "utf8::unchecked::utf16to8(replaced.begin(), replaced.end(), res_bi);"
    new_replaced16to8 = """if (sizeof(SQLWCHAR) == 4) {
\t\t\tutf8::unchecked::utf32to8(replaced.begin(), replaced.end(), res_bi);
\t\t} else {
\t\t\tutf8::unchecked::utf16to8(replaced.begin(), replaced.end(), res_bi);
\t\t}"""

    old_utf8to16 = "utf8::unchecked::utf8to16(in_buf, in_buf_end, res_bi);"
    new_utf8to16 = """if (sizeof(SQLWCHAR) == 4) {
\t\t\tutf8::unchecked::utf8to32(in_buf, in_buf_end, res_bi);
\t\t} else {
\t\t\tutf8::unchecked::utf8to16(in_buf, in_buf_end, res_bi);
\t\t}"""

    old_replaced8to16 = "utf8::unchecked::utf8to16(replaced.begin(), replaced.end(), res_bi);"
    new_replaced8to16 = """if (sizeof(SQLWCHAR) == 4) {
\t\t\tutf8::unchecked::utf8to32(replaced.begin(), replaced.end(), res_bi);
\t\t} else {
\t\t\tutf8::unchecked::utf8to16(replaced.begin(), replaced.end(), res_bi);
\t\t}"""

    if old_coef in c:
        c = c.replace(old_coef, new_coef)
    if old_invalid_check in c:
        c = c.replace(old_invalid_check, new_invalid_check)
    if old_utf16to8 in c:
        c = c.replace(old_utf16to8, new_utf16to8)
    if old_replaced16to8 in c:
        c = c.replace(old_replaced16to8, new_replaced16to8)
    if old_utf8to16 in c:
        c = c.replace(old_utf8to16, new_utf8to16)
    if old_replaced8to16 in c:
        c = c.replace(old_replaced8to16, new_replaced8to16)

    with open(f_widechar, 'w') as f:
        f.write(c)
    print("Patched widechar.cpp")

if __name__ == '__main__':
    patch_widechar()
