/**
 * Copyright (C) 2009-2012, Shao Miller <sha0.miller@gmail.com>.
 * Copyright 2006-2008, V.
 * For WinAoE contact information, see http://winaoe.org/
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
 * Driver specifics.
 */

#include <ntddk.h>
#include <scsi.h>

#include "portable.h"
#include "winvblock.h"
#include "thread.h"
#include "wv_stdlib.h"
#include "wv_string.h"
#include "irp.h"
#include "driver.h"
#include "bus.h"
#include "device.h"
#include "disk.h"
#include "registry.h"
#include "mount.h"
#include "filedisk.h"
#include "ramdisk.h"
#include "debug.h"

/* From mainbus/mainbus.c */
extern WVL_S_BUS_T WvBus;
extern DRIVER_INITIALIZE WvMainBusDriverEntry;
extern NTSTATUS STDCALL WvBusAttach(IN PDEVICE_OBJECT);

/** Public objects */
DRIVER_OBJECT * WvDriverObj;
UINT32 WvFindDisk;
KSPIN_LOCK WvFindDiskLock;
S_WVL_RESOURCE_TRACKER WvDriverUsage[1];
WVL_M_LIB BOOLEAN WvlCddbDone;

/** Private objects */

/** Power state handle */
static VOID * WvDriverStateHandle;

/** Is the driver started? */
static BOOLEAN WvDriverStarted;

/** Contains TXTSETUP.SIF/BOOT.INI-style OsLoadOptions parameters */
static WCHAR * WvOsLoadOpts;

/** The list of registered mini-drivers */
static S_WVL_LOCKED_LIST WvRegisteredMiniDrivers[1];

/** Notice for WvDeregisterMiniDrivers that the registration list is empty */
static KEVENT WvMiniDriversDeregistered;

/* Private function declarations */
static VOID WvDriverCheckCddb(IN UNICODE_STRING * RegistryPath);
static DRIVER_DISPATCH WvIrpNotSupported;
static
  __drv_dispatchType(IRP_MJ_POWER)
  __drv_dispatchType(IRP_MJ_CREATE)
  __drv_dispatchType(IRP_MJ_CLOSE)
  __drv_dispatchType(IRP_MJ_SYSTEM_CONTROL)
  __drv_dispatchType(IRP_MJ_DEVICE_CONTROL)
  __drv_dispatchType(IRP_MJ_SCSI)
  __drv_dispatchType(IRP_MJ_PNP)
  DRIVER_DISPATCH WvDriverDispatchIrp;
static DRIVER_UNLOAD WvUnload;
/* TODO: DRIVER_ADD_DEVICE isn't available in DDK 3790.1830, it seems */
static DRIVER_ADD_DEVICE WvDriveDevice;
static DRIVER_UNLOAD WvUnloadMiniDriver;
static VOID WvDeregisterMiniDrivers(void);
static NTSTATUS WvStartDeviceThread(IN DEVICE_OBJECT * Device);
static VOID WvStopDeviceThread(IN DEVICE_OBJECT * Device);
static F_WVL_DEVICE_THREAD_FUNCTION WvStopDeviceThreadInThread;
static F_WVL_DEVICE_THREAD_FUNCTION WvTestDeviceThread;
static LONG WvIncrementActiveIrpCount(WV_S_DEV_EXT * DeviceExtension);
static LONG WvDecrementActiveIrpCount(WV_S_DEV_EXT * DeviceExtension);
static NTSTATUS STDCALL WvDriverAddIrpToDeviceQueueInternal(
    IN DEVICE_OBJECT * DeviceObject,
    IN IRP * Irp,
    IN BOOLEAN Wait,
    IN BOOLEAN Internal
  );
/* KSTART_ROUTINE isn't available in DDK 3790.1830, it seems */
static VOID WvDeviceThread(VOID * Context);
static VOID WvProcessDeviceThreadWorkItem(
    IN DEVICE_OBJECT * Device,
    IN S_WVL_DEVICE_THREAD_WORK_ITEM * WorkItem,
    IN BOOLEAN DeviceNotAvailable
  );
static VOID WvProcessDeviceIrp(
    IN DEVICE_OBJECT * Device,
    IN IRP * Irp,
    IN BOOLEAN DeviceNotAvailable
  );
static IO_COMPLETION_ROUTINE WvIoCompletion;
static F_WVL_DEVICE_THREAD_FUNCTION WvDriverWaitForActiveIrps;

static LPWSTR STDCALL WvGetOpt(IN LPWSTR opt_name) {
    LPWSTR our_opts, the_opt;
    WCHAR our_sig[] = L"WINVBLOCK=";
    /* To produce constant integer expressions. */
    enum {
        our_sig_len_bytes = sizeof ( our_sig ) - sizeof ( WCHAR ),
        our_sig_len = our_sig_len_bytes / sizeof ( WCHAR )
      };
    size_t opt_name_len, opt_name_len_bytes;

    if (!WvOsLoadOpts || !opt_name)
      return NULL;

    /* Find /WINVBLOCK= options. */
    our_opts = WvOsLoadOpts;
    while (*our_opts != L'\0') {
        if (!wv_memcmpeq(our_opts, our_sig, our_sig_len_bytes)) {
            our_opts++;
            continue;
          }
        our_opts += our_sig_len;
        break;
      }

    /* Search for the specific option. */
    the_opt = our_opts;
    opt_name_len = wcslen(opt_name);
    opt_name_len_bytes = opt_name_len * sizeof (WCHAR);
    while (*the_opt != L'\0' && *the_opt != L' ') {
        if (!wv_memcmpeq(the_opt, opt_name, opt_name_len_bytes)) {
            while (*the_opt != L'\0' && *the_opt != L' ' && *the_opt != L',')
              the_opt++;
            continue;
          }
        the_opt += opt_name_len;
        break;
      }

    if (*the_opt == L'\0' || *the_opt == L' ')
      return NULL;

    /* Next should come "=". */
    if (*the_opt != L'=')
      return NULL;

    /*
     * And finally our option's value.  The caller needs
     * to worry about looking past the end of the option.
     */
    the_opt++;
    if (*the_opt == L'\0' || *the_opt == L' ')
      return NULL;
    return the_opt;
  }

static VOID STDCALL WvDriverReinitialize(
    IN PDRIVER_OBJECT driver_obj,
    IN PVOID context,
    ULONG count
  ) {
    static U_WV_LARGE_INT delay_time = {-10000000LL};
    KIRQL irql;
    UINT32 find_disk;

    DBG("Called\n");

    /* Check if any threads still need to find their disk. */
    KeAcquireSpinLock(&WvFindDiskLock, &irql);
    find_disk = WvFindDisk;
    KeReleaseSpinLock(&WvFindDiskLock, irql);

    /* Should we retry? */
    if (!find_disk || count >= 10) {
        WvlDecrementResourceUsage(WvDriverUsage);
        DBG("Exiting...\n");
        return;
      }

    /* Yes, we should. */
    IoRegisterBootDriverReinitialization(
        WvDriverObj,
        WvDriverReinitialize,
        NULL
      );
    /* Sleep. */
    DBG("Sleeping...");
    KeDelayExecutionThread(KernelMode, FALSE, &delay_time.large_int);
    return;
  }

/**
 * @name DriverEntry
 *
 * The driver entry-point
 *
 * @param drv_obj
 *   The driver object provided by Windows
 *
 * @param reg_path
 *   The Registry path provided by Windows
 *
 * @return
 *   The status
 */
