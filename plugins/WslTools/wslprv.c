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
// tab costs nothing: no registry reads and no wsl.exe.

static HANDLE WslpProviderThreadHandle = NULL;
static HANDLE WslpProviderWakeEvent = NULL;
static LONG WslpProviderEnabled = FALSE;
static LONG WslpProviderStopping = FALSE;

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

            // The GUI thread takes ownership of the snapshot reference.
            SystemInformer_Invoke(WslOnSnapshotUpdated, snapshot);
        }

        NtWaitForSingleObject(WslpProviderWakeEvent, FALSE, &interval);
    }

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

    if (!NT_SUCCESS(PhCreateThreadEx(&WslpProviderThreadHandle, WslpProviderThread, NULL)))
    {
        NtClose(WslpProviderWakeEvent);
        WslpProviderWakeEvent = NULL;
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

    // Refresh right away when the tab becomes visible instead of waiting for the next interval.
    if (Enabled)
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
