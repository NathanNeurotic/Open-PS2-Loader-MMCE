/*
  Copyright 2009-2010, jimmikaelkael
  Licenced under Academic Free License version 3.0
  Review Open PS2 Loader README & LICENSE files for further details.
*/

#include "internal.h"

#include <bdm.h>
#include <bd_defrag.h>

#include "device.h"

#ifdef USE_BDM_ATA
#include "atad.h"
char lba_48bit = 0;
char atad_inited = 0;
#endif

extern struct cdvdman_settings_bdm cdvdman_settings;
static struct block_device *g_bd = NULL;
static u32 g_bd_sectors_per_sector = 4;
static int bdm_io_sema;

extern struct irx_export_table _exp_bdm;

#ifdef USE_BDM_ATA
extern struct irx_export_table _exp_atad;
#endif

#ifdef USE_BDM_ATA
static int bdm_is_ata_driver_name(const char *name)
{
    return (name != NULL) &&
           (name[0] == 'a') &&
           (name[1] == 't') &&
           (name[2] == 'a') &&
           (name[3] == '\0');
}
#endif

/* IEEE1394_bd is the ONLY transport that answers to TWO driver tokens: the SDK binary carries both
   "sd" and "ilink", and OPL's own bdmDriverIsIlink() accepts either. Everything else has exactly one
   ("usb", "ata"; mx4sio_bd's second string is not used as a block-device name).

   That matters here because the launch binding is recorded in the MENU, from
   USBMASS_IOCTL_GET_DRIVERNAME, while the block device the game sees is registered by a FRESH
   IEEE1394_bd after the IOP reset. If those two disagree about which of its own aliases to use, the
   strncmp below fails, g_bd is never attached, bdm_io_sema is never signalled, and the game waits on
   it forever -- a silent hang with no error, on iLink only, while USB and ATA are unaffected.

   Treating the two as one identity costs nothing: no other driver reports either token, so this can
   never widen a match for USB, MX4SIO or ATA. */
static int bdm_is_ilink_driver_name(const char *name)
{
    if (name == NULL)
        return 0;
    return (name[0] == 's' && name[1] == 'd' && name[2] == '\0') ||
           (name[0] == 'i' && name[1] == 'l' && name[2] == 'i' && name[3] == 'n' &&
            name[4] == 'k' && name[5] == '\0');
}

static int bdm_matches_launch_device(const struct block_device *bd)
{
    if (bd == NULL)
        return 0;

    if (cdvdman_settings.bdDeviceDriver[0] == '\0')
        return bd->devNr == cdvdman_settings.bdDeviceId;

#ifdef USE_BDM_ATA
    /* The ATA-specific BDM cdvdman only ever exposes a single synthetic "ata"
       block device from atad.c, so binding by driver token is sufficient. */
    if (bdm_is_ata_driver_name(bd->name))
        return bdm_is_ata_driver_name(cdvdman_settings.bdDeviceDriver);
#endif

    if (bd->devNr != cdvdman_settings.bdDeviceId || bd->name == NULL)
        return 0;

    /* iLink answers to either of its own two tokens -- see bdm_is_ilink_driver_name above. */
    if (bdm_is_ilink_driver_name(bd->name) && bdm_is_ilink_driver_name(cdvdman_settings.bdDeviceDriver))
        return 1;

    return strncmp(bd->name, cdvdman_settings.bdDeviceDriver, sizeof(cdvdman_settings.bdDeviceDriver)) == 0;
}

//
// BDM exported functions
//

void bdm_connect_bd(struct block_device *bd)
{
    DPRINTF("connecting device %s%dp%d\n", bd->name, bd->devNr, bd->parNr);

    if (g_bd == NULL && bdm_matches_launch_device(bd)) {
        DPRINTF("attaching to %s%dp%d\n", bd->name, bd->devNr, bd->parNr);
        g_bd = bd;
        g_bd_sectors_per_sector = (2048 / bd->sectorSize);
        // Free usage of block device
        SignalSema(bdm_io_sema);
    }
}

void bdm_disconnect_bd(struct block_device *bd)
{
    DPRINTF("disconnecting device %s%dp%d\n", bd->name, bd->devNr, bd->parNr);

    if (bdm_matches_launch_device(bd)) {
        DPRINTF("detatching from %s%dp%d\n", bd->name, bd->devNr, bd->parNr);

        // Lock usage of block device
        WaitSema(bdm_io_sema);
        if (g_bd == bd)
            g_bd = NULL;
    }
}

//
// cdvdman "Device" functions
//

void DeviceInit(void)
{
    iop_sema_t smp;

    DPRINTF("%s\n", __func__);

    // Create semaphore, initially locked
    smp.initial = 0;
    smp.max = 1;
    smp.option = 0;
    smp.attr = SA_THPRI;
    bdm_io_sema = CreateSema(&smp);

    RegisterLibraryEntries(&_exp_bdm);

#ifdef USE_BDM_ATA
    RegisterLibraryEntries(&_exp_atad);
    // Initialize ATA interface which will register the HDD as a block device.
    atad_start();
    atad_inited = 1;
#endif
}

void DeviceDeinit(void)
{
    DPRINTF("%s\n", __func__);
}

int DeviceReady(void)
{
    // DPRINTF("%s\n", __func__);

    return (g_bd == NULL) ? SCECdNotReady : SCECdComplete;
}

void DeviceStop(void)
{
    DPRINTF("%s\n", __func__);

    if (g_bd != NULL)
        g_bd->stop(g_bd);
}

void DeviceFSInit(void)
{
#ifdef USE_BDM_ATA
    lba_48bit = cdvdman_settings.hddIsLBA48;
    // TODO: there's more cdvdman init stuff after this in device-hdd.c...
#endif

    DPRINTF("Waiting for device...\n");
    WaitSema(bdm_io_sema);
    DPRINTF("Waiting for device...done!\n");
    SignalSema(bdm_io_sema);
}

void DeviceLock(void)
{
    DPRINTF("%s\n", __func__);

    WaitSema(bdm_io_sema);
}

void DeviceUnmount(void)
{
    DPRINTF("%s\n", __func__);
}

int DeviceReadSectors(u64 lsn, void *buffer, unsigned int sectors)
{
    int rv = SCECdErNO;

    // DPRINTF("%s(%u, 0x%p, %u)\n", __func__, (unsigned int)lsn, buffer, sectors);

    if (g_bd == NULL)
        return SCECdErTRMOPN;

    WaitSema(bdm_io_sema);
    if (bd_defrag(g_bd, cdvdman_settings.fragfile[0].frag_count, &cdvdman_settings.frags[cdvdman_settings.fragfile[0].frag_start], lsn * 4, buffer, sectors * 4) != (sectors * 4))
        rv = SCECdErREAD;
    SignalSema(bdm_io_sema);

    return rv;
}

//
// oplutils exported function, used by MCEMU
//

void bdm_readSector(u64 lba, unsigned short int nsectors, unsigned char *buffer)
{
    DPRINTF("%s\n", __func__);

    WaitSema(bdm_io_sema);
    g_bd->read(g_bd, lba, buffer, nsectors);
    SignalSema(bdm_io_sema);
}

void bdm_writeSector(u64 lba, unsigned short int nsectors, const unsigned char *buffer)
{
    DPRINTF("%s\n", __func__);

    WaitSema(bdm_io_sema);
    g_bd->write(g_bd, lba, buffer, nsectors);
    SignalSema(bdm_io_sema);
}