NTSTATUS STDCALL DriverEntry(
    IN DRIVER_OBJECT * drv_obj,
    IN UNICODE_STRING * reg_path
  ) {
    NTSTATUS status;
    ULONG i;

    ASSERT(drv_obj);
    ASSERT(reg_path);

    DBG("Entry\n");

    /* Dummy to keep libbus linked-in */
    { volatile INT x = 0; if (x) WvlBusPnp(NULL, NULL); }

    /* Have we already initialized the driver? */
    if (WvDriverObj) {
        DBG("Re-entry not allowed!\n");
        return STATUS_NOT_SUPPORTED;
      }
    WvDriverObj = drv_obj;

    /* Have we already started the driver? */
    if (WvDriverStarted)
      return STATUS_SUCCESS;

    /* Track resource usage */
    WvlInitializeResourceTracker(WvDriverUsage);

    WvlDebugModuleInit();

    /* Check OS loader options */
    status = WvlRegNoteOsLoadOpts(&WvOsLoadOpts);
    if (!NT_SUCCESS(status)) {
        WvlDebugModuleUnload();
        return WvlError("WvlRegNoteOsLoadOpts", status);
      }

    /* Check if CDDB associations have been produced by the .INF file */
    WvDriverCheckCddb(reg_path);

    /* AoE doesn't support sleeping */
    WvDriverStateHandle = PoRegisterSystemState(NULL, ES_CONTINUOUS);
    if (!WvDriverStateHandle)
      DBG("Could not set system state to ES_CONTINUOUS!!\n");

    KeInitializeSpinLock(&WvFindDiskLock);

    /*
     * Set up IRP MajorFunction function table for devices
     * this driver handles
     */
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
      drv_obj->MajorFunction[i] = WvIrpNotSupported;
    drv_obj->MajorFunction[IRP_MJ_PNP] = WvDriverDispatchIrp;
    drv_obj->MajorFunction[IRP_MJ_POWER] = WvDriverDispatchIrp;
    drv_obj->MajorFunction[IRP_MJ_CREATE] = WvDriverDispatchIrp;
    drv_obj->MajorFunction[IRP_MJ_CLOSE] = WvDriverDispatchIrp;
    drv_obj->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = WvDriverDispatchIrp;
    drv_obj->MajorFunction[IRP_MJ_DEVICE_CONTROL] = WvDriverDispatchIrp;
    drv_obj->MajorFunction[IRP_MJ_SCSI] = WvDriverDispatchIrp;

    /* Set the driver Unload callback */
    drv_obj->DriverUnload = WvUnload;

    /* Set the driver AddDevice callback */
    drv_obj->DriverExtension->AddDevice = WvDriveDevice;

    /* Initialize the list of registered mini-drivers */
    WvlInitializeLockedList(WvRegisteredMiniDrivers);
    KeInitializeEvent(&WvMiniDriversDeregistered, NotificationEvent, TRUE);
    WvlIncrementResourceUsage(WvDriverUsage);

    /*
     * Invoke internal mini-drivers.  The order here determines which
     * mini-drivers have a higher priority for driving devices
     */
    status = WvMainBusDriverEntry(drv_obj, reg_path);
    if(!NT_SUCCESS(status))
      goto err_internal_minidriver;

    /* Register re-initialization routine to allow for disks to arrive */
    WvlIncrementResourceUsage(WvDriverUsage);
    IoRegisterBootDriverReinitialization(
        WvDriverObj,
        WvDriverReinitialize,
        NULL
      );

    WvDriverStarted = TRUE;
    DBG("Exit\n");
    return STATUS_SUCCESS;

    err_internal_minidriver:

    /* Release resources and unwind state */
    WvUnload(drv_obj);
    DBG("Exit due to failure\n");
    return status;
  }

/**
 * Drive a supported device
 *
 * @param DriverObject
 *   Ignored.  The driver object provided by Windows
 *
 * @param PhysicalDeviceObject
 *   The PDO to probe and attach an FDO to
 *
 * @return
 *   The status of the operation
 */
static NTSTATUS STDCALL WvDriveDevice(
    IN PDRIVER_OBJECT driver_obj,
    IN PDEVICE_OBJECT pdo
  ) {
    LIST_ENTRY * link;
    S_WVL_MINI_DRIVER * minidriver;
    NTSTATUS status;

    /* Ignore the main driver object that Windows will pass */
    (VOID) driver_obj;

    /* Assume failure */
    status = STATUS_NOT_SUPPORTED;

    /*
     * We need to own the registered mini-drivers list until we've
     * finished working with it.  Unfortunately, there is a slight race
     * here where:
     *
     * 1. An external mini-driver's Unload routine could be invoked,
     *    meaning the mini-driver should not be driving any more devices
     *
     * 2. This very function is invoked by Windows to attempt to drive a
     *    device
     *
     * 3. This function examines the list of registered drivers before
     *    the mini-driver's Unload routine has a chance to deregister
     *
     * Hopefully if the mini-driver calls IoCreateDevice while it's in an
     * "unloading" state (from Windows' perspective), the request will be
     * refused.  That would help for the mini-driver's AddDevice to fail
     * and return control back to this function.
     *
     * Just in case, the mini-driver's Unload routine's call to
     * WvlDeregisterMiniDriver will stall until all of the mini-driver's
     * devices have been deleted.  As long as the Unload routine hasn't
     * performed any clean-up before deregistering, the mini-driver
     * can continue to serve its devices, as awkward as it might be
     */
    WvlAcquireLockedList(WvRegisteredMiniDrivers);
    for (
        link = WvRegisteredMiniDrivers->List->Flink;
        link != WvRegisteredMiniDrivers->List;
        link = link->Flink
      ) {
        ASSERT(link);
        minidriver = CONTAINING_RECORD(link, S_WVL_MINI_DRIVER, Link);
        ASSERT(minidriver);

        /* Skip this mini-driver if it has no AddDevice routine */
        if (!minidriver->AddDevice)
          continue;

        ASSERT(minidriver->DriverObject);
        ASSERT(pdo);
        status = minidriver->AddDevice(minidriver->DriverObject, pdo);

        /* If the mini-driver drives the device, we're done */
        if (NT_SUCCESS(status))
          break;
      }
    WvlReleaseLockedList(WvRegisteredMiniDrivers);

    return status;
  }

static NTSTATUS STDCALL WvIrpNotSupported(
    IN PDEVICE_OBJECT dev_obj,
    IN PIRP irp
  ) {
    return WvlIrpComplete(irp, 0, STATUS_NOT_SUPPORTED);
  }

/**
 * Release resources and unwind state
 *
 * @param DriverObject
 *   The driver object provided by Windows
 */
static VOID STDCALL WvUnload(IN DRIVER_OBJECT * drv_obj) {
    DBG("Unloading...\n");

    WvDeregisterMiniDrivers();

    if (WvDriverStateHandle != NULL)
      PoUnregisterSystemState(WvDriverStateHandle);
    wv_free(WvOsLoadOpts);
    WvDriverStarted = FALSE;
    WvlDebugModuleUnload();

    WvlWaitForResourceZeroUsage(WvDriverUsage);

    DBG("Done\n");
  }

/**
 * Check if CriticalDeviceDatabase associations have been produced
 * by installation with the .INF file
 *
 * @param RegistryPath
 *   The Registry path for the driver, provided by Windows
 *
 * Sets the value of WvDriverCddbDone, accordingly
 */
static VOID WvDriverCheckCddb(IN UNICODE_STRING * reg_path) {
    HANDLE reg_key;
    UINT32 cddb_done;
    NTSTATUS status;

    /* Open our Registry path */
    ASSERT(reg_path);
    status = WvlRegOpenKey(reg_path->Buffer, &reg_key);
    if (!NT_SUCCESS(status)) {
        DBG("Couldn't open Registry path!\n");
        return;
      }

    /*
     * Check the Registry to see if we've already got a PDO.
     * This entry is produced when the PDO has been properly
     * installed via the .INF file
     */
    cddb_done = 0;
    status = WvlRegFetchDword(reg_key, L"CddbDone", &cddb_done);
    if (NT_SUCCESS(status) && cddb_done) {
        DBG("CddbDone was set\n");
        WvlCddbDone = TRUE;
      }

    WvlRegCloseKey(reg_key);
  }

