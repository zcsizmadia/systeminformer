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

// wslproc.c

typedef struct _WSL_LINUX_PROCESS
{
    ULONG ProcessId; // In the distribution's PID namespace
    ULONG ParentProcessId;
    ULONG64 StartTime; // Clock ticks after boot; with the PID it identifies a process across frames
    PPH_STRING Name;
    WCHAR State; // The state letter from /proc/<pid>/stat
    ULONG NumberOfThreads;
    ULONG64 ResidentBytes;
    FLOAT CpuUsage; // Fraction of all host processors, as PH_PROCESS_ITEM.CpuUsage
    BOOLEAN HaveCpuUsage; // FALSE until the process has been seen in two frames
} WSL_LINUX_PROCESS, *PWSL_LINUX_PROCESS;

// One sample of every process in a distribution.
typedef struct _WSL_PROCESS_FRAME
{
    PPH_LIST Processes; // PWSL_LINUX_PROCESS
    FLOAT CpuUsage; // Whole distribution, including processes that exited since the previous frame
    BOOLEAN HaveCpuUsage;
    ULONG64 ResidentBytes; // Sum over processes, so pages shared between processes count more than once
} WSL_PROCESS_FRAME, *PWSL_PROCESS_FRAME;

typedef struct _WSL_COLLECTOR *PWSL_COLLECTOR;

VOID WslInitializeProcessFrameType(
    VOID
    );

PWSL_COLLECTOR WslStartCollector(
    _In_ PPH_STRING DistroName
    );

VOID WslStopCollector(
    _In_ PWSL_COLLECTOR Collector
    );

BOOLEAN WslIsCollectorRunning(
    _In_ PWSL_COLLECTOR Collector
    );

PWSL_PROCESS_FRAME WslReferenceCollectorFrame(
    _In_ PWSL_COLLECTOR Collector
    );

PCPH_STRINGREF WslGetLinuxProcessStateText(
    _In_ WCHAR State
    );

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
    PWSL_PROCESS_FRAME Processes; // Latest process sample of a running distribution, or NULL
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

NTSTATUS WslCreateProcess(
    _In_ PCPH_STRINGREF Arguments,
    _Out_ PHANDLE ProcessHandle,
    _Out_ PHANDLE ReadHandle,
    _Out_ PHANDLE JobHandle
    );

NTSTATUS WslRunCommand(
    _In_ PCPH_STRINGREF Arguments,
    _Out_opt_ PPH_STRING *Output
    );

BOOLEAN WslIsSafeDistroName(
    _In_ PPH_STRING Name
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

// wslsys.c

VOID WslSystemInformationInitializing(
    _In_ PPH_PLUGIN_SYSINFO_POINTERS Pointers
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
