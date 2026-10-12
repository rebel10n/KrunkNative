# cmake -DIN=<file> -DOUT=<file.c> -P embed.cmake: IN as `const unsigned char kn_schema_json[]`.
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" n)
math(EXPR len "${n} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," arr "${hex}")
file(WRITE "${OUT}" "/* generated from ${IN} by embed.cmake */\n#include <stddef.h>\nconst unsigned char kn_schema_json[] = {${arr}0x00};\nconst size_t kn_schema_json_len = ${len};\n")
