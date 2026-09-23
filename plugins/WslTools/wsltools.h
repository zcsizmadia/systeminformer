/*
 * Copyright (c) 2022 Winsider Seminars & Solutions, Inc.  All rights reserved.
 *
 * This file is part of System Informer.
 *
 * Authors:
 *
 *     zcsizmadia   2026
 *
 */

#ifndef _WSLTOOLS_H
#define _WSLTOOLS_H

#include <phdk.h>
#include <phappresource.h>
#include <settings.h>

#include "resource.h"

#define PLUGIN_NAME L"WslTools"

#define SETTING_NAME_TREE_LIST_COLUMNS (PLUGIN_NAME L".TreeListColumns")
#define SETTING_NAME_TREE_LIST_SORT (PLUGIN_NAME L".TreeListSort")

// Distributions are re-read at this interval while the WSL tab is visible.
#define WSL_REFRESH_INTERVAL_MS 5000
// wsl.exe is killed if it has not exited within this time, e.g. when the WSL service is hung.
#define WSL_COMMAND_TIMEOUT_MS 10000

extern PPH_PLUGIN PluginInstance;

// wslutil.c

typedef enum _WSL_DISTRO_STATE
{
    WslDistroStateUnknown,
    WslDistroStateStopped,
    WslDistroStateRunning
} WSL_DISTRO_STATE;

typedef struct _WSL_DISTRO_ITEM
{
    PPH_STRING Id; // Registry key name, e.g. "{43779113-...}"
    PPH_STRING Name;
    PPH_STRING BasePath; // Win32 path of the distribution folder
    PPH_STRING VhdFileName; // Win32 path of the ext4 virtual disk (WSL 2 only)
    ULONG Version;
    BOOLEAN Default;
    WSL_DISTRO_STATE State;
    ULONG64 VhdSize; // Size of the virtual disk file, not of the file system inside it
} WSL_DISTRO_ITEM, *PWSL_DISTRO_ITEM;

// A point-in-time view of all registered distributions. Snapshots are immutable once
// published, so the UI can keep one while the provider builds the next.
typedef struct _WSL_SNAPSHOT
{
    PPH_LIST Distributions; // PWSL_DISTRO_ITEM
    // When the running query fails, every distribution is shown as unknown, not stopped.
    NTSTATUS RunningQueryStatus;
} WSL_SNAPSHOT, *PWSL_SNAPSHOT;

BOOLEAN WslIsInstalled(
    VOID
    );

VOID WslInitializeSnapshotType(
    VOID
    );

PWSL_SNAPSHOT WslQuerySnapshot(
    _In_ BOOLEAN VmRunning
    );

PCPH_STRINGREF WslGetDistroStateText(
    _In_ WSL_DISTRO_STATE State
    );

NTSTATUS WslRunCommand(
    _In_ PCPH_STRINGREF Arguments,
    _Out_opt_ PPH_STRING *Output
    );

NTSTATUS WslStartShell(
    _In_ PPH_STRING DistroName
    );

// wslprv.c

VOID WslStartProvider(
    VOID
    );

VOID WslStopProvider(
    VOID
    );

VOID WslSetProviderEnabled(
    _In_ BOOLEAN Enabled
    );

VOID WslRefreshProvider(
    VOID
    );

PPH_PROCESS_ITEM WslReferenceVmProcessItem(
    _Out_opt_ PULONG NumberOfCandidates
    );

// wsltab.c

VOID WslInitializeTab(
    VOID
    );

VOID NTAPI WslOnSnapshotUpdated(
    _In_ PVOID Parameter
    );

VOID WslOnProcessesUpdated(
    VOID
    );

#endif
