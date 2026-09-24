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
// hands it to the GUI thread. It is enabled only while the WSL tab or the WSL page of System
// Information is visible, so otherwise it costs nothing: no registry reads, no wsl.exe and no
// process collectors.

typedef struct _WSL_COLLECTOR_ENTRY
{
    PPH_STRING Id; // Distribution id
    PWSL_COLLECTOR Collector; // NULL if it could not be started
} WSL_COLLECTOR_ENTRY, *PWSL_COLLECTOR_ENTRY;

static HANDLE WslpProviderThreadHandle = NULL;
static HANDLE WslpProviderWakeEvent = NULL;
static LONG WslpProviderEnabled = 0; // WSL_PROVIDER_* reasons it is enabled for
static LONG WslpProviderStopping = FALSE;
static PH_QUEUED_LOCK WslpLatestSnapshotLock = PH_QUEUED_LOCK_INIT;
static PWSL_SNAPSHOT WslpLatestSnapshot = NULL; // For readers other than the tab, e.g. System Information
static PPH_LIST WslpCollectors = NULL; // PWSL_COLLECTOR_ENTRY, used only by the provider thread

static CONST PH_STRINGREF WslpVmProcessNameWin11 = PH_STRINGREF_INIT(L"vmmemWSL");
static CONST PH_STRINGREF WslpVmProcessName = PH_STRINGREF_INIT(L"vmmem");

/**
 * Gets the "{GUID}" that follows "--vm-id " in a WSL helper process command line.
 */
static BOOLEAN WslpGetVmIdArgument(
    _In_opt_ PPH_STRING CommandLine,
    _Out_ PPH_STRINGREF VmId
    )
{
    static CONST PH_STRINGREF option = PH_STRINGREF_INIT(L"--vm-id ");
    ULONG_PTR index;
    ULONG_PTR close;

    if (!CommandLine || (index = PhFindStringInStringRef(&CommandLine->sr, &option, TRUE)) == SIZE_MAX)
        return FALSE;

    VmId->Buffer = CommandLine->Buffer + index + option.Length / sizeof(WCHAR);
    VmId->Length = CommandLine->Length - index * sizeof(WCHAR) - option.Length;

    if (VmId->Length == 0 || VmId->Buffer[0] != L'{' || (close = PhFindCharInStringRef(VmId, L'}', FALSE)) == SIZE_MAX)
        return FALSE;

    VmId->Length = (close + 1) * sizeof(WCHAR);

    return TRUE;
}

/**
 * Finds a process item by process ID in an enumeration.
 */
static PPH_PROCESS_ITEM WslpFindProcessItem(
    _In_reads_(NumberOfProcessItems) PPH_PROCESS_ITEM *ProcessItems,
    _In_ ULONG NumberOfProcessItems,
    _In_ HANDLE ProcessId
    )
{
    for (ULONG i = 0; i < NumberOfProcessItems; i++)
    {
        if (ProcessItems[i]->ProcessId == ProcessId)
            return ProcessItems[i];
    }

    return NULL;
}

/**
 * Gets the WSL VM's ID from the command line of a wslhost.exe that hosts a distribution
 * ("--distro-id ... --vm-id {id}").
 *
 * \return TRUE if such a process exists, which is only while a WSL 2 distribution runs.
 */
static BOOLEAN WslpGetWslVmId(
    _In_reads_(NumberOfProcessItems) PPH_PROCESS_ITEM *ProcessItems,
    _In_ ULONG NumberOfProcessItems,
    _Out_ PPH_STRINGREF VmId
    )
{
    static CONST PH_STRINGREF hostName = PH_STRINGREF_INIT(L"wslhost.exe");
    static CONST PH_STRINGREF distroOption = PH_STRINGREF_INIT(L"--distro-id");

    for (ULONG i = 0; i < NumberOfProcessItems; i++)
    {
        PPH_PROCESS_ITEM processItem = ProcessItems[i];

        if (processItem->ProcessName && processItem->CommandLine &&
            PhEqualStringRef(&processItem->ProcessName->sr, &hostName, TRUE) &&
            PhFindStringInStringRef(&processItem->CommandLine->sr, &distroOption, TRUE) != SIZE_MAX &&
            WslpGetVmIdArgument(processItem->CommandLine, VmId))
        {
            return TRUE;
        }
    }

    return FALSE;
}

typedef enum _WSLP_VM_KIND
{
    WslpVmKindUnknown, // No relay found, e.g. a Hyper-V VM
    WslpVmKindWsl, // Relay started by wslservice.exe
    WslpVmKindSession // Relay started by wslcsession.exe
} WSLP_VM_KIND;