NTSTATUS STDCALL WvDriverGetDevCapabilities(
    IN PDEVICE_OBJECT DevObj,
    IN PDEVICE_CAPABILITIES DevCapabilities
  ) {
    IO_STATUS_BLOCK io_status;
    KEVENT pnp_event;
    NTSTATUS status;
    PDEVICE_OBJECT target_obj;
    PIO_STACK_LOCATION io_stack_loc;
    PIRP pnp_irp;

    RtlZeroMemory(DevCapabilities, sizeof *DevCapabilities);
    DevCapabilities->Size = sizeof *DevCapabilities;
    DevCapabilities->Version = 1;
    DevCapabilities->Address = -1;
    DevCapabilities->UINumber = -1;

    KeInitializeEvent(&pnp_event, NotificationEvent, FALSE);
    target_obj = IoGetAttachedDeviceReference(DevObj);
    pnp_irp = IoBuildSynchronousFsdRequest(
        IRP_MJ_PNP,
        target_obj,
        NULL,
        0,
        NULL,
        &pnp_event,
        &io_status
      );
    if (pnp_irp == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
      } else {
        pnp_irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        io_stack_loc = IoGetNextIrpStackLocation(pnp_irp);
        RtlZeroMemory(io_stack_loc, sizeof *io_stack_loc);
        io_stack_loc->MajorFunction = IRP_MJ_PNP;
        io_stack_loc->MinorFunction = IRP_MN_QUERY_CAPABILITIES;
        io_stack_loc->Parameters.DeviceCapabilities.Capabilities =
          DevCapabilities;
        status = IoCallDriver(target_obj, pnp_irp);
        if (status == STATUS_PENDING) {
            KeWaitForSingleObject(
                &pnp_event,
                Executive,
                KernelMode,
                FALSE,
                NULL
              );
            status = io_status.Status;
          }
      }
    ObDereferenceObject(target_obj);
    return status;
  }

/**
 * Dispatch an IRP
 *
 * @param DeviceObject
 *   The target device for the IRP
 *
 * @param Irp
 *   The IRP to dispatch
 *
 * @return
 *   The status of the operation
 */
static NTSTATUS WvDriverDispatchIrp(
    IN DEVICE_OBJECT * dev_obj,
    IN IRP * irp
  ) {
    WV_S_DEV_EXT * dev_ext;
    VOID ** ptrs;
    LONG flags;
    DRIVER_DISPATCH * irp_handler;

    ASSERT(dev_obj);
    ASSERT(irp);
    WVL_M_DEBUG_IRP_START(dev_obj, irp);

    /*
     * TODO: We currently handle the mini-driver and the old case.
     * Eventually there will just be the mini-driver case
     */
    dev_ext = dev_obj->DeviceExtension;
    ASSERT(dev_ext);

    if (dev_ext->MiniDriver) {
        /* Signal our intention as soon as possible */
        WvIncrementActiveIrpCount(dev_ext);

        /*
         * Set a default completion routine that will call WvlPassIrpUp.
         * This might be overridden by the device's IRP dispatcher
         */
        /* TODO: Deal with the PDO case, which has no next stack */
        IoCopyCurrentIrpStackLocationToNext(irp);
        IoSetCompletionRoutine(irp, WvIoCompletion, NULL, TRUE, TRUE, TRUE);

        /* Clear context */
        ptrs = irp->Tail.Overlay.DriverContext;
        RtlZeroMemory(ptrs, sizeof irp->Tail.Overlay.DriverContext);

        /* Check if the IRP should be enqueued */
        flags = InterlockedOr(&dev_ext->Flags, 0);
        if (flags & CvWvlDeviceFlagSerialIrps) {
            return WvDriverAddIrpToDeviceQueueInternal(
                dev_obj,
                irp,
                TRUE,
                TRUE
              );
          }

        /*
         * Otherwise, dispatch the IRP immediately.  The eventual call
         * to WvlPassIrpUp will decrement the active IRP count.  A call
         * to WvlPassIrpUp could either be by the default completion
         * routine or explicitly
         */
        ASSERT(dev_ext->IrpDispatch);
        return dev_ext->IrpDispatch(dev_obj, irp);
      }

    /* Handle the old, non-mini-driver case */
    irp_handler = WvDevGetIrpHandler(dev_obj);
    ASSERT(irp_handler);
    return irp_handler(dev_obj, irp);
  }

/* Allow the driver to handle IRPs of a particular major code. */
NTSTATUS STDCALL WvDriverHandleMajor(IN UCHAR Major) {
    WvDriverObj->MajorFunction[Major] = WvDriverDispatchIrp;
    return STATUS_SUCCESS;
  }

/**
 * Miscellaneous: Grouped memory allocation functions.
 */

/* Internal type describing the layout of the tracking data. */
typedef struct WVL_MEM_GROUP_ITEM_ {
    PCHAR Next;
    PVOID Obj;
  } WVL_S_MEM_GROUP_ITEM_, * WVL_SP_MEM_GROUP_ITEM_;

/**
 * Initialize a memory group for use.
 *
 * @v Group             The memory group to initialize.
 */
WVL_M_LIB VOID STDCALL WvlMemGroupInit(OUT WVL_SP_MEM_GROUP Group) {
    if (!Group)
      return;
    Group->First = Group->Current = Group->Last = NULL;
    return;
  }

/**
 * Attempt to allocate a memory resource and add it to a group.
 *
 * @v Group             The group to add the resource to.
 * @v Size              The size, in bytes, of the object to allocate.
 * @ret PVOID           A pointer to the newly allocated object, or NULL.
 *
 * The advantage to having an allocation associated with a group is
 * that all items in a group can be freed simultaneously.  Each resource _can_
 * be individually freed once a batch free is no longer desirable.  Do _not_
 * perform a regular wv_free() on an item and then try to free a group.
 */
WVL_M_LIB PVOID STDCALL WvlMemGroupAlloc(
    IN OUT WVL_SP_MEM_GROUP Group,
    IN SIZE_T Size
  ) {
    WVL_S_MEM_GROUP_ITEM_ item;
    PCHAR item_ptr;

    if (!Group || !Size)
      return NULL;

    /* We allocate the object, with a footer item. */
    item.Obj = wv_mallocz(Size + sizeof item);
    if (!item.Obj)
      return NULL;

    /* Note the footer where the item will be copied to. */
    item_ptr = (PCHAR) item.Obj + Size;
    /* We are obviously the last item. */
    item.Next = NULL;
    /* Copy item into the new object. */
    RtlCopyMemory(item_ptr, &item, sizeof item);
    /* Point the previously last item's 'Next' member to the new item. */
    if (Group->Last)
      RtlCopyMemory(Group->Last, &item_ptr, sizeof item_ptr);
    /* Note the new item as the last in the group. */
    Group->Last = item_ptr;
    /* Note the new item as the first in the group, if applicable. */
    if (!Group->First)
      Group->First = item_ptr;

    /* All done. */
    return item.Obj;
  }

/**
 * Free all memory resources in a group.
 *
 * @v Group             The group of resources to free.
 *
 * Do _not_ perform a regular wv_free() on an item and then try to
 * free a group with this function.  The tracking data will have
 * already been freed, so resources iteration is expected to fail.
 */
WVL_M_LIB VOID STDCALL WvlMemGroupFree(IN OUT WVL_SP_MEM_GROUP Group) {
    WVL_S_MEM_GROUP_ITEM_ item;
    PCHAR next;

    if (!Group || !Group->First)
      return;

    /* Point to the first item. */
    next = Group->First;
    /* For each item, fetch it and free it. */
    while (next) {
        /* Fill with the next item. */
        RtlCopyMemory(&item, next, sizeof item);
        wv_free(item.Obj);
        next = item.Next;
      }
    Group->First = Group->Current = Group->Last = NULL;
    return;
  }

/**
 * Iterate through memory resources in a group.
 *
 * @v Group             The group of resources to iterate through.
 * @ret PVOID           The next available object in the group, or NULL.
 *
 * Before beginning iteration, set Group->Current = Group->First.
 */
WVL_M_LIB PVOID STDCALL WvlMemGroupNextObj(IN OUT WVL_SP_MEM_GROUP Group) {
    WVL_S_MEM_GROUP_ITEM_ item;

    if (!Group || !Group->First)
      return NULL;

    /* Copy the current item for inspection. */
    RtlCopyMemory(&item, Group->Current, sizeof item);
    /* Remember the next object for next time. */
    Group->Current = item.Next;
    return item.Obj;
  }

/**
 * Attempt to allocate a batch of identically-sized memory
 * resources, and associate them with a group.
 *
 * @v Group             The group to associate the allocations with.
 * @v Size              The size, in bytes, of each object to allocate.
 * @v Count             The count of objects to allocate.
 * @ret PVOID           Points to the first allocated object in the batch.
 *
 * If a single allocation in the batch fails, any of the allocations in
 * that batch will be freed.  Do not confuse that to mean that all resources
 * in the group are freed.
 *
 * You might use this function to allocate a batch of objects with size X
 * and have them associated with a group.  Then you might call it again for
 * a batch of objects with size Y which will have the same group.  Then you
 * might add some individual objects to the group with WvlMemGroupAllocate().
 * Perhaps the last allocation fails; you can still free all allocations
 * with WvlMemGroupFree().
 */
