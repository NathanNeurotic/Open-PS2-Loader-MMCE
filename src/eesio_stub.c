#include <stdint.h>

/*
 * EESIO debug build expects ee_sio_start to exist.
 * Provide a weak no-op implementation so link succeeds. If a real implementation
 * is later added (or linked from a library), it will override this symbol.
 */
__attribute__((weak)) int ee_sio_start(const uint8_t *data, int size)
{
    (void)data;
    (void)size;
    return 0;
}
