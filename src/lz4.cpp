// modules/isofs stays guard-free: wrap the text-included module body so the LZ4_*
// definitions keep C linkage (zso.cpp and IOP-side callers expect unmangled names).
extern "C" {
#include "../modules/isofs/lz4.c"
}