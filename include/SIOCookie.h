#ifndef __SIOCOOKIE_H__
#define __SIOCOOKIE_H__

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Minimal stub for the EESIO debug build.
 *
 * Open PS2 Loader uses ee_sio_start() only to initialize serial output when
 * __EESIO_DEBUG is enabled (see src/opl.c). The CI environment for this fork
 * expects the header to exist, but it is not shipped in the tree.
 *
 * This declaration is sufficient for compilation; the actual implementation
 * is provided by the EESIO debug runtime/library in environments that use it.
 */
int ee_sio_start(int baud, int mode, int parity, int stopbits, int flow, int cookie);

#ifdef __cplusplus
}
#endif

#endif /* __SIOCOOKIE_H__ */
