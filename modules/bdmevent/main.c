#include <loadcore.h>
#include <bdm.h>
#include <sifcmd.h>
#include <irx.h>
#include "bdmevent.h"

IRX_ID("bdmevent", 1, 1);

static unsigned int callbackSequence;
static unsigned int mountCallbackCount;
static unsigned int unmountCallbackCount;

static void bdm_diag_fill_snapshot(bdm_event_packet_t *packet, unsigned int kind, int cause)
{
    struct block_device *devices[20];
    unsigned int i;

    bdm_get_bd(devices, sizeof(devices) / sizeof(devices[0]));

    packet->kind = kind;
    packet->cause = cause;
    packet->header.opt = cause;
    packet->callbackSequence = callbackSequence;
    packet->mountCallbackCount = mountCallbackCount;
    packet->unmountCallbackCount = unmountCallbackCount;
    packet->blockDeviceCount = 0;
    packet->usbRootCount = 0;
    packet->usbRootMask = 0;

    for (i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
        struct block_device *device = devices[i];

        if (device == NULL)
            continue;

        packet->blockDeviceCount++;
        if (device->parNr == 0 && device->name != NULL &&
            device->name[0] == 'u' && device->name[1] == 's' && device->name[2] == 'b' && device->name[3] == '\0') {
            packet->usbRootCount++;
            if (device->devNr < 32)
                packet->usbRootMask |= 1U << device->devNr;
        }
    }
}

static void bdm_callback(int cause)
{
    static bdm_event_packet_t EventCmdData;

    callbackSequence++;
    if (cause)
        mountCallbackCount++;
    else
        unmountCallbackCount++;

    bdm_diag_fill_snapshot(&EventCmdData, BDMEVENT_PACKET_EVENT, cause);
    sceSifSendCmd(BDMEVENT_EE_EVENT_CMD, &EventCmdData, sizeof(EventCmdData), NULL, NULL, 0);
}

static void bdm_diag_query_handler(void *packet, void *arg)
{
    static bdm_event_packet_t SnapshotCmdData;

    (void)packet;
    (void)arg;

    bdm_diag_fill_snapshot(&SnapshotCmdData, BDMEVENT_PACKET_DIAG_SNAPSHOT, -1);
    sceSifSendCmd(BDMEVENT_EE_EVENT_CMD, &SnapshotCmdData, sizeof(SnapshotCmdData), NULL, NULL, 0);
}

int _start(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    sceSifAddCmdHandler(BDMEVENT_IOP_DIAG_QUERY_CMD, &bdm_diag_query_handler, NULL);
    bdm_RegisterCallback(&bdm_callback);
    return MODULE_RESIDENT_END;
}
