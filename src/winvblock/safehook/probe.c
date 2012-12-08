/**
 * Copyright (C) 2009-2012, Shao Miller <sha0.miller@gmail.com>.
 *
 * This file is part of WinVBlock, originally derived from WinAoE.
 *
 * WinVBlock is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * WinVBlock is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with WinVBlock.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * "Safe INT 0x13 hook" mini-driver
 */

#include <ntddk.h>

#include "portable.h"
#include "winvblock.h"
#include "wv_string.h"
#include "driver.h"
#include "bus.h"
#include "device.h"
#include "disk.h"
#include "mount.h"
#include "debug.h"
#include "bus.h"
#include "ramdisk.h"
#include "grub4dos.h"
#include "thread.h"
#include "filedisk.h"
#include "x86.h"
#include "safehook.h"
#include "memdisk.h"

/** Public function declarations */
DRIVER_INITIALIZE WvSafeHookDriverEntry;

/** Function declarations */
static DRIVER_ADD_DEVICE WvSafeHookDriveDevice;
static DRIVER_UNLOAD WvSafeHookUnload;
static VOID WvSafeHookInitialProbe(IN DEVICE_OBJECT * DeviceObject);
static BOOLEAN WvGrub4dosProcessSlot(SP_WV_G4D_DRIVE_MAPPING);
static BOOLEAN WvGrub4dosProcessSafeHook(
    PUCHAR,
    SP_X86_SEG16OFF16,
    WV_SP_PROBE_SAFE_MBR_HOOK
  );

/** Objects */
static S_WVL_MINI_DRIVER * WvSafeHookMiniDriver;

/** Function definitions */

/**
 * The mini-driver entry-point
 *
 * @param DriverObject
 *   The driver object provided by the caller
 *
 * @param RegistryPath
 *   The Registry path provided by the caller
 *
 * @return
 *   The status of the operation
 */
NTSTATUS WvSafeHookDriverEntry(
    IN DRIVER_OBJECT * drv_obj,
    IN UNICODE_STRING * reg_path
  ) {
    static S_WVL_MAIN_BUS_PROBE_REGISTRATION probe_reg;
    NTSTATUS status;

    /* TODO: Build the PDO and FDO IRP dispatch tables */

    /* Register this mini-driver */
    status = WvlRegisterMiniDriver(
        &WvSafeHookMiniDriver,
        drv_obj,
        WvSafeHookDriveDevice,
        WvSafeHookUnload
      );
    if (!NT_SUCCESS(status))
      goto err_register;
    ASSERT(WvSafeHookMiniDriver);

    probe_reg.Callback = WvSafeHookInitialProbe;
    WvlRegisterMainBusInitialProbeCallback(&probe_reg);

    return STATUS_SUCCESS;

    err_register:

    return status;
  }

/**
 * Drive a safe hook PDO with a safe hook FDO
 *
 * @param DriverObject
 *   The driver object provided by the caller
 *
 * @param PhysicalDeviceObject
 *   The PDO to probe and attach the safe hook FDO to
 *
 * @return
 *   The status of the operation
 */
static NTSTATUS STDCALL WvSafeHookDriveDevice(
    IN DRIVER_OBJECT * drv_obj,
    IN DEVICE_OBJECT * pdo
  ) {
    DBG("Refusing to drive PDO %p\n", (VOID *) pdo);
    return STATUS_NOT_SUPPORTED;
  }

/**
 * Release resources and unwind state
 *
 * @param DriverObject
 *   The driver object provided by the caller
 */
static VOID STDCALL WvSafeHookUnload(IN DRIVER_OBJECT * drv_obj) {
    ASSERT(drv_obj);
    (VOID) drv_obj;
    WvlDeregisterMiniDriver(WvSafeHookMiniDriver);
  }

/**
 * Main bus initial bus relations probe callback
 *
 * @param DeviceObject
 *   The main bus device
 */
