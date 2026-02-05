#include <stdint.h>

/*
 * EESIO debug builds in this tree call ee_sio_start() from src/opl.c when
 * __EESIO_DEBUG is enabled, but the implementation is not present in this repo.
 *
 * Provide a weak no-op implementation so link succeeds. If a real implementation
 * is later added (or linked from a library), it will override this symbol.
 */
__attribute__((weak))
int ee_sio_start(const uint8_t *data, int size)
{
    (void)data;
    (void)size;
    return 0;
}
