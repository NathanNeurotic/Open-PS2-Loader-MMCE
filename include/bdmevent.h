#ifndef __BDMEVENT_H
#define __BDMEVENT_H

#include <sifcmd.h>

#define BDMEVENT_EE_EVENT_CMD       0
// SIFCMD has 32 system-handler slots. Use the last one for this diagnostic request rather than an
// out-of-range user command; the handler is installed only in OPL's private post-reset IOP image.
#define BDMEVENT_IOP_DIAG_QUERY_CMD (SIF_CMD_ID_SYSTEM | 31)

enum bdm_event_packet_kind {
    BDMEVENT_PACKET_EVENT = 1,
    BDMEVENT_PACKET_DIAG_SNAPSHOT,
};

typedef struct
{
    SifCmdHeader_t header;
    unsigned int kind;
    int cause;
    unsigned int callbackSequence;
    unsigned int mountCallbackCount;
    unsigned int unmountCallbackCount;
    unsigned int blockDeviceCount;
    unsigned int usbRootCount;
    unsigned int usbRootMask;
} bdm_event_packet_t;

#endif