static VOID WvSafeHookInitialProbe(IN DEVICE_OBJECT * dev_obj) {
    S_X86_SEG16OFF16 int_13h;
    NTSTATUS status;
    DEVICE_OBJECT * first_hook;
    S_WV_SAFEHOOK_PDO * safe_hook;

    ASSERT(dev_obj);
    (VOID) dev_obj;

    /* Probe the first INT 0x13 vector for a safe hook */
    int_13h.Segment = 0;
    int_13h.Offset = 0x13 * sizeof int_13h;
    status = WvlCreateSafeHookDevice(&int_13h, &first_hook);
    if (!NT_SUCCESS(status))
      return;
    ASSERT(first_hook);

    status = WvlAddDeviceToMainBus(first_hook);
    if (!NT_SUCCESS(status)) {
        DBG("Couldn't add safe hook to main bus\n");
        IoDeleteDevice(first_hook);
        return;
      }

    /* Hack */
    safe_hook = first_hook->DeviceExtension;
    ASSERT(safe_hook);
    safe_hook->ParentBus = WvBus.Fdo;
  }

WVL_M_LIB NTSTATUS STDCALL WvlCreateSafeHookDevice(
    IN S_X86_SEG16OFF16 * hook_addr,
    IN OUT DEVICE_OBJECT ** dev_obj
  ) {
    static const CHAR sig[] = "$INT13SF";
    const SIZE_T one_mib = 0x100000;
    PHYSICAL_ADDRESS phys_addr;
    UCHAR * phys_mem;
    NTSTATUS status;
    UINT32 hook_phys_addr;
    WV_S_PROBE_SAFE_MBR_HOOK * safe_mbr_hook;
    DEVICE_OBJECT * new_dev;

    ASSERT(hook_addr);
    ASSERT(dev_obj);

    /* Map the first MiB of memory */
    phys_addr.QuadPart = 0LL;
    phys_mem = MmMapIoSpace(phys_addr, one_mib, MmNonCached);
    if (!phys_mem) {
        DBG("Could not map low memory\n");
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto err_map;
      }

    /* Special-case the IVT entry for INT 0x13 */
    if (
        hook_addr->Segment == 0 &&
        hook_addr->Offset == (sizeof *hook_addr * 0x13)
      ) {
        hook_addr = (VOID *) (phys_mem + sizeof *hook_addr * 0x13);
      }

    hook_phys_addr = M_X86_SEG16OFF16_ADDR(hook_addr);
    DBG(
        "Probing for safe hook at %04X:%04X (0x%08X)...\n",
        hook_addr->Segment,
        hook_addr->Offset,
        hook_phys_addr
      );

    /* Check for the signature */
    safe_mbr_hook = (VOID *) (phys_mem + hook_phys_addr);
    if (!wv_memcmpeq(safe_mbr_hook->Signature, sig, sizeof sig - 1)) {
        DBG("Invalid safe INT 0x13 hook signature.  End of chain\n");
        goto err_sig;
      }

    /* Found one */
    DBG("Found safe hook with vendor ID: %.8s\n", safe_mbr_hook->VendorId);
    new_dev = WvSafeHookPdoCreate(hook_addr, safe_mbr_hook, NULL);
    if (!new_dev) {
        DBG("Could not create safe hook object\n");
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto err_new_dev;
      }

    /* TODO: This is a hack while porting to mini-driver style */
    {
        WV_S_DEV_EXT * dev_ext = new_dev->DeviceExtension;
        WvlInitializeResourceTracker(dev_ext->Usage);
        WvlIncrementResourceUsage(dev_ext->Usage);
      }

    *dev_obj = new_dev;
    status = STATUS_SUCCESS;
    goto out;

    IoDeleteDevice(new_dev);
    err_new_dev:

    err_sig:

    out:

    MmUnmapIoSpace(phys_mem, one_mib);
    err_map:

    return status;
  }

