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

#include "wsltools.h"

// The provider thread builds a snapshot every WSL_REFRESH_INTERVAL_MS while enabled, and
// hands it to the GUI thread. It is enabled only while the WSL tab is visible, so a hidden
// tab costs nothing: no registry reads, no wsl.exe and no process collectors.

typedef struct _WSL_COLLECTOR_ENTRY
{
    PPH_STRING Id; // Distribution id
    PWSL_COLLECTOR Collector; // NULL if it could not be started
} WSL_COLLECTOR_ENTRY, *PWSL_COLLECTOR_ENTRY;

static HANDLE WslpProviderThreadHandle = NULL;
static HANDLE WslpProviderWakeEvent = NULL;
static LONG WslpProviderEnabled = FALSE;
static LONG WslpProviderStopping = FALSE;
static PPH_LIST WslpCollectors = NULL; // PWSL_COLLECTOR_ENTRY, used only by the provider thread

static CONST PH_STRINGREF WslpVmProcessNameWin11 = PH_STRINGREF_INIT(L"vmmemWSL");
static CONST PH_STRINGREF WslpVmProcessName = PH_STRINGREF_INIT(L"vmmem");

/**
 * Finds the process that hosts the WSL 2 virtual machine.
 *
 * \param NumberOfCandidates Receives the number of processes that could be the WSL VM.
 * \return The VM process item, or NULL if there is none or it is ambiguous. The caller
 * owns the reference.
 * \remarks Windows 11 names the WSL VM process "vmmemWSL". Windows 10 names every VM
 * process "vmmem", including Hyper-V and WSLC session VMs, so there the process is only
 * returned when it is the single candidate.
 */
PPH_PROCESS_ITEM WslReferenceVmProcessItem(
    _Out_opt_ PULONG NumberOfCandidates
    )
{
    PPH_PROCESS_ITEM *processItems;
    ULONG numberOfProcessItems;
    PPH_PROCESS_ITEM vmProcessItem = NULL;
    PPH_PROCESS_ITEM candidateItem = NULL;
    ULONG candidates = 0;

    PhEnumProcessItems(&processItems, &numberOfProcessItems);

    for (ULONG i = 0; i < numberOfProcessItems; i++)
    {
        PPH_PROCESS_ITEM processItem = processItems[i];

        if (!processItem->ProcessName)
            continue;

        if (PhEqualStringRef(&processItem->ProcessName->sr, &WslpVmProcessNameWin11, TRUE))
        {
            vmProcessItem = processItem;
            candidates = 1;
            break;
        }

        if (PhEqualStringRef(&processItem->ProcessName->sr, &WslpVmProcessName, TRUE))
        {
            candidateItem = processItem;
            candidates++;
        }
    }

    if (!vmProcessItem && candidates == 1)
        vmProcessItem = candidateItem;

    if (vmProcessItem)
        PhReferenceObject(vmProcessItem);

    PhDereferenceObjects(processItems, numberOfProcessItems);
    PhFree(processItems);

    if (NumberOfCandidates)
        *NumberOfCandidates = candidates;

    return vmProcessItem;
}

/**
 * Stops a collector entry and frees it.
 */
static VOID WslpDestroyCollectorEntry(
    _In_ PWSL_COLLECTOR_ENTRY Entry
    )
{
    if (Entry->Collector)
        WslStopCollector(Entry->Collector);

    PhDereferenceObject(Entry->Id);
    PhFree(Entry);
}

/**
 * Stops every collector, e.g. when the tab is hidden.
 */
static VOID WslpStopAllCollectors(
    VOID
    )
{
    for (ULONG i = 0; i < WslpCollectors->Count; i++)
        WslpDestroyCollectorEntry(WslpCollectors->Items[i]);

    PhClearList(WslpCollectors);
}

/**
 * Matches the collectors to the running distributions of a snapshot, and attaches each
 * collector's latest frame to its distribution.
 *
 * \remarks A collector is only started for a distribution the snapshot just reported as
 * running, because starting one starts a stopped distribution. The distribution could still
 * stop in between, but WSL only stops a distribution after it has been idle for several
 * seconds, so the window is small. A collector whose wsl.exe exits on its own is not
 * restarted, for the same reason; it is replaced once the distribution has been seen
 * stopped, or when the tab is shown again.
 *
 * An unknown state, e.g. after the running query timed out once, keeps the collectors as
 * they are. Only WSL 2 distributions get a collector; the WSL 1 process tree and pipe
 * handling have not been verified.
 */