WVL_M_LIB PVOID STDCALL WvlMemGroupBatchAlloc(
    IN OUT WVL_SP_MEM_GROUP Group,
    IN SIZE_T Size,
    IN UINT32 Count
  ) {
    WVL_S_MEM_GROUP_ITEM_ item;
    PVOID first_obj;
    WVL_S_MEM_GROUP tmp_group = {0};
    PCHAR item_ptr, last = (PCHAR) &tmp_group.First;

    if (!Group || !Size || !Count)
      return NULL;

    /* Allocate the first object and remember it. */
    first_obj = item.Obj = wv_mallocz(Size + sizeof item);
    if (!first_obj)
      return NULL;
    goto jump_in;
    do {
        /* We allocate each object, with a footer item. */
        item.Obj = wv_mallocz(Size + sizeof item);
        if (!item.Obj) {
            break;
          }
        jump_in:
        /* Note the footer where the item will be copied to. */
        item_ptr = (PCHAR) item.Obj + Size;
        /* We are obviously the newest item in the batch. */
        item.Next = NULL;
        /* Copy item into the new object. */
        RtlCopyMemory(item_ptr, &item, sizeof item);
        /* Point the previous item's 'Next' member to the new item. */
        RtlCopyMemory(last, &item_ptr, sizeof item_ptr);
        /* Note the new item as the last in the batch. */
        last = item_ptr;
      } while (--Count);

    /* Abort? */
    if (Count) {
        WvlMemGroupFree(&tmp_group);
        return NULL;
      }

    /*
     * Success. Point the real group's last item's
     * 'Next' member to the batch's first item.
     */
    if (Group->Last)
      RtlCopyMemory(Group->Last, &tmp_group.First, sizeof tmp_group.First);
    /* Set the real group's 'First' member, if applicable. */
    if (!Group->First)
      Group->First = tmp_group.First;
    /* All done. */
    return first_obj;
  }

WVL_M_LIB NTSTATUS WvlRegisterMiniDriver(
    OUT S_WVL_MINI_DRIVER ** minidriver,
    IN OUT DRIVER_OBJECT * drv_obj,
    IN DRIVER_ADD_DEVICE * add_dev,
    IN DRIVER_UNLOAD * unload
  ) {
    /* Check for invalid parameters */
    if (!minidriver || !drv_obj || !unload)
      return STATUS_UNSUCCESSFUL;

    *minidriver = wv_malloc(sizeof **minidriver);
    if (!*minidriver)
      return STATUS_INSUFFICIENT_RESOURCES;

    /* Initialize the mini-driver object */
    (*minidriver)->DriverObject = drv_obj;
    (*minidriver)->AddDevice = add_dev;
    (*minidriver)->Unload = unload;
    WvlInitializeResourceTracker((*minidriver)->Usage);

    /* If the mini-driver is external, initialize its driver object */
    if (drv_obj != WvDriverObj) {
        /* IRP major function dispatch table */
        RtlCopyMemory(
            drv_obj->MajorFunction,
            WvDriverObj->MajorFunction,
            sizeof drv_obj->MajorFunction
          );

        /* Set the driver's Unload and AddDevice routines */
        drv_obj->DriverUnload = WvUnloadMiniDriver;
        drv_obj->DriverExtension->AddDevice = add_dev;
      }

    /* Register the mini-driver */
    WvlAppendLockedListLink(WvRegisteredMiniDrivers, (*minidriver)->Link);
    KeClearEvent(&WvMiniDriversDeregistered);

    return STATUS_SUCCESS;
  }

WVL_M_LIB VOID WvlDeregisterMiniDriver(
    IN S_WVL_MINI_DRIVER * minidriver
  ) {
    /* Ignore an invalid parameter */
    if (!minidriver)
      return;

    /*
     * De-register the mini-driver.  If this is the last mini-driver
     * in the registration list, notify WvDeregisterMiniDrivers
     */
    if (WvlRemoveLockedListLink(WvRegisteredMiniDrivers, minidriver->Link))
      KeSetEvent(&WvMiniDriversDeregistered, 0, FALSE);

    WvlWaitForResourceZeroUsage(minidriver->Usage);
    wv_free(minidriver);
  }

/**
 * Invoke an external mini-driver's Unload routine.  Internal
 * mini-drivers' Unload routines will be invoked by WvDeregisterMiniDrivers.
 * Multiple mini-drivers with the same driver object are understood
 *
 * @param drv_obj
 *   The driver object provided by Windows
 */
static VOID STDCALL WvUnloadMiniDriver(IN DRIVER_OBJECT * drv_obj) {
    LIST_ENTRY * cur_link;
    S_WVL_MINI_DRIVER * minidriver;

    /* Find the associated mini-driver(s) */
    WvlAcquireLockedList(WvRegisteredMiniDrivers);
    for (
        cur_link = WvRegisteredMiniDrivers->List->Flink;
        cur_link != WvRegisteredMiniDrivers->List;
        cur_link = cur_link->Flink
      ) {
        ASSERT(cur_link);
        minidriver = CONTAINING_RECORD(cur_link, S_WVL_MINI_DRIVER, Link);

        /* Check for a match */
        if (minidriver->DriverObject == drv_obj) {
            WvlReleaseLockedList(WvRegisteredMiniDrivers);

            ASSERT(minidriver->Unload);
            minidriver->Unload(drv_obj);

            /* Start over at the beginning of the list */
            WvlAcquireLockedList(WvRegisteredMiniDrivers);
            cur_link = WvRegisteredMiniDrivers->List;
          }
      }
    WvlReleaseLockedList(WvRegisteredMiniDrivers);
  }

/**
 * Unload all internal mini-drivers and wait for all
 * mini-drivers to be deregistered
 */
static VOID WvDeregisterMiniDrivers(void) {
    /* Process internal mini-drivers */
    ASSERT(WvDriverObj);
    WvUnloadMiniDriver(WvDriverObj);

    /* Wait for all mini-driver Unload routines to complete */
    KeWaitForSingleObject(
        &WvMiniDriversDeregistered,
        Executive,
        KernelMode,
        FALSE,
        NULL
      );

    WvlDecrementResourceUsage(WvDriverUsage);
  }

/* TODO: Find a better place for this and adjust headers */
WVL_M_LIB VOID WvlInitializeLockedList(OUT S_WVL_LOCKED_LIST * list) {
    ASSERT(list);
    KeInitializeEvent(&list->Lock, SynchronizationEvent, TRUE);
    InitializeListHead(list->List);
  }

/* TODO: Find a better place for this and adjust headers */
WVL_M_LIB VOID WvlAcquireLockedList(IN S_WVL_LOCKED_LIST * list) {
    ASSERT(list);
    KeWaitForSingleObject(&list->Lock, Executive, KernelMode, FALSE, NULL);
  }

/* TODO: Find a better place for this and adjust headers */
WVL_M_LIB VOID WvlReleaseLockedList(IN S_WVL_LOCKED_LIST * list) {
    ASSERT(list);
    KeSetEvent(&list->Lock, 0, FALSE);
  }

/* TODO: Find a better place for this and adjust headers */
WVL_M_LIB VOID WvlAppendLockedListLink(
    IN OUT S_WVL_LOCKED_LIST * list,
    IN OUT LIST_ENTRY * link
  ) {
    ASSERT(list);
    ASSERT(link);
    WvlAcquireLockedList(list);
    InsertTailList(list->List, link);
    WvlReleaseLockedList(list);
  }

/* TODO: Find a better place for this and adjust headers */
WVL_M_LIB BOOLEAN WvlRemoveLockedListLink(
    IN OUT S_WVL_LOCKED_LIST * list,
    IN OUT LIST_ENTRY * link
  ) {
    BOOLEAN result;

    ASSERT(list);
    ASSERT(link);
    WvlAcquireLockedList(list);
    result = RemoveEntryList(link);
    WvlReleaseLockedList(list);
    return result;
  }

