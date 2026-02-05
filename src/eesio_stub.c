/*
 * EESIO / SIOCookie optional support stub
 *
 * Some build environments (including CI) may enable __EESIO_DEBUG without
 * providing an external libsiocookie that implements ee_sio_start().
 *
 * Provide a weak no-op implementation so the project links cleanly.
 * If a real implementation is linked in, it will override this one.
 */

#include "SIOCookie.h"

__attribute__((weak))
int ee_sio_start(void)
{
    return 0;
}