/**
 * Determines which kind of VM a vmmem process hosts, and its VM ID.
 *
 * \return The kind of VM. VmId is only set when the kind is not unknown.
 * \remarks A vmmem process can only be opened with administrative rights, so its VM cannot
 * be read from it. Instead: about a second after a VM's vmmem appears, a wslrelay.exe with
 * "--vm-id {id}" starts for it, started by wslservice.exe for the WSL VM and by
 * wslcsession.exe for a WSLC session VM. The relay that started within a few seconds after
 * a vmmem therefore names its VM and its kind. This is inferred from observed process start
 * order and parents, not from a documented interface, so anything unexpected leaves the VM
 * unknown. A Hyper-V VM has no relay.
 */
static WSLP_VM_KIND WslpGetVmKind(
    _In_reads_(NumberOfProcessItems) PPH_PROCESS_ITEM *ProcessItems,
    _In_ ULONG NumberOfProcessItems,
    _In_ PPH_PROCESS_ITEM VmProcessItem,
    _Out_ PPH_STRINGREF VmId
    )
{
    static CONST PH_STRINGREF relayName = PH_STRINGREF_INIT(L"wslrelay.exe");
    static CONST PH_STRINGREF serviceName = PH_STRINGREF_INIT(L"wslservice.exe");
    static CONST PH_STRINGREF sessionName = PH_STRINGREF_INIT(L"wslcsession.exe");

    for (ULONG i = 0; i < NumberOfProcessItems; i++)
    {
        PPH_PROCESS_ITEM relayItem = ProcessItems[i];
        PPH_PROCESS_ITEM parentItem;
        LONG64 delay;

        if (!relayItem->ProcessName || !PhEqualStringRef(&relayItem->ProcessName->sr, &relayName, TRUE))
            continue;

        delay = relayItem->CreateTime.QuadPart - VmProcessItem->CreateTime.QuadPart;

        if (delay < 0 || delay > 5 * PH_TICKS_PER_SEC)
            continue;

        if (!(parentItem = WslpFindProcessItem(ProcessItems, NumberOfProcessItems, relayItem->ParentProcessId)) ||
            !parentItem->ProcessName || !WslpGetVmIdArgument(relayItem->CommandLine, VmId))
        {
            continue;
        }

        if (PhEqualStringRef(&parentItem->ProcessName->sr, &serviceName, TRUE))
            return WslpVmKindWsl;
        if (PhEqualStringRef(&parentItem->ProcessName->sr, &sessionName, TRUE))
            return WslpVmKindSession;
    }

    return WslpVmKindUnknown;
}

/**
 * Finds the processes of the WSL 2 VM and of a WSLC session VM.
 *
 * \param WslVm Receives the WSL VM process item, or NULL. The caller owns the reference.
 * \param SessionVm Receives the session VM process item, or NULL when there is none or
 * several could be meant. The caller owns the reference.
 * \param NumberOfCandidates Receives the number of processes that could be the WSL VM.
 * \remarks Windows 11 names the WSL VM process "vmmemWSL". Windows 10 names every VM
 * process "vmmem", including Hyper-V and WSLC session VMs, which WslpGetVmKind tells apart.
 * A single vmmem of unknown kind is taken for the WSL VM, as before WSLC existed.
 */
static VOID WslpFindVmProcessItems(
    _Out_opt_ PPH_PROCESS_ITEM *WslVm,
    _Out_opt_ PPH_PROCESS_ITEM *SessionVm,
    _Out_opt_ PULONG NumberOfCandidates
    )
{
    PPH_PROCESS_ITEM *processItems;
    ULONG numberOfProcessItems;
    PPH_PROCESS_ITEM wslVmItem = NULL;
    PPH_PROCESS_ITEM sessionVmItem = NULL;
    PPH_PROCESS_ITEM unknownVmItem = NULL;
    PPH_LIST candidates;
    PH_STRINGREF wslVmId;
    BOOLEAN haveWslVmId;
    ULONG sessionCandidates = 0;
    ULONG unknownCandidates = 0;

    PhEnumProcessItems(&processItems, &numberOfProcessItems);
    candidates = PhCreateList(2);

    for (ULONG i = 0; i < numberOfProcessItems; i++)
    {
        PPH_PROCESS_ITEM processItem = processItems[i];

        if (!processItem->ProcessName)
            continue;

        if (PhEqualStringRef(&processItem->ProcessName->sr, &WslpVmProcessNameWin11, TRUE))
            wslVmItem = processItem;
        else if (PhEqualStringRef(&processItem->ProcessName->sr, &WslpVmProcessName, TRUE))
            PhAddItemList(candidates, processItem);
    }

    haveWslVmId = WslpGetWslVmId(processItems, numberOfProcessItems, &wslVmId);

    for (ULONG i = 0; i < candidates->Count; i++)
    {
        PPH_PROCESS_ITEM candidate = candidates->Items[i];
        PH_STRINGREF vmId;

        switch (WslpGetVmKind(processItems, numberOfProcessItems, candidate, &vmId))
        {
        case WslpVmKindWsl:
            // With the WSL VM's ID known, it decides between several WSL service VMs.
            if (!wslVmItem && (!haveWslVmId || PhEqualStringRef(&vmId, &wslVmId, TRUE)))
                wslVmItem = candidate;
            break;
        case WslpVmKindSession:
            sessionVmItem = candidate;
            sessionCandidates++;
            break;
        default:
            unknownVmItem = candidate;
            unknownCandidates++;
            break;
        }
    }

    if (!wslVmItem && candidates->Count == 1 && unknownCandidates == 1)
        wslVmItem = unknownVmItem;

    if (sessionCandidates != 1)
        sessionVmItem = NULL;

    if (WslVm)
        *WslVm = wslVmItem ? PhReferenceObject(wslVmItem) : NULL;
    if (SessionVm)
        *SessionVm = sessionVmItem ? PhReferenceObject(sessionVmItem) : NULL;
    if (NumberOfCandidates)
        *NumberOfCandidates = wslVmItem && candidates->Count == 0 ? 1 : candidates->Count;

    PhDereferenceObject(candidates);
    PhDereferenceObjects(processItems, numberOfProcessItems);
    PhFree(processItems);
}

