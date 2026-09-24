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
// User actions get longer: stopping a container that ignores SIGTERM waits out its grace period.
#define WSL_ACTION_TIMEOUT_MS 60000
// How long unloading waits for the provider thread, which can be inside one command.
#define WSL_PROVIDER_STOP_TIMEOUT_MS 3000

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
    PPH_STRING ContainerId; // Full ID of the WSLC container it runs in, or NULL
} WSL_LINUX_PROCESS, *PWSL_LINUX_PROCESS;

// One sample of every process in a distribution.
typedef struct _WSL_PROCESS_FRAME
{
    PPH_LIST Processes; // PWSL_LINUX_PROCESS
    FLOAT CpuUsage; // Whole distribution, including processes that exited since the previous frame
    BOOLEAN HaveCpuUsage;
    ULONG64 ResidentBytes; // Sum over processes, so pages shared between processes count more than once
    DOUBLE Uptime; // Seconds since the VM booted, when the frame was taken
    ULONG64 TicksPerSecond; // Unit of WSL_LINUX_PROCESS.StartTime
    PPH_STRING KernelRelease; // e.g. "6.18.40.1-microsoft-standard-WSL2"
    ULONG64 MemoryTotal; // Memory of the whole VM, from /proc/meminfo
    ULONG64 MemoryAvailable;
} WSL_PROCESS_FRAME, *PWSL_PROCESS_FRAME;

typedef struct _WSL_COLLECTOR *PWSL_COLLECTOR;
typedef struct _WSL_FRAME_PARSER *PWSL_FRAME_PARSER;

PWSL_FRAME_PARSER WslCreateFrameParser(
    VOID
    );

VOID WslDestroyFrameParser(
    _In_ PWSL_FRAME_PARSER Parser
    );

PWSL_PROCESS_FRAME WslParseFrameOutput(
    _In_ PWSL_FRAME_PARSER Parser,
    _In_ PPH_BYTES Output
    );

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

ULONG WslGetHostProcessorCount(
    VOID
    );

// wslc.c

typedef struct _WSL_CONTAINER
{
    PPH_STRING Id; // Short ID, as "wslc list" prints it
    PPH_STRING Name;
    PPH_STRING Image;
    PPH_STRING State; // e.g. "running", "exited"
    PPH_STRING Status; // e.g. "Up 5 minutes"
    PPH_STRING Ports;
    BOOLEAN Running;
    BOOLEAN HaveStats;
    FLOAT CpuUsage; // Fraction of all host processors, as PH_PROCESS_ITEM.CpuUsage
    ULONG64 MemoryBytes; // Memory usage as wslc stats reports it
    ULONG NumberOfProcesses;
} WSL_CONTAINER, *PWSL_CONTAINER;

typedef struct _WSL_SESSION
{
    ULONG Id;
    PPH_STRING Name; // Display name; wslc addresses sessions by it
    PPH_LIST Containers; // PWSL_CONTAINER
    NTSTATUS QueryStatus; // Result of listing the containers
    BOOLEAN HaveStats;
    FLOAT CpuUsage; // Sum over running containers
    ULONG64 MemoryBytes; // Sum over running containers
    PWSL_PROCESS_FRAME Processes; // Processes of the session VM, with their containers, or NULL
} WSL_SESSION, *PWSL_SESSION;

PPH_LIST WslQuerySessions(
    VOID
    );

VOID WslFreeSessions(
    _In_ PPH_LIST Sessions
    );

VOID WslResetSessionProcesses(
    VOID
    );

BOOLEAN WslIsSafeSessionName(
    _In_ PPH_STRING Name
    );

BOOLEAN WslIsSafeContainerId(
    _In_ PPH_STRING Id
    );

NTSTATUS WslStartContainerConsole(
    _In_ PPH_STRING SessionName,
    _In_ PPH_STRING ContainerId,
    _In_ BOOLEAN Logs
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
    PPH_STRING OsName; // e.g. "Ubuntu 26.04", from the Flavor and OsVersion values
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
    PPH_LIST Sessions; // PWSL_SESSION of running WSLC sessions, or NULL
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

PPH_STRING WslGetWslFileName(
    VOID
    );

PPH_STRING WslGetWslcFileName(
    VOID
    );

NTSTATUS WslCreateProcess(
    _In_ PPH_STRING FileName,
    _In_ PCPH_STRINGREF Arguments,
    _Out_ PHANDLE ProcessHandle,
    _Out_ PHANDLE ReadHandle,
    _Out_ PHANDLE JobHandle
    );

NTSTATUS WslRunCommand(
    _In_ PPH_STRING FileName,
    _In_ PCPH_STRINGREF Arguments,
    _Out_opt_ PPH_BYTES *Output
    );

NTSTATUS WslRunCommandEx(
    _In_ PPH_STRING FileName,
    _In_ PCPH_STRINGREF Arguments,
    _In_ ULONG TimeoutMs,
    _Out_opt_ PPH_BYTES *Output,
    _In_ BOOLEAN OutputOnFailure
    );

BOOLEAN WslIsSafeDistroName(
    _In_ PPH_STRING Name
    );

NTSTATUS WslStartShell(
    _In_ PPH_STRING DistroName
    );

// wslinsp.c

NTSTATUS WslShowContainerInspect(
    _In_ PPH_STRING SessionName,
    _In_ PPH_STRING ContainerId,
    _In_ PPH_STRING ContainerName
    );

// wslprv.c

VOID WslStartProvider(
    VOID
    );

VOID WslStopProvider(
    _In_ BOOLEAN Wait
    );

// Reasons for WslSetProviderEnabled.
#define WSL_PROVIDER_TAB 0x1
#define WSL_PROVIDER_SYSINFO 0x2

VOID WslSetProviderEnabled(
    _In_ LONG Reason,
    _In_ BOOLEAN Enabled
    );

PWSL_SNAPSHOT WslReferenceLatestSnapshot(
    VOID
    );

VOID WslRefreshProvider(
    VOID
    );

BOOLEAN WslIsVmProcessName(
    _In_ PPH_STRING ProcessName
    );

PPH_PROCESS_ITEM WslReferenceVmProcessItem(
    _Out_opt_ PULONG NumberOfCandidates
    );

PPH_PROCESS_ITEM WslReferenceSessionVmProcessItem(
    VOID
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

typedef enum _WSL_VM_SELECTION
{
    WslVmSelectionNone,
    WslVmSelectionWsl, // The WSL 2 VM row
    WslVmSelectionSession // The row of the single running WSLC session
} WSL_VM_SELECTION;

VOID WslSelectVmNode(
    _In_ WSL_VM_SELECTION Selection
    );

#endif
