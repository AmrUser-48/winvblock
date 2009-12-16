/**
 * Copyright 2006-2008, V.
 * Portions copyright (C) 2009, Shao Miller <shao.miller@yrdsb.edu.on.ca>.
 * For WinAoE contact information, see http://winaoe.org/
 *
 * This file is part of WinVBlock, derived from WinAoE.
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
#ifndef _MOUNT_H
#  define _MOUNT_H

/**
 * @file
 *
 * Mount command and device control code header
 *
 */

#  include "portable.h"

#  define IOCTL_AOE_SCAN CTL_CODE(FILE_DEVICE_CONTROLLER, 0x800,\
                                METHOD_BUFFERED,\
                                FILE_READ_DATA | FILE_WRITE_DATA)
#  define IOCTL_AOE_SHOW CTL_CODE(FILE_DEVICE_CONTROLLER, 0x801,\
                                METHOD_BUFFERED,\
                                FILE_READ_DATA | FILE_WRITE_DATA)
#  define IOCTL_AOE_MOUNT CTL_CODE(FILE_DEVICE_CONTROLLER, 0x802,\
                                 METHOD_BUFFERED,\
                                 FILE_READ_DATA | FILE_WRITE_DATA)
#  define IOCTL_AOE_UMOUNT CTL_CODE(FILE_DEVICE_CONTROLLER, 0x803,\
                                  METHOD_BUFFERED,\
                                  FILE_READ_DATA | FILE_WRITE_DATA)

typedef struct _MOUNT_TARGET
{
  winvblock__uint8 ClientMac[6];
  winvblock__uint8 ServerMac[6];
  winvblock__uint32 Major;
  winvblock__uint32 Minor;
  LONGLONG LBASize;
  LARGE_INTEGER ProbeTime;
} MOUNT_TARGET,
*PMOUNT_TARGET;

typedef struct _MOUNT_TARGETS
{
  winvblock__uint32 Count;
  MOUNT_TARGET Target[];
} MOUNT_TARGETS,
*PMOUNT_TARGETS;

typedef struct _MOUNT_DISK
{
  winvblock__uint32 Disk;
  winvblock__uint8 ClientMac[6];
  winvblock__uint8 ServerMac[6];
  winvblock__uint32 Major;
  winvblock__uint32 Minor;
  LONGLONG LBASize;
} MOUNT_DISK,
*PMOUNT_DISK;

typedef struct _MOUNT_DISKS
{
  winvblock__uint32 Count;
  MOUNT_DISK Disk[];
} MOUNT_DISKS,
*PMOUNT_DISKS;

#endif				/* _MOUNT_H */
