/*
  IOP shim for <stdlib.h>.

  The IOP has no libc: string/conversion routines come from the kernel's sysclib module, and there
  is no heap at all -- malloc/calloc/realloc/free are provided by this module's alloc.c on top of
  AllocSysMemory. libsmb2 is portable C and expects the standard header, so declare that surface
  here rather than editing vendored sources.
*/
#ifndef SMB2MAN_SHIM_STDLIB_H
#define SMB2MAN_SHIM_STDLIB_H

#include <stddef.h>
#include <sysclib.h>

void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);

#endif