/**
 * Finds the process that hosts the WSL 2 virtual machine.
 *
 * \param NumberOfCandidates Receives the number of processes that could be the WSL VM.
 * \return The VM process item, or NULL if there is none or it cannot be identified. The
 * caller owns the reference.
 */
PPH_PROCESS_ITEM WslReferenceVmProcessItem(
    _Out_opt_ PULONG NumberOfCandidates
    )
{
    PPH_PROCESS_ITEM vmProcessItem;

    WslpFindVmProcessItems(&vmProcessItem, NULL, NumberOfCandidates);

    return vmProcessItem;
}

/**
 * Finds the process of the WSLC session VM.
 *
 * \return The process item, or NULL if there is no session VM or several. The caller owns
 * the reference.
 * \remarks Nothing links a session VM to a session name, so the caller may only attribute
 * the process to a session when exactly one session is running.
 */
PPH_PROCESS_ITEM WslReferenceSessionVmProcessItem(
    VOID
    )
{
    PPH_PROCESS_ITEM vmProcessItem;

    WslpFindVmProcessItems(NULL, &vmProcessItem, NULL);

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

            // A WSLC session is a VM of its own, so without any VM process none can be running.
            if (candidates != 0)
                snapshot->Sessions = WslQuerySessions();

            // The snapshot is complete and no longer changes, so other threads can read it.
            PhAcquireQueuedLockExclusive(&WslpLatestSnapshotLock);
            PhSetReference(&WslpLatestSnapshot, snapshot);
            PhReleaseQueuedLockExclusive(&WslpLatestSnapshotLock);

            // The GUI thread takes ownership of the snapshot reference.
            SystemInformer_Invoke(WslOnSnapshotUpdated, snapshot);
        }
        else
        {
            // Hidden: nothing is collected, and CPU usage starts fresh when shown again.
            WslpStopAllCollectors();
            WslResetSessionProcesses();

            PhAcquireQueuedLockExclusive(&WslpLatestSnapshotLock);
            PhClearReference(&WslpLatestSnapshot);
            PhReleaseQueuedLockExclusive(&WslpLatestSnapshotLock);
        }

        NtWaitForSingleObject(WslpProviderWakeEvent, FALSE, &interval);
    }

    WslpStopAllCollectors();
    WslResetSessionProcesses();

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
    PhClearReference(&WslpLatestSnapshot);
}

/**
 * Enables or disables snapshot collection for one reason. The provider runs while any
 * reason is enabled.
 *
 * \param Reason WSL_PROVIDER_TAB or WSL_PROVIDER_SYSINFO.
 * \param Enabled TRUE while that view is visible.
 */
VOID WslSetProviderEnabled(
    _In_ LONG Reason,
    _In_ BOOLEAN Enabled
    )
{
    if (Enabled)
        _InterlockedOr(&WslpProviderEnabled, Reason);
    else
        _InterlockedAnd(&WslpProviderEnabled, ~Reason);

    // Wake the thread either way: to refresh right away when the tab becomes visible, and to
    // stop the collectors right away when it is hidden.
    WslRefreshProvider();
}

/**
 * Gets the latest snapshot, for readers on threads other than the GUI thread.
 *
 * \return The snapshot, or NULL while the provider is not running. The caller owns the reference.
 */
PWSL_SNAPSHOT WslReferenceLatestSnapshot(
    VOID
    )
{
    PWSL_SNAPSHOT snapshot;

    PhAcquireQueuedLockShared(&WslpLatestSnapshotLock);

    if (snapshot = WslpLatestSnapshot)
        PhReferenceObject(snapshot);

    PhReleaseQueuedLockShared(&WslpLatestSnapshotLock);

    return snapshot;
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
