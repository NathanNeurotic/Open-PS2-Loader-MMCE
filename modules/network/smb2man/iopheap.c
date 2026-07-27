/*
  Minimal heap for libsmb2 on the IOP.

  libsmb2 is portable C and expects malloc/calloc/free/realloc. The IOP has no libc heap -- the
  kernel exposes only AllocSysMemory/FreeSysMemory, which allocate from the shared IOP RAM pool and
  hand back a bare pointer with no recorded size. FreeSysMemory needs only the pointer, but realloc
  and calloc need the size, so every block carries a small header holding it.

  The header is 16 bytes rather than 4 to keep every returned pointer 16-byte aligned: libsmb2
  memcpy()s packet data through these blocks, and the IOP's unaligned-access behaviour makes
  under-aligned buffers an intermittent-corruption risk rather than a clean fault.

  Named iopheap.c, not alloc.c: libsmb2 ships its own lib/alloc.c, and both would compile to the
  same obj/alloc.o -- which the linker reports as "multiple definition of realloc" pointing at one
  file, since the object is simply listed twice.

  These symbols are deliberately NOT exported -- they exist to satisfy libsmb2's link inside this
  IRX only, and must not become an accidental IOP-wide allocator.
*/

#include <stdint.h>

#include <sysclib.h>
#include <sysmem.h>

#define ALLOC_HDR_SIZE 16
#define ALLOC_MAGIC    0x5342324D // "SB2M"

struct alloc_hdr
{
    u32 magic;
    u32 size; // payload size, not including this header
    u32 pad[2];
};

void *malloc(size_t size)
{
    struct alloc_hdr *hdr;
    void *block;

    if (size == 0)
        return NULL;

    block = AllocSysMemory(ALLOC_FIRST, size + ALLOC_HDR_SIZE, NULL);
    if (block == NULL)
        return NULL;

    hdr = (struct alloc_hdr *)block;
    hdr->magic = ALLOC_MAGIC;
    hdr->size = size;

    return (void *)((u8 *)block + ALLOC_HDR_SIZE);
}

void free(void *ptr)
{
    struct alloc_hdr *hdr;

    if (ptr == NULL)
        return;

    hdr = (struct alloc_hdr *)((u8 *)ptr - ALLOC_HDR_SIZE);
    // A bad magic means the pointer did not come from here. Freeing it would corrupt the IOP pool,
    // so drop it instead -- leaking is survivable for a browse-time module, corruption is not.
    if (hdr->magic != ALLOC_MAGIC)
        return;

    hdr->magic = 0;
    FreeSysMemory((void *)hdr);
}

void *calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    void *ptr;

    // Reject the multiply overflowing; a wrapped size would under-allocate and the caller would
    // then write past the end.
    if (nmemb != 0 && (total / nmemb) != size)
        return NULL;

    ptr = malloc(total);
    if (ptr != NULL)
        memset(ptr, 0, total);

    return ptr;
}

void *realloc(void *ptr, size_t size)
{
    struct alloc_hdr *hdr;
    void *newptr;

    if (ptr == NULL)
        return malloc(size);

    if (size == 0) {
        free(ptr);
        return NULL;
    }

    hdr = (struct alloc_hdr *)((u8 *)ptr - ALLOC_HDR_SIZE);
    if (hdr->magic != ALLOC_MAGIC)
        return NULL;

    newptr = malloc(size);
    if (newptr == NULL)
        return NULL;

    memcpy(newptr, ptr, (hdr->size < size) ? hdr->size : size);
    free(ptr);

    return newptr;
}