/* TODO: Find a better place for this and adjust headers */
WVL_M_LIB VOID WvlInitializeResourceTracker(
    S_WVL_RESOURCE_TRACKER * res_tracker
  ) {
    ASSERT(res_tracker);
    res_tracker->UsageCount = 0;
    KeInitializeEvent(&res_tracker->ZeroUsage, NotificationEvent, TRUE);
  }

/* TODO: Find a better place for this and adjust headers */
WVL_M_LIB VOID WvlWaitForResourceZeroUsage(
    S_WVL_RESOURCE_TRACKER * res_tracker
  ) {
    ASSERT(res_tracker);
    KeWaitForSingleObject(
        &res_tracker->ZeroUsage,
        Executive,
        KernelMode,
        FALSE,
        NULL
      );
  }

/* TODO: Find a better place for this and adjust headers */
WVL_M_LIB VOID WvlIncrementResourceUsage(
    S_WVL_RESOURCE_TRACKER * res_tracker
  ) {
    LONG c;

    ASSERT(res_tracker);
    c = InterlockedIncrement(&res_tracker->UsageCount);

    /* If the usage is no longer zero, clear the event */
    ASSERT(c >= 0);
    if (c == 1)
      KeClearEvent(&res_tracker->ZeroUsage);
  }

/* TODO: Find a better place for this and adjust headers */
WVL_M_LIB VOID WvlDecrementResourceUsage(
    S_WVL_RESOURCE_TRACKER * res_tracker
  ) {
    LONG c;

    ASSERT(res_tracker);
    c = InterlockedDecrement(&res_tracker->UsageCount);

    /* If the usage count reaches zero, set the event */
    ASSERT(c >= 0);
    if (!c)
      KeSetEvent(&res_tracker->ZeroUsage, 0, FALSE);
  }

WVL_M_LIB NTSTATUS STDCALL WvlCreateDevice(
    IN S_WVL_MINI_DRIVER * minidriver,
    IN ULONG dev_ext_sz,
    IN UNICODE_STRING * dev_name,
    IN DEVICE_TYPE dev_type,
    IN ULONG dev_characteristics,
    IN BOOLEAN exclusive,
    OUT DEVICE_OBJECT ** dev_obj
  ) {
    NTSTATUS status;
    DEVICE_OBJECT * new_dev;
    WV_S_DEV_EXT * new_dev_ext;

    /* Check for invalid parameters */
    if (!minidriver || dev_ext_sz < sizeof *new_dev_ext || !dev_obj)
      return STATUS_INVALID_PARAMETER;

    /* Create the device object */
    ASSERT(minidriver->DriverObject);
    status = IoCreateDevice(
        minidriver->DriverObject,
        dev_ext_sz,
        dev_name,
        dev_type,
        dev_characteristics,
        exclusive,
        &new_dev
      );
    if (!NT_SUCCESS(status))
      goto err_create_dev;
    ASSERT(new_dev);

    /* Initialize the common part of the device extension */
    new_dev_ext = new_dev->DeviceExtension;
    ASSERT(new_dev_ext);
    /* TODO: device and IrpDispatch */
    KeInitializeEvent(&new_dev_ext->IrpArrival, NotificationEvent, FALSE);
    new_dev_ext->ThreadHandle = NULL;
    new_dev_ext->Thread = NULL;
    InitializeListHead(new_dev_ext->IrpQueue);
    new_dev_ext->SentinelIrpLink = NULL;
    new_dev_ext->NotAvailable = FALSE;
    KeInitializeSpinLock(&new_dev_ext->Lock);
    new_dev_ext->Flags = CvWvlDeviceFlagZero;
    new_dev_ext->ActiveIrpCount = 0;
    KeInitializeEvent(
        &new_dev_ext->ActiveIrpCountOne,
        NotificationEvent,
        FALSE
      );
    new_dev_ext->MiniDriver = minidriver;
    WvlInitializeResourceTracker(new_dev_ext->Usage);

    /* Start the device thread */
    status = WvStartDeviceThread(new_dev);
    if (!NT_SUCCESS(status))
      goto err_dev_thread;

    /* When the thread is stopping, it'll decrement this usage */
    WvlIncrementResourceUsage(minidriver->Usage);

    *dev_obj = new_dev;
    return STATUS_SUCCESS;

    WvStopDeviceThread(new_dev);
    err_dev_thread:

    IoDeleteDevice(new_dev);
    err_create_dev:

    return status;
  }

WVL_M_LIB VOID STDCALL WvlDeleteDevice(IN DEVICE_OBJECT * dev_obj) {
    WV_S_DEV_EXT * dev_ext;

    ASSERT(dev_obj);
    dev_ext = dev_obj->DeviceExtension;
    ASSERT(dev_ext);

    WvlLockDevice(dev_obj);
    dev_ext->NotAvailable = TRUE;
    WvlUnlockDevice(dev_obj);
  }

WVL_M_LIB VOID STDCALL WvlLockDevice(IN DEVICE_OBJECT * dev_obj) {
    WV_S_DEV_EXT * dev_ext;
    KIRQL irql;

    ASSERT(dev_obj);
    dev_ext = dev_obj->DeviceExtension;
    ASSERT(dev_ext);

    KeAcquireSpinLock(&dev_ext->Lock, &irql);
    dev_ext->PreLockIrql = irql;
  }

WVL_M_LIB VOID STDCALL WvlUnlockDevice(IN DEVICE_OBJECT * dev_obj) {
    WV_S_DEV_EXT * dev_ext;

    ASSERT(dev_obj);
    dev_ext = dev_obj->DeviceExtension;
    ASSERT(dev_ext);

    KeReleaseSpinLock(&dev_ext->Lock, dev_ext->PreLockIrql);
  }

/**
 * Start a device thread
 *
 * @param DeviceObject
 *   The device to start a thread for
 *
 * @return
 *   The status of the operation
 */
static NTSTATUS WvStartDeviceThread(IN DEVICE_OBJECT * dev_obj) {
    WV_S_DEV_EXT * dev_ext;
    OBJECT_ATTRIBUTES obj_attrs;
    NTSTATUS status;

    if (!dev_obj)
      return STATUS_INVALID_PARAMETER;

    dev_ext = dev_obj->DeviceExtension;
    ASSERT(dev_ext);

    KeInitializeEvent(&dev_ext->IrpArrival, NotificationEvent, FALSE);
    InitializeObjectAttributes(
        &obj_attrs,
        NULL,
        OBJ_KERNEL_HANDLE,
        NULL,
        NULL
      );
    status = PsCreateSystemThread(
        &dev_ext->ThreadHandle,
        THREAD_ALL_ACCESS,
        &obj_attrs,
        NULL,
        NULL,
        WvDeviceThread,
        dev_obj
      );
    if (!NT_SUCCESS(status))
      goto err_create_thread;

    /* Decremented by WvDeviceThread */
    WvlIncrementResourceUsage(dev_ext->Usage);

    status = WvlCallFunctionInDeviceThread(
        dev_obj,
        WvTestDeviceThread,
        NULL,
        TRUE
      );
    if (!NT_SUCCESS(status) || !dev_ext->Thread)
      goto err_test_thread;

    return status;

    err_test_thread:

    WvStopDeviceThread(dev_obj);
    err_create_thread:

    return status;
  }

/**
 * Stop a device thread
 *
 * @param DeviceObject
 *   The device whose thread will be stopped
 */
static VOID WvStopDeviceThread(IN DEVICE_OBJECT * dev_obj) {
    WV_S_DEV_EXT * dev_ext;
    HANDLE thread_handle;
    NTSTATUS status;
    ETHREAD * thread;

    ASSERT(dev_obj);
    dev_ext = dev_obj->DeviceExtension;
    ASSERT(dev_ext);
    thread_handle = dev_ext->ThreadHandle;
    ASSERT(thread_handle);

    /* Reference the thread */
    status = ObReferenceObjectByHandle(
        thread_handle,
        THREAD_ALL_ACCESS,
        *PsThreadType,
        KernelMode,
        &thread,
        NULL
      );
    ASSERT(NT_SUCCESS(status));

    /* Signal the the stop */
    status = WvlCallFunctionInDeviceThread(
        dev_obj,
        WvStopDeviceThreadInThread,
        NULL,
        TRUE
      );
    ASSERT(NT_SUCCESS(status));

    /* Wait for the thread to stop */
    KeWaitForSingleObject(
        thread,
        Executive,
        KernelMode,
        FALSE,
        NULL
      );

    /* Cleanup */
    ObDereferenceObject(thread);
    ZwClose(thread_handle);
  }