WVL_M_LIB S_WV_SAFEHOOK_PDO * STDCALL WvlGetSafeHook(
    IN DEVICE_OBJECT * dev_obj
  ) {
    WV_S_DEV_EXT * dev_ext;

    ASSERT(dev_obj);
    dev_ext = dev_obj->DeviceExtension;
    if (!dev_ext || dev_ext->MiniDriver != WvSafeHookMiniDriver)
      return NULL;

    return dev_obj->DeviceExtension;
  }

/** Process a GRUB4DOS drive mapping slot.  Probably belongs elsewhere */
static BOOLEAN WvGrub4dosProcessSlot(SP_WV_G4D_DRIVE_MAPPING slot) {
    WVL_E_DISK_MEDIA_TYPE media_type;
    UINT32 sector_size;

    /* Check for an empty mapping */
    if (slot->SectorCount == 0)
      return FALSE;
    DBG("GRUB4DOS SourceDrive: 0x%02x\n", slot->SourceDrive);
    DBG("GRUB4DOS DestDrive: 0x%02x\n", slot->DestDrive);
    DBG("GRUB4DOS MaxHead: %d\n", slot->MaxHead);
    DBG("GRUB4DOS MaxSector: %d\n", slot->MaxSector);
    DBG("GRUB4DOS DestMaxCylinder: %d\n", slot->DestMaxCylinder);
    DBG("GRUB4DOS DestMaxHead: %d\n", slot->DestMaxHead);
    DBG("GRUB4DOS DestMaxSector: %d\n", slot->DestMaxSector);
    DBG("GRUB4DOS SectorStart: 0x%08x\n", slot->SectorStart);
    DBG("GRUB4DOS SectorCount: %d\n", slot->SectorCount);

    if (slot->SourceODD) {
        media_type = WvlDiskMediaTypeOptical;
        sector_size = 2048;
      } else {
        media_type =
          (slot->SourceDrive & 0x80) ?
          WvlDiskMediaTypeHard :
          WvlDiskMediaTypeFloppy;
        sector_size = 512;
      }

    /* Check for a RAM disk mapping */
    if (slot->DestDrive == 0xFF) {
        WvRamdiskCreateG4dDisk(slot, media_type, sector_size);
      } else {
        WvFilediskCreateG4dDisk(slot, media_type, sector_size);
      }
    return TRUE;
  }

/** Process a GRUB4DOS "safe hook".  Probably belongs elsewhere */
static BOOLEAN WvGrub4dosProcessSafeHook(
    PUCHAR phys_mem,
    SP_X86_SEG16OFF16 segoff,
    WV_SP_PROBE_SAFE_MBR_HOOK safe_mbr_hook
  ) {
    enum {CvG4dSlots = 8};
    static const UCHAR sig[sizeof safe_mbr_hook->VendorId] = "GRUB4DOS";
    SP_WV_G4D_DRIVE_MAPPING g4d_map;
    #ifdef TODO_RESTORE_FILE_MAPPED_G4D_DISKS
    WV_SP_FILEDISK_GRUB4DOS_DRIVE_FILE_SET sets;
    #endif
    int i;
    BOOLEAN found = FALSE;

    if (!wv_memcmpeq(safe_mbr_hook->VendorId, sig, sizeof sig)) {
        DBG("Not a GRUB4DOS safe hook\n");
        return FALSE;
      }
    DBG("Processing GRUB4DOS safe hook...\n");

    #ifdef TODO_RESTORE_FILE_MAPPED_G4D_DISKS
    sets = wv_mallocz(sizeof *sets * CvG4dSlots);
    if (!sets) {
        DBG("Couldn't allocate GRUB4DOS file mapping!\n");
    #endif

    g4d_map = (SP_WV_G4D_DRIVE_MAPPING) (
        phys_mem + (((UINT32) segoff->Segment) << 4) + 0x20
      );
    /* Process each drive mapping slot */
    i = CvG4dSlots;
    while (i--)
      found |= WvGrub4dosProcessSlot(g4d_map + i);
    DBG("%sGRUB4DOS drives found\n", found ? "" : "No ");

    return TRUE;
  }
