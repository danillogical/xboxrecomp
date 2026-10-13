# cmake -DSPV=<in.spv> -DOUT=<out.h> -DSYMBOL=<name> -P xbr_spirv_to_header.cmake
# Writes a SPIR-V binary as a C uint32_t array plus its byte size.
file(READ "${SPV}" _hex HEX)
string(LENGTH "${_hex}" _hexlen)
math(EXPR _bytes "${_hexlen} / 2")
math(EXPR _rem "${_bytes} % 4")
if(_bytes EQUAL 0 OR NOT _rem EQUAL 0)
    message(FATAL_ERROR "${SPV}: size ${_bytes} is not a non-empty multiple of 4")
endif()
if(NOT _hex MATCHES "^03022307")
    message(FATAL_ERROR "${SPV}: missing little-endian SPIR-V magic")
endif()
# Little-endian bytes -> one 32-bit word each.
string(REGEX REPLACE "(..)(..)(..)(..)" "0x\\4\\3\\2\\1u," _words "${_hex}")
# Eight words per line.
string(REGEX REPLACE "(([^,]+,){8})" "    \\1\n" _words "${_words}")
file(WRITE "${OUT}"
"/* Generated from ${SYMBOL}; do not edit. */
#include <stddef.h>
#include <stdint.h>
static const uint32_t ${SYMBOL}[] = {
${_words}
};
static const size_t ${SYMBOL}_size = ${_bytes};
")