/**
 * Stop a device thread from within the thread
 *
 * @param DeviceObject
 *   The device whose thread will be stopped
 *
 * @param Context
 *   Ignored
 *
 * @retval STATUS_SUCCESS
 *
 * This function is a bit of a sentinel, as WvDeviceThread will notice it
 */
static NTSTATUS WvStopDeviceThreadInThread(
    IN DEVICE_OBJECT * dev_obj,
    IN VOID * c
  ) {
    ASSERT(dev_obj);
    (VOID) c;
    DBG("Stopping thread for device %p...\n", (VOID *) dev_obj);
    return STATUS_SUCCESS;
  }

/**
 * Test a device thread
 *
 * @param DeviceObject
 *   The device whose thread will be tested
 *
 * @param Context
 *   Ignored.
 *
 * @retval STATUS_SUCCESS
 */
static NTSTATUS WvTestDeviceThread(IN DEVICE_OBJECT * dev_obj, IN VOID * c) {
    WV_S_DEV_EXT * dev_ext;

    (VOID) c;

    ASSERT(dev_obj);
    dev_ext = dev_obj->DeviceExtension;
    ASSERT(dev_ext);

    WvlIncrementResourceUsage(dev_ext->Usage);
    dev_ext->Thread = PsGetCurrentThread();
    WvlDecrementResourceUsage(dev_ext->Usage);
    DBG("Thread test for device %p passed\n", (VOID *) dev_obj);
    return STATUS_SUCCESS;
  }

WVL_M_LIB NTSTATUS STDCALL WvlCallFunctionInDeviceThread(
    IN DEVICE_OBJECT * dev_obj,
    IN F_WVL_DEVICE_THREAD_FUNCTION * func,
    IN VOID * context,
    IN BOOLEAN wait
  ) {
    WV_S_DEV_EXT * dev_ext;
    S_WVL_DEVICE_THREAD_WORK_ITEM stack_work_item;
    S_WVL_DEVICE_THREAD_WORK_ITEM * work_item;
    VOID ** ptrs;
    NTSTATUS status;

    /* Check for invalid parameters */
    if (!dev_obj || !func)
      return STATUS_INVALID_PARAMETER;

    dev_ext = dev_obj->DeviceExtension;
    ASSERT(dev_ext);

    /*
     * If we wait for the function to complete, we use the stack.
     * Otherwise, we try to allocate the work item
     */
    if (!wait) {
        work_item = wv_malloc(sizeof *work_item);
        if (!work_item)
          return STATUS_INSUFFICIENT_RESOURCES;
      } else {
        work_item = &stack_work_item;
      }

    work_item->Function = func;

    /* Signal the work item */
    WvIncrementActiveIrpCount(dev_ext);

    /**
     * We use some of the 4 driver-owned pointers in the dummy IRP.
     * The first lets WvDeviceThread recognize and find the work item.
     * The second is the context to be passed to the called function.
     * The third lets WvDeviceThread recognize and free the work item,
     * if required.  The fourth is used by WvDriverAddIrpToDeviceQueueInternal
     * for waiting for IRP completion, if required
     */
    ptrs = work_item->DummyIrp->Tail.Overlay.DriverContext;
    ptrs[0] = work_item;
    ptrs[1] = context;
    ptrs[2] = wait ? NULL : work_item;

    status = WvDriverAddIrpToDeviceQueueInternal(
        dev_obj,
        work_item->DummyIrp,
        wait,
        TRUE
      );

    /* If we're not waiting and the work item was enqueued, that's good */
    if (!wait && status == STATUS_PENDING)
      return STATUS_SUCCESS;

    /* Otherwise, return the result */
    return status;
  }

/**
 * Increment the active IRP count for a mini-driver device
 *
 * @param DeviceExtension
 *   The mini-driver's device extension
 *
 * @return
 *   The incremented count of active IRPs
 */
static LONG WvIncrementActiveIrpCount(WV_S_DEV_EXT * dev_ext) {
    LONG c;

    ASSERT(dev_ext);
    c = InterlockedIncrement(&dev_ext->ActiveIrpCount);
    ASSERT(c > 0);
    return c;
  }

/**
 * Decrement the active IRP count for a mini-driver device
 *
 * @param DeviceExtension
 *   The mini-driver's device extension
 *
 * @return
 *   The decremented count of active IRPs
 *
 * If the device has been placed into serial IRP mode and there is only
 * one more active IRP, then that IRP must be waiting for all active IRPs
 * to complete.  This function will signal that waiter, if needed
 */
static LONG WvDecrementActiveIrpCount(WV_S_DEV_EXT * dev_ext) {
    LONG c;
    LONG flags;

    ASSERT(dev_ext);
    c = InterlockedDecrement(&dev_ext->ActiveIrpCount);
    ASSERT(c >= 0);
    flags = InterlockedOr(&dev_ext->Flags, 0);

    /*
     * If there's only one active IRP, it could be the waiter, and
     * it could be waiting for us to signal it that it is the only
     * active IRP.  Or, it could simply be an IRP that is currently
     * being processed by the device's thread, so we check that, too
     */
    if (c == 1 && flags & CvWvlDeviceFlagSerialIrpsNeedSignal) {
        InterlockedXor(
            &dev_ext->Flags,
            CvWvlDeviceFlagSerialIrpsNeedSignal
          );
        KeSetEvent(&dev_ext->ActiveIrpCountOne, 0, FALSE);
      }

    return c;
  }

WVL_M_LIB NTSTATUS STDCALL WvlAddIrpToDeviceQueue(
    IN DEVICE_OBJECT * dev,
    IN IRP * irp,
    IN BOOLEAN wait
  ) {
    return WvDriverAddIrpToDeviceQueueInternal(dev, irp, wait, FALSE);
  }

/**
 * Add an IRP (or pseudo-IRP) to a device's IRP queue
 *
 * @param DeviceObject
 *   The device to process the IRP
 *
 * @param Irp
 *   The IRP to enqueue for the device
 *
 * @param Wait
 *   Specifies whether or not to wait for the IRP to be processed by the
 *   mini-driver device's thread.
 *
 *   If FALSE and the IRP is added to the device's queue, this function
 *   should return STATUS_PENDING.
 *
 *   If FALSE and the device is no longer available, the IRP will be
 *   completed and this function should return STATUS_NO_SUCH_DEVICE.
 *
 *   If TRUE and the IRP is added to the device's queue, this function
 *   returns the status of processing the IRP within the device's
 *   thread context.
 *
 *   If TRUE and the device is no longer available, the IRP will be
 *   completed and this function should return STATUS_NO_SUCH_DEVICE
 *
 * @param Internal
 *   Specifies whether or not this function has been called for internal
 *   use.  If not, then it has been invoked by a call to WvlAddIrpToDeviceQueue
 *
 * @retval STATUS_NO_SUCH_DEVICE
 *   The device is no longer available.  'Wait' is TRUE or FALSE
 * @retval STATUS_PENDING
 *   The IRP was successfully queued and 'Wait' was FALSE
 * @return
 *   Other values are returned when 'Wait' is TRUE and indicate the
 *   status of processing the IRP within the device's thread context
 */