static VOID WslpUpdateCollectors(
    _In_ PWSL_SNAPSHOT Snapshot
    )
{
    // Stop the collectors of distributions that stopped or were unregistered.
    for (ULONG i = WslpCollectors->Count; i != 0; i--)
    {
        PWSL_COLLECTOR_ENTRY entry = WslpCollectors->Items[i - 1];
        BOOLEAN keep = FALSE;

        for (ULONG j = 0; j < Snapshot->Distributions->Count; j++)
        {
            PWSL_DISTRO_ITEM distro = Snapshot->Distributions->Items[j];

            if (PhEqualString(distro->Id, entry->Id, TRUE))
            {
                keep = distro->State != WslDistroStateStopped;
                break;
            }
        }

        if (!keep)
        {
            PhRemoveItemList(WslpCollectors, i - 1);
            WslpDestroyCollectorEntry(entry);
        }
    }

    for (ULONG i = 0; i < Snapshot->Distributions->Count; i++)
    {
        PWSL_DISTRO_ITEM distro = Snapshot->Distributions->Items[i];
        PWSL_COLLECTOR_ENTRY entry = NULL;

        if (distro->Version != 2)
            continue;

        for (ULONG j = 0; j < WslpCollectors->Count; j++)
        {
            if (PhEqualString(((PWSL_COLLECTOR_ENTRY)WslpCollectors->Items[j])->Id, distro->Id, TRUE))
            {
                entry = WslpCollectors->Items[j];
                break;
            }
        }

        // A new collector needs a distribution reported as running just now; an existing one
        // also keeps supplying frames while the state is unknown.
        if (!entry && distro->State != WslDistroStateRunning)
            continue;

        if (!entry)
        {
            entry = PhAllocateZero(sizeof(WSL_COLLECTOR_ENTRY));
            PhSetReference(&entry->Id, distro->Id);
            entry->Collector = WslStartCollector(distro->Name);
            PhAddItemList(WslpCollectors, entry);
        }

        // A frame from a collector that has exited would be stale.
        if (entry->Collector && WslIsCollectorRunning(entry->Collector))
            distro->Processes = WslReferenceCollectorFrame(entry->Collector);
    }
}

/**
 * Provider thread: builds snapshots while enabled and sleeps otherwise.
 */
_Function_class_(USER_THREAD_START_ROUTINE)
static NTSTATUS NTAPI WslpProviderThread(
    _In_ PVOID Parameter
    )
{
    LARGE_INTEGER interval;

    PhTimeoutFromMilliseconds(&interval, WSL_REFRESH_INTERVAL_MS);

    while (!ReadAcquire(&WslpProviderStopping))
    {
        if (ReadAcquire(&WslpProviderEnabled))
        {
            PPH_PROCESS_ITEM vmProcessItem;
            ULONG candidates;
            PWSL_SNAPSHOT snapshot;

            // Any candidate VM process is enough to allow the running query; a false positive
            // only costs one wsl.exe call, which does not start the VM.
            vmProcessItem = WslReferenceVmProcessItem(&candidates);
            PhClearReference(&vmProcessItem);

            snapshot = WslQuerySnapshot(candidates != 0);
            WslpUpdateCollectors(snapshot);

            // The GUI thread takes ownership of the snapshot reference.
            SystemInformer_Invoke(WslOnSnapshotUpdated, snapshot);
        }
        else if (WslpCollectors->Count != 0)
        {
            WslpStopAllCollectors();
        }

        NtWaitForSingleObject(WslpProviderWakeEvent, FALSE, &interval);
    }

    WslpStopAllCollectors();

    return STATUS_SUCCESS;
}

/**
 * Starts the provider thread. It stays idle until WslSetProviderEnabled(TRUE).
 */
VOID WslStartProvider(
    VOID
    )
{
    if (WslpProviderThreadHandle)
        return;

    if (!NT_SUCCESS(NtCreateEvent(&WslpProviderWakeEvent, EVENT_ALL_ACCESS, NULL, SynchronizationEvent, FALSE)))
        return;

    WslpCollectors = PhCreateList(4);

    if (!NT_SUCCESS(PhCreateThreadEx(&WslpProviderThreadHandle, WslpProviderThread, NULL)))
    {
        NtClose(WslpProviderWakeEvent);
        WslpProviderWakeEvent = NULL;
        PhClearReference(&WslpCollectors);
    }
}

/**
 * Stops the provider thread and waits for it to exit.
 */
VOID WslStopProvider(
    VOID
    )
{
    if (!WslpProviderThreadHandle)
        return;

    WriteRelease(&WslpProviderStopping, TRUE);
    NtSetEvent(WslpProviderWakeEvent, NULL);

    // A snapshot in progress can be waiting on wsl.exe, which is bounded by WSL_COMMAND_TIMEOUT_MS.
    NtWaitForSingleObject(WslpProviderThreadHandle, FALSE, NULL);

    NtClose(WslpProviderThreadHandle);
    WslpProviderThreadHandle = NULL;
    NtClose(WslpProviderWakeEvent);
    WslpProviderWakeEvent = NULL;
    PhClearReference(&WslpCollectors);
}

/**
 * Enables or disables snapshot collection.
 *
 * \param Enabled TRUE while the WSL tab is visible.
 */
VOID WslSetProviderEnabled(
    _In_ BOOLEAN Enabled
    )
{
    WriteRelease(&WslpProviderEnabled, Enabled);

    // Wake the thread either way: to refresh right away when the tab becomes visible, and to
    // stop the collectors right away when it is hidden.
    WslRefreshProvider();
}

/**
 * Asks the provider thread to build a snapshot now, e.g. after an action changed WSL state.
 */
VOID WslRefreshProvider(
    VOID
    )
{
    if (WslpProviderWakeEvent)
        NtSetEvent(WslpProviderWakeEvent, NULL);
}
