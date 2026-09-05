#ifndef STUB_SYSCLIB_H
#define STUB_SYSCLIB_H
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
static inline int look_ctype_table(int c) { return isalpha(c) ? 2 : 0; }
#endif