static NTSTATUS STDCALL WvDriverAddIrpToDeviceQueueInternal(
    IN DEVICE_OBJECT * dev_obj,
    IN IRP * irp,
    IN BOOLEAN wait,
    IN BOOLEAN internal
  ) {
    LONG flags;
    VOID ** ptrs;
    S_WVL_STATUS_WAITER status_waiter;
    WV_S_DEV_EXT * dev_ext;
    KEVENT completion;
    NTSTATUS status;

    ASSERT(dev_obj);
    dev_ext = dev_obj->DeviceExtension;
    ASSERT(dev_ext);
    ASSERT(irp);

    ptrs = irp->Tail.Overlay.DriverContext;

    /*
     * Check if we should process the IRP or work item asynchronously.
     * If not, this overrides the user's Wait argument
     */
    if (!wait) {
        flags = InterlockedOr(&dev_ext->Flags, 0);
        if (flags & CvWvlDeviceFlagSerialIrps)
          wait = TRUE;
      }

    /*
     * If we wait for status, we tell the thread where to return that
     * status and where the completion event is.  Note that the
     * caller _must_ zero out all of the DriverContext pointers.
     * That way the thread won't work with garbage
     */
    if (wait) {
        KeInitializeEvent(&status_waiter.Complete, NotificationEvent, FALSE);
        status_waiter.Status = STATUS_DRIVER_INTERNAL_ERROR;
        ptrs[3] = &status_waiter;
      }

    WvlLockDevice(dev_obj);
    if (dev_ext->NotAvailable) {
        status = irp->IoStatus.Status = STATUS_NO_SUCH_DEVICE;
      } else {
        /* Enqueue */
        InsertTailList(dev_ext->IrpQueue, &irp->Tail.Overlay.ListEntry);
        status = STATUS_PENDING;

        /*
         * We decrement the active IRP count ("unsignal" our intention)
         * because WvDeviceThread will re-establish it when our turn comes
         */
        WvDecrementActiveIrpCount(dev_ext);
      }

    /*
     * If we are invoked as a result of a call to WvlAddIrpToDeviceQueue,
     * it could be that we are re-adding the IRP to the device's IRP
     * queue.  Without any counter-measure, this would result in the thread
     * endlessly looping over any IRPs that keep adding themselves to the
     * queue.  Thus, if this is a possibility, make a note of this IRP as a
     * sentinel value for the thread to check to know if it's about to
     * loop endlessly
     */
    if (!internal && !dev_ext->SentinelIrpLink)
      dev_ext->SentinelIrpLink = &irp->Tail.Overlay.ListEntry;

    KeSetEvent(&dev_ext->IrpArrival, 0, FALSE);
    WvlUnlockDevice(dev_obj);

    /* If the device wasn't available, we decrement the active IRP count */
    if (status == STATUS_NO_SUCH_DEVICE) {
        /*  If it was a real IRP, complete it */
        if (!ptrs[0]) {
            irp->IoStatus.Status = status;
            WvlPassIrpUp(dev_obj, irp, IO_NO_INCREMENT);
          } else {
            /* It was a work item */
            WvDecrementActiveIrpCount(dev_ext);
          }
        return status;
      }

    if (wait) {
        /* Wait for the IRP or work item to complete and return its status */
        KeWaitForSingleObject(
            &status_waiter.Complete,
            Executive,
            KernelMode,
            FALSE,
            NULL
          );
        status = status_waiter.Status;
      }

    return status;
  }

/**
 * The device thread for a mini-driver device
 *
 * @param Context
 *   The device object
 *
 * This routine will process IRPs (and pseudo-IRPs) in the device's queue
 */
static VOID STDCALL WvDeviceThread(IN VOID * context) {
    DEVICE_OBJECT * const dev_obj = context;
    const LONG serial_irp_flags =
      CvWvlDeviceFlagSerialIrps | CvWvlDeviceFlagSerialIrpsNeedSignal;
    WV_S_DEV_EXT * dev_ext;
    LARGE_INTEGER timeout;
    LIST_ENTRY * link;
    BOOLEAN stop;
    LONG flags;
    IRP * irp;
    S_WVL_DEVICE_THREAD_WORK_ITEM * work_item;
    S_WVL_MINI_DRIVER * minidriver;

    ASSERT(dev_obj);

    DBG("Thread for device %p started\n", (VOID *) dev_obj);

    dev_ext = dev_obj->DeviceExtension;
    ASSERT(dev_ext);

    /* Wake up at least every 30 seconds. */
    timeout.QuadPart = -300000000LL;

    stop = FALSE;
    WvlLockDevice(dev_obj);
    while (1) {
        link = RemoveHeadList(dev_ext->IrpQueue);
        ASSERT(link);

        /* Did we finish our pass through the IRP queue? */
        if (link == dev_ext->IrpQueue) {
            /* Yes.  We can clear the serial IRP mode */
            InterlockedAnd(&dev_ext->Flags, ~serial_irp_flags);
            KeClearEvent(&dev_ext->ActiveIrpCountOne);

            /* Clear any sentinel value, since the list is empty */
            dev_ext->SentinelIrpLink = NULL;

            /* Ready for more work */
            KeClearEvent(&dev_ext->IrpArrival);
            WvlUnlockDevice(dev_obj);

            /* Are we stopping? */
            if (stop) {
                /*
                 * Yes.  All IRPs have been processed and no more
                 * can be added, since the device was marked as no
                 * longer available before 'stop' was set
                 */
                break;
              }

            /* Otherwise, wait for more work or the timeout */
            KeWaitForSingleObject(
                &dev_ext->IrpArrival,
                Executive,
                KernelMode,
                FALSE,
                &timeout
              );
            WvlLockDevice(dev_obj);
            continue;
          }

        /*
         * Check the sentinel value to prevent endlessly looping over IRPs
         * that keep re-adding themselves to the device's IRP queue.  We'll
         * still endlessly loop, but we'll use the timeout to give a chance
         * for something else to happen to make things more interesting
         */
        if (link == dev_ext->SentinelIrpLink) {
            /* Clear the sentinel value */
            dev_ext->SentinelIrpLink = NULL;

            /* Ready for more work */
            KeClearEvent(&dev_ext->IrpArrival);
            WvlUnlockDevice(dev_obj);

            /* Wait for more work or the timeout */
            KeWaitForSingleObject(
                &dev_ext->IrpArrival,
                Executive,
                KernelMode,
                FALSE,
                &timeout
              );
            WvlLockDevice(dev_obj);
          }

        /* Otherwise, examine the IRP */
        irp = CONTAINING_RECORD(link, IRP, Tail.Overlay.ListEntry);
        ASSERT(irp);

        /*
         * Re-establish the IRP count that was cleared when the IRP or
         * work item was enqueued.  It will later be decremented by
         * either WvlPassIrpUp or by WvProcessDeviceThreadWorkItem
         */
        WvIncrementActiveIrpCount(dev_ext);

        /* Is this a work item? */
        if (irp->Tail.Overlay.DriverContext[0]) {
            /* Yes */
            work_item = CONTAINING_RECORD(
                irp,
                S_WVL_DEVICE_THREAD_WORK_ITEM,
                DummyIrp
              );
            ASSERT(work_item);

            /* Check for a stop signal */
            if (work_item->Function == WvStopDeviceThreadInThread)
              dev_ext->NotAvailable = TRUE;

            /* Unlock the device and process the work item */
            WvlUnlockDevice(dev_obj);
            WvProcessDeviceThreadWorkItem(dev_obj, work_item, stop);
          } else {
            /* This is a normal IRP.  Unlock the device and process it */
            WvlUnlockDevice(dev_obj);
            WvProcessDeviceIrp(dev_obj, irp, stop);
          }

        /* Continue processing the IRP queue */
        WvlLockDevice(dev_obj);

        /* Check if we should be stopping, soon */
        if (dev_ext->NotAvailable)
          stop = TRUE;
      }

    minidriver = dev_ext->MiniDriver;
    ASSERT(minidriver);

    /* Incremented by WvStartDeviceThread */
    WvlDecrementResourceUsage(dev_ext->Usage);

    /* Delete the device */
    WvlWaitForResourceZeroUsage(dev_ext->Usage);
    DBG("Device %p deleted.  Thread terminated\n", (VOID *) dev_obj);
    IoDeleteDevice(dev_obj);

    WvlDecrementResourceUsage(minidriver->Usage);

    PsTerminateSystemThread(STATUS_SUCCESS);
    DBG("Yikes!\n");
  }

/**
 * Process a device thread work item
 *
 * @param DeviceObject
 *   The device to process the work item
 *
 * @param WorkItem
 *   The work item to process
 *
 * @param DeviceNotAvailable
 *   A boolean specifying whether or not to reject the work item
 */
static VOID WvProcessDeviceThreadWorkItem(
    IN DEVICE_OBJECT * dev_obj,
    IN S_WVL_DEVICE_THREAD_WORK_ITEM * work_item,
    IN BOOLEAN reject
  ) {
    WV_S_DEV_EXT * dev_ext;
    VOID ** ptrs;
    VOID * cleanup_item;
    S_WVL_STATUS_WAITER * status_waiter;
    NTSTATUS status;

    ASSERT(dev_obj);
    dev_ext = dev_obj->DeviceExtension;
    ASSERT(dev_ext);
    ASSERT(work_item);
    ptrs = work_item->DummyIrp->Tail.Overlay.DriverContext;
    cleanup_item = ptrs[2];
    status_waiter = ptrs[3];

    if (reject) {
        status = STATUS_NO_SUCH_DEVICE;
      } else {
        /* Call the work item function, passing the device and context */
        ASSERT(work_item->Function);
        status = work_item->Function(dev_obj, ptrs[1]);
      }

    /* Possible cleanup */
    wv_free(cleanup_item);

    /*
     * Decrement the active IRP count that was incremented by either
     * WvlCallFunctionInDeviceThread or by WvDeviceThread
     */
    WvDecrementActiveIrpCount(dev_ext);

    /* Signal that the work item has been completed */
    if (status_waiter) {
        status_waiter->Status = status;
        KeSetEvent(&status_waiter->Complete, 0, FALSE);
      }
  }

/**
 * Process an IRP in a device's thread context
 *
 * @param Device
 *   The device to process the IRP
 *
 * @param Irp
 *   The IRP to process
 *
 * @param DeviceNotAvailable
 *   A boolean specifying whether or not to reject the IRP
 */
static VOID WvProcessDeviceIrp(
    IN DEVICE_OBJECT * dev_obj,
    IN IRP * irp,
    IN BOOLEAN reject
  ) {
    VOID ** ptrs;
    S_WVL_STATUS_WAITER * status_waiter;
    NTSTATUS status;
    WV_S_DEV_EXT * dev_ext;

    ASSERT(dev_obj);
    ASSERT(irp);
    ptrs = irp->Tail.Overlay.DriverContext;

    /* Note the IRP completion event and status slot */
    status_waiter = ptrs[3];

    if (reject) {
        status = irp->IoStatus.Status = STATUS_NO_SUCH_DEVICE;
        WvlPassIrpUp(dev_obj, irp, IO_NO_INCREMENT);
      } else {
        /* Dispatch the IRP */
        dev_ext = dev_obj->DeviceExtension;
        ASSERT(dev_ext);
        ASSERT(dev_ext->IrpDispatch);
        status = dev_ext->IrpDispatch(dev_obj, irp);
      }

    /* Signal that the IRP has been completed */
    if (status_waiter) {
        status_waiter->Status = status;
        KeSetEvent(&status_waiter->Complete, 0, FALSE);
      }
  }

WVL_M_LIB VOID STDCALL WvlPassIrpUp(
    IN DEVICE_OBJECT * dev_obj,
    IN IRP * irp,
    IN const CHAR boost
  ) {
    WV_S_DEV_EXT * dev_ext;

    ASSERT(dev_obj);
    dev_ext = dev_obj->DeviceExtension;
    ASSERT(dev_ext);
    ASSERT(irp);

    DBG("Passing IRP %p up\n", (VOID *) irp);
    WvDecrementActiveIrpCount(dev_ext);
    IoCompleteRequest(irp, boost);
  }

WVL_M_LIB NTSTATUS STDCALL WvlPassIrpDown(
    IN DEVICE_OBJECT * dev_obj,
    IN OUT IRP * irp
  ) {
    ASSERT(dev_obj);
    ASSERT(irp);
    DBG("Passing IRP %p down\n", (VOID *) irp);
    return IoCallDriver(dev_obj, irp);
  }

/**
 * Simple IRP completion routine
 *
 * @param DeviceObject
 *   The device associated with the IRP completion event
 *
 * @param Irp
 *   The IRP that has been completed by a lower driver
 *
 * @param Context
 *   The event to signal that the IRP has been completed
 */
static NTSTATUS STDCALL WvIoCompletion(
    IN DEVICE_OBJECT * dev_obj,
    IN IRP * irp,
    IN VOID * context
  ) {
    KEVENT * const event = context;
    WV_S_DEV_EXT * dev_ext;

    ASSERT(dev_obj);
    dev_ext = dev_obj->DeviceExtension;
    ASSERT(dev_ext);
    ASSERT(irp);

    /*
     * For mini-drivers, we will pass up/complete the IRP if this
     * completion routine was set by WvDriverDispatchIrp
     */
    if (!event && dev_ext->MiniDriver)
      WvlPassIrpUp(dev_obj, irp, IO_NO_INCREMENT);

    if (event && irp->PendingReturned)
      KeSetEvent(event, 0, FALSE);

    return STATUS_MORE_PROCESSING_REQUIRED;
  }

WVL_M_LIB NTSTATUS STDCALL WvlWaitForIrpCompletion(
    IN DEVICE_OBJECT * lower_dev_obj,
    IN IRP * irp
  ) {
    KEVENT event;

    ASSERT(lower_dev_obj);
    ASSERT(irp);

    /* Prepare to wait for the IRP to complete */
    IoCopyCurrentIrpStackLocationToNext(irp);
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    IoSetCompletionRoutine(
        irp,
        WvIoCompletion,
        &event,
        TRUE,
        TRUE,
        TRUE
      );

    /* Send the IRP down and wait for completion */
    if (WvlPassIrpDown(lower_dev_obj, irp) == STATUS_PENDING)
      KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);

    /* Return the status from the lower driver */
    return irp->IoStatus.Status;
  }

WVL_M_LIB BOOLEAN STDCALL WvlInDeviceThread(IN DEVICE_OBJECT * dev_obj) {
    WV_S_DEV_EXT * dev_ext;

    ASSERT(dev_obj);
    dev_ext = dev_obj->DeviceExtension;
    ASSERT(dev_ext);
    return PsGetCurrentThread() == dev_ext->Thread;
  }

/**
 * Wrapper for WvlWaitForActiveIrps to be re-invoked within a device thread
 *
 * @param DeviceObject
 *   The device object whose thread will call this function
 *
 * @param Context
 *   The IRP currently being processed
 *
 * @retval STATUS_SUCCESS
 *   The active IRP count reached one
 */
static NTSTATUS WvDriverWaitForActiveIrps(
    IN DEVICE_OBJECT * dev_obj,
    IN VOID * context
  ) {
    IRP * const irp = context;

    ASSERT(dev_obj);
    ASSERT(irp);
    return WvlWaitForActiveIrps(dev_obj, irp);
  }

WVL_M_LIB NTSTATUS STDCALL WvlWaitForActiveIrps(
    IN DEVICE_OBJECT * dev_obj,
    IN IRP * irp
  ) {
    const LONG serial_irp_flags =
      CvWvlDeviceFlagSerialIrps | CvWvlDeviceFlagSerialIrpsNeedSignal;
    WV_S_DEV_EXT * dev_ext;
    LONG flags;
    LONG c;

    ASSERT(dev_obj);
    dev_ext = dev_obj->DeviceExtension;
    ASSERT(dev_ext);
    ASSERT(irp);

    /*
     * Change the IRP-processing mode to serial as soon as possible,
     * even though the device's thread could unset it
     */
    flags = InterlockedOr(&dev_ext->Flags, serial_irp_flags);

    /* Check if we're running from inside the device's thread */
    if (WvlInDeviceThread(dev_obj)) {
        /* This is a simple case of waiting for all active IRPs to finish */
        c = InterlockedOr(&dev_ext->ActiveIrpCount, 0);
        if (c != 1) {
            KeWaitForSingleObject(
                &dev_ext->ActiveIrpCountOne,
                Executive,
                KernelMode,
                FALSE,
                NULL
              );
          } else {
            /* It's just us, so the signal is no longer needed */
            InterlockedXor(
                &dev_ext->Flags,
                CvWvlDeviceFlagSerialIrpsNeedSignal
              );
          }
        return STATUS_SUCCESS;
      }

    /* If we're outside the thread, wait for a turn inside it */
    return WvlCallFunctionInDeviceThread(
        dev_obj,
        WvDriverWaitForActiveIrps,
        irp,
        TRUE
      );
  }
