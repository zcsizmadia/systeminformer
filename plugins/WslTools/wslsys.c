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
#include <svcsup.h>

// The WSL section of System Information graphs the CPU usage and private bytes of the
// WSL 2 VM process and, stacked on top, of the WSLC session VM process. The history comes
// from the process items themselves, which the process provider already samples, so the
// section adds no sampling of its own. Everything here runs on the System Information
// window thread.

// The two VMs use System Informer's configurable graph colors: green for the WSL VM and
// blue for the session VM, as the default colors of these settings are.
#define WSL_SYS_COLOR_WSL_VM SETTING_COLOR_CPU_KERNEL
#define WSL_SYS_COLOR_SESSION_VM SETTING_COLOR_PHYSICAL

#define GRAPH_PADDING 3

static PPH_SYSINFO_SECTION WslSysSection = NULL;
static HWND WslSysDialog = NULL;
static PH_LAYOUT_MANAGER WslSysLayoutManager;
static RECT WslSysGraphMargin;
static RECT WslSysGraphMarginScaled;
static HWND WslSysCpuGraphHandle = NULL;
static HWND WslSysPrivateGraphHandle = NULL;
static PH_GRAPH_STATE WslSysCpuGraphState;
static PH_GRAPH_STATE WslSysPrivateGraphState;
// The panel below the graphs has four boxes side by side, each with four label/value rows.
#define WSL_SYS_BOXES 4
#define WSL_SYS_ROWS 4

typedef enum _WSL_SYS_BOX
{
    WslSysBoxWslVm,
    WslSysBoxSessionVm,
    WslSysBoxDistributions,
    WslSysBoxService
} WSL_SYS_BOX;

static CONST PCWSTR WslSysBoxTitles[WSL_SYS_BOXES] =
{
    L"VM: WSL",
    L"VM: WSLC session",
    L"Distributions",
    L"Service"
};

static CONST PCWSTR WslSysRowLabels[WSL_SYS_BOXES][WSL_SYS_ROWS] =
{
    { L"Process", L"CPU", L"Private bytes", L"Guest used / total" },
    { L"Process", L"CPU", L"Private bytes", L"Containers" },
    { L"Registered", L"Running", L"Default", L"VHD total" },
    { L"wslservice", L"WSLC sessions", L"VM idle timeout", L"Memory reclaim" },
};

static HWND WslSysPanel = NULL;
static PH_STRINGREF WslpServiceName = PH_STRINGREF_INIT(L"WSLService"); // not CONST: PhReferenceServiceItem takes a PPH_STRINGREF
static HWND WslSysPanelBoxes[WSL_SYS_BOXES];
static HWND WslSysPanelLabels[WSL_SYS_BOXES][WSL_SYS_ROWS];
static HWND WslSysPanelValues[WSL_SYS_BOXES][WSL_SYS_ROWS];

static PPH_PROCESS_ITEM WslSysVmProcessItem = NULL;
static ULONG WslSysVmCandidates = 0;
static PPH_PROCESS_ITEM WslSysSessionVmProcessItem = NULL;

/**
 * Gets the display text for the VM state.
 */
static PCPH_STRINGREF WslpSysGetVmStateText(
    VOID
    )
{
    if (WslSysVmProcessItem)
        return WslGetDistroStateText(WslDistroStateRunning);
    if (WslSysVmCandidates > 1)
        return WslGetDistroStateText(WslDistroStateUnknown);

    return WslGetDistroStateText(WslDistroStateStopped);
}

/**
 * Gets the number of history samples to draw: the longer history of the two VMs.
 */
static ULONG WslpSysGetHistoryCount(
    VOID
    )
{
    ULONG count = 0;

    if (WslSysVmProcessItem)
        count = WslSysVmProcessItem->CpuKernelHistory.Count;
    if (WslSysSessionVmProcessItem)
        count = max(count, WslSysSessionVmProcessItem->CpuKernelHistory.Count);

    return count;
}

/**
 * Gets the CPU usage of a VM process at a history index, or 0 before the process existed.
 */
static FLOAT WslpSysGetCpu(
    _In_opt_ PPH_PROCESS_ITEM ProcessItem,
    _In_ ULONG Index
    )
{
    if (!ProcessItem || Index >= ProcessItem->CpuKernelHistory.Count)
        return 0;

    return PhGetItemCircularBuffer_FLOAT(&ProcessItem->CpuKernelHistory, Index) +
        PhGetItemCircularBuffer_FLOAT(&ProcessItem->CpuUserHistory, Index);
}

/**
 * Gets the private bytes of a VM process at a history index, or 0 before the process existed.
 */
static SIZE_T WslpSysGetPrivateBytes(
    _In_opt_ PPH_PROCESS_ITEM ProcessItem,
    _In_ ULONG Index
    )
{
    if (!ProcessItem || Index >= ProcessItem->PrivateBytesHistory.Count)
        return 0;

    return PhGetItemCircularBuffer_SIZE_T(&ProcessItem->PrivateBytesHistory, Index);
}

/**
 * Fills graph data with the CPU history of the WSL VM, and of the session VM stacked on it.
 *
 * \param DrawInfo The draw info whose LineDataCount has been set.
 * \param Data1 Receives the WSL VM values.
 * \param Data2 Receives the session VM values.
 */
static VOID WslpSysFillCpuData(
    _In_ PPH_GRAPH_DRAW_INFO DrawInfo,
    _Out_writes_(DrawInfo->LineDataCount) PFLOAT Data1,
    _Out_writes_(DrawInfo->LineDataCount) PFLOAT Data2
    )
{
    // CPU usage is already a fraction of all processors, so the graph needs no scaling.
    for (ULONG i = 0; i < DrawInfo->LineDataCount; i++)
    {
        Data1[i] = WslpSysGetCpu(WslSysVmProcessItem, i);
        Data2[i] = WslpSysGetCpu(WslSysSessionVmProcessItem, i);
    }
}

/**
 * Fills graph data with the private bytes history of both VMs, scaled to the largest total.
 *
 * \param DrawInfo The draw info whose LineDataCount has been set.
 * \param Data1 Receives the WSL VM values.
 * \param Data2 Receives the session VM values.
 */
static VOID WslpSysFillPrivateData(
    _Inout_ PPH_GRAPH_DRAW_INFO DrawInfo,
    _Out_writes_(DrawInfo->LineDataCount) PFLOAT Data1,
    _Out_writes_(DrawInfo->LineDataCount) PFLOAT Data2
    )
{
    FLOAT max = 1024 * 1024; // Minimum scaling of 1 MB

    for (ULONG i = 0; i < DrawInfo->LineDataCount; i++)
    {
        Data1[i] = (FLOAT)WslpSysGetPrivateBytes(WslSysVmProcessItem, i);
        Data2[i] = (FLOAT)WslpSysGetPrivateBytes(WslSysSessionVmProcessItem, i);

        // The lines are stacked, so the scale must fit their sum.
        if (max < Data1[i] + Data2[i])
            max = Data1[i] + Data2[i];
    }

    PhDivideSinglesBySingle(Data1, max, DrawInfo->LineDataCount);
    PhDivideSinglesBySingle(Data2, max, DrawInfo->LineDataCount);

    DrawInfo->LabelYFunction = PhSiSizeLabelYFunction;
    DrawInfo->LabelYFunctionParameter = max;
}

/**
 * Formats a graph tooltip.
 *
 * \param Value The value lines, e.g. "WSL: 1.23%", or NULL. This function takes ownership of the string.
 * \param Index The history index the tooltip is for.
 * \return The tooltip text.
 */
static PPH_STRING WslpSysFormatTooltip(
    _In_opt_ PPH_STRING Value,
    _In_ ULONG Index
    )
{
    PH_FORMAT format[3];
    PPH_STRING text;

    PhInitFormatSR(&format[0], PhGetStringRef(Value));
    PhInitFormatC(&format[1], L'\n');
    PhInitFormatSR(&format[2], PH_AUTO_T(PH_STRING, PhGetStatisticsTimeString(NULL, Index))->sr);

    text = PhFormat(format, RTL_NUMBER_OF(format), 64);
    PhClearReference(&Value);

    return text;
}

/**
 * Gets the CPU tooltip for a history index, with a line per VM.
 */
static PPH_STRING WslpSysGetCpuTooltip(
    _In_ ULONG Index
    )
{
    if (!WslSysSessionVmProcessItem)
        return WslpSysFormatTooltip(PhFormatString(L"WSL: %.2f%%", WslpSysGetCpu(WslSysVmProcessItem, Index) * 100), Index);

    return WslpSysFormatTooltip(PhFormatString(
        L"WSL: %.2f%%\nSession: %.2f%%",
        WslpSysGetCpu(WslSysVmProcessItem, Index) * 100,
        WslpSysGetCpu(WslSysSessionVmProcessItem, Index) * 100
        ), Index);
}

/**
 * Gets the private bytes tooltip for a history index, with a line per VM.
 */
static PPH_STRING WslpSysGetPrivateTooltip(
    _In_ ULONG Index
    )
{
    PPH_STRING wslText = PhFormatSize(WslpSysGetPrivateBytes(WslSysVmProcessItem, Index), ULONG_MAX);
    PPH_STRING text;

    if (WslSysSessionVmProcessItem)
    {
        PPH_STRING sessionText = PhFormatSize(WslpSysGetPrivateBytes(WslSysSessionVmProcessItem, Index), ULONG_MAX);

        text = PhFormatString(L"WSL: %s\nSession: %s", wslText->Buffer, sessionText->Buffer);
        PhDereferenceObject(sessionText);
    }
    else
    {
        text = PhFormatString(L"WSL: %s", wslText->Buffer);
    }

    PhDereferenceObject(wslText);

    return WslpSysFormatTooltip(text, Index);
}

/**
 * Handles notifications from the CPU graph on the section page.
 */
static VOID WslpSysNotifyCpuGraph(
    _In_ NMHDR *Header
    )
{
    switch (Header->code)
    {
    case GCN_GETDRAWINFO:
        {
            PPH_GRAPH_GETDRAWINFO getDrawInfo = (PPH_GRAPH_GETDRAWINFO)Header;
            PPH_GRAPH_DRAW_INFO drawInfo = getDrawInfo->DrawInfo;

            drawInfo->Flags = PH_GRAPH_USE_GRID_X | PH_GRAPH_USE_GRID_Y | PH_GRAPH_USE_LINE_2;
            WslSysSection->Parameters->ColorSetupFunction(drawInfo, PhGetIntegerSetting(WSL_SYS_COLOR_WSL_VM), PhGetIntegerSetting(WSL_SYS_COLOR_SESSION_VM), WslSysSection->Parameters->WindowDpi);

            PhGraphStateGetDrawInfo(&WslSysCpuGraphState, getDrawInfo, WslpSysGetHistoryCount());

            if (!WslSysCpuGraphState.Valid)
            {
                WslpSysFillCpuData(drawInfo, WslSysCpuGraphState.Data1, WslSysCpuGraphState.Data2);

                WslSysCpuGraphState.Valid = TRUE;
            }
        }
        break;
    case GCN_GETTOOLTIPTEXT:
        {
            PPH_GRAPH_GETTOOLTIPTEXT getTooltipText = (PPH_GRAPH_GETTOOLTIPTEXT)Header;

            if ((WslSysVmProcessItem || WslSysSessionVmProcessItem) && getTooltipText->Index < getTooltipText->TotalCount)
            {
                if (WslSysCpuGraphState.TooltipIndex != getTooltipText->Index)
                    PhMoveReference(&WslSysCpuGraphState.TooltipText, WslpSysGetCpuTooltip(getTooltipText->Index));

                getTooltipText->Text = PhGetStringRef(WslSysCpuGraphState.TooltipText);
            }
        }
        break;
    }
}

/**
 * Handles notifications from the private bytes graph on the section page.
 */
static VOID WslpSysNotifyPrivateGraph(
    _In_ NMHDR *Header
    )
{
    switch (Header->code)
    {
    case GCN_GETDRAWINFO:
        {
            PPH_GRAPH_GETDRAWINFO getDrawInfo = (PPH_GRAPH_GETDRAWINFO)Header;
            PPH_GRAPH_DRAW_INFO drawInfo = getDrawInfo->DrawInfo;

            drawInfo->Flags = PH_GRAPH_USE_GRID_X | PH_GRAPH_USE_GRID_Y | PH_GRAPH_LABEL_MAX_Y | PH_GRAPH_USE_LINE_2;
            WslSysSection->Parameters->ColorSetupFunction(drawInfo, PhGetIntegerSetting(WSL_SYS_COLOR_WSL_VM), PhGetIntegerSetting(WSL_SYS_COLOR_SESSION_VM), WslSysSection->Parameters->WindowDpi);

            PhGraphStateGetDrawInfo(&WslSysPrivateGraphState, getDrawInfo, WslpSysGetHistoryCount());

            if (!WslSysPrivateGraphState.Valid)
            {
                WslpSysFillPrivateData(drawInfo, WslSysPrivateGraphState.Data1, WslSysPrivateGraphState.Data2);

                WslSysPrivateGraphState.Valid = TRUE;
            }
        }
        break;
    case GCN_GETTOOLTIPTEXT:
        {
            PPH_GRAPH_GETTOOLTIPTEXT getTooltipText = (PPH_GRAPH_GETTOOLTIPTEXT)Header;

            if ((WslSysVmProcessItem || WslSysSessionVmProcessItem) && getTooltipText->Index < getTooltipText->TotalCount)
            {
                if (WslSysPrivateGraphState.TooltipIndex != getTooltipText->Index)
                    PhMoveReference(&WslSysPrivateGraphState.TooltipText, WslpSysGetPrivateTooltip(getTooltipText->Index));

                getTooltipText->Text = PhGetStringRef(WslSysPrivateGraphState.TooltipText);
            }
        }
        break;
    }
}

_Function_class_(PH_GRAPH_MESSAGE_CALLBACK)
static BOOLEAN WslpSysGraphMessageCallback(
    _In_ HWND WindowHandle,
    _In_ ULONG Message,
    _In_ PVOID Parameter1,
    _In_ PVOID Parameter2,
    _In_ PVOID Context
    )
{
    NMHDR *header = (NMHDR *)Parameter1;

    if (header->hwndFrom == WslSysCpuGraphHandle)
        WslpSysNotifyCpuGraph(header);
    else if (header->hwndFrom == WslSysPrivateGraphHandle)
        WslpSysNotifyPrivateGraph(header);

    return TRUE;
}

/**
 * Creates the graphs on the section page.
 */
static VOID WslpSysCreateGraphs(
    VOID
    )
{
    PH_GRAPH_CREATEPARAMS graphCreateParams;

    memset(&graphCreateParams, 0, sizeof(PH_GRAPH_CREATEPARAMS));
    graphCreateParams.Size = sizeof(PH_GRAPH_CREATEPARAMS);
    graphCreateParams.Callback = WslpSysGraphMessageCallback;

    WslSysCpuGraphHandle = PhCreateWindow(
        PH_GRAPH_CLASSNAME,
        NULL,
        WS_VISIBLE | WS_CHILD | WS_BORDER | WS_CLIPSIBLINGS,
        0,
        0,
        0,
        0,
        WslSysDialog,
        NULL,
        NULL,
        &graphCreateParams
        );
    Graph_SetTooltip(WslSysCpuGraphHandle, TRUE);

    WslSysPrivateGraphHandle = PhCreateWindow(
        PH_GRAPH_CLASSNAME,
        NULL,
        WS_VISIBLE | WS_CHILD | WS_BORDER | WS_CLIPSIBLINGS,
        0,
        0,
        0,
        0,
        WslSysDialog,
        NULL,
        NULL,
        &graphCreateParams
        );
    Graph_SetTooltip(WslSysPrivateGraphHandle, TRUE);
}

/**
 * Places the graph labels and graphs one above the other, splitting the height evenly.
 */
static VOID WslpSysLayoutGraphs(
    _In_ HWND WindowHandle
    )
{
    RECT clientRect;
    RECT labelRect;
    RECT marginRect;
    LONG graphWidth;
    LONG graphHeight;
    LONG graphPadding;
    HDWP deferHandle;
    LONG y;

    marginRect = WslSysGraphMarginScaled;
    graphPadding = PhScaleToDisplay(GRAPH_PADDING, WslSysSection->Parameters->WindowDpi);

    PhGetClientRect(WindowHandle, &clientRect);
    PhGetClientRect(GetDlgItem(WindowHandle, IDC_CPU_L), &labelRect);
    graphWidth = clientRect.right - marginRect.left - marginRect.right;
    graphHeight = (clientRect.bottom - marginRect.top - marginRect.bottom - labelRect.bottom * 2 - graphPadding * 3) / 2;

    deferHandle = BeginDeferWindowPos(4);
    y = marginRect.top;

    deferHandle = DeferWindowPos(deferHandle, GetDlgItem(WindowHandle, IDC_CPU_L), NULL, marginRect.left, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
    y += labelRect.bottom + graphPadding;

    deferHandle = DeferWindowPos(deferHandle, WslSysCpuGraphHandle, NULL, marginRect.left, y, graphWidth, graphHeight, SWP_NOACTIVATE | SWP_NOZORDER);
    y += graphHeight + graphPadding;

    deferHandle = DeferWindowPos(deferHandle, GetDlgItem(WindowHandle, IDC_PRIVATE_L), NULL, marginRect.left, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
    y += labelRect.bottom + graphPadding;

    // The last graph takes the remaining height so rounding never leaves a gap.
    deferHandle = DeferWindowPos(deferHandle, WslSysPrivateGraphHandle, NULL, marginRect.left, y, graphWidth, clientRect.bottom - marginRect.bottom - y, SWP_NOACTIVATE | SWP_NOZORDER);

    EndDeferWindowPos(deferHandle);
}

/**
 * Marks both page graphs for redraw.
 */
static VOID WslpSysInvalidateGraphs(
    VOID
    )
{
    WslSysCpuGraphState.Valid = FALSE;
    WslSysCpuGraphState.TooltipIndex = ULONG_MAX;
    WslSysPrivateGraphState.Valid = FALSE;
    WslSysPrivateGraphState.TooltipIndex = ULONG_MAX;
}

/**
 * Gets the WSL version from the version resource of wslservice.exe, e.g. "2.9.12.0".
 *
 * \return The version, or NULL. The string is cached for the lifetime of the process.
 * \remarks This reads a file instead of running "wsl --version", whose output is localized.
 */
static PPH_STRING WslpSysGetWslVersion(
    VOID
    )
{
    static PH_INITONCE initOnce = PH_INITONCE_INIT;
    static PPH_STRING version = NULL;

    if (PhBeginInitOnce(&initOnce))
    {
        static CONST PH_STRINGREF path = PH_STRINGREF_INIT(L"%ProgramW6432%\\WSL\\wslservice.exe");
        PPH_STRING fileName;
        PH_IMAGE_VERSION_INFO versionInfo;

        if (fileName = PhExpandEnvironmentStrings(&path))
        {
            if (NT_SUCCESS(PhInitializeImageVersionInfo(&versionInfo, fileName->Buffer)))
            {
                PhSetReference(&version, versionInfo.FileVersion);
                PhDeleteImageVersionInfo(&versionInfo);
            }

            PhDereferenceObject(fileName);
        }

        PhEndInitOnce(&initOnce);
    }

    return version;
}

/**
 * Reads a setting from %USERPROFILE%\.wslconfig.
 *
 * \param Section The section, e.g. L"wsl2".
 * \param Key The key, e.g. L"vmIdleTimeout".
 * \return The value, or NULL when it is not set and WSL uses its default.
 * \remarks .wslconfig is an INI file, so the profile API reads it.
 */
static PPH_STRING WslpSysGetWslConfigValue(
    _In_ PCWSTR Section,
    _In_ PCWSTR Key
    )
{
    static CONST PH_STRINGREF path = PH_STRINGREF_INIT(L"%USERPROFILE%\\.wslconfig");
    PPH_STRING fileName;
    PPH_STRING value = NULL;
    WCHAR buffer[64];

    if (fileName = PhExpandEnvironmentStrings(&path))
    {
        if (GetPrivateProfileString(Section, Key, L"", buffer, RTL_NUMBER_OF(buffer), fileName->Buffer) != 0)
            value = PhCreateString(buffer);

        PhDereferenceObject(fileName);
    }

    return value;
}

/**
 * Gets the latest process frame of a running WSL 2 distribution, for data of the whole VM
 * such as its kernel and memory.
 */
static PWSL_PROCESS_FRAME WslpSysGetVmFrame(
    _In_opt_ PWSL_SNAPSHOT Snapshot
    )
{
    for (ULONG i = 0; Snapshot && i < Snapshot->Distributions->Count; i++)
    {
        PWSL_DISTRO_ITEM distro = Snapshot->Distributions->Items[i];

        if (distro->Version == 2 && distro->Processes)
            return distro->Processes;
    }

    return NULL;
}

/**
 * Sets a value of the panel.
 *
 * \param Value The text. This function takes ownership of the string; NULL clears the value.
 */
static VOID WslpSysSetValue(
    _In_ WSL_SYS_BOX Box,
    _In_ ULONG Row,
    _In_opt_ PPH_STRING Value
    )
{
    PhSetWindowText(WslSysPanelValues[Box][Row], PhGetStringOrEmpty(Value));
    PhClearReference(&Value);
}

/**
 * Sets the process, CPU and private bytes rows of a VM box.
 */
static VOID WslpSysSetVmValues(
    _In_ WSL_SYS_BOX Box,
    _In_opt_ PPH_PROCESS_ITEM ProcessItem
    )
{
    if (ProcessItem)
    {
        WslpSysSetValue(Box, 0, PhFormatString(L"%s (%lu)", PhGetStringOrEmpty(ProcessItem->ProcessName), HandleToUlong(ProcessItem->ProcessId)));
        WslpSysSetValue(Box, 1, PhFormatString(L"%.2f%%", ProcessItem->CpuUsage * 100));
        WslpSysSetValue(Box, 2, PhFormatSize(ProcessItem->VmCounters.PagefileUsage, ULONG_MAX));
    }
    else
    {
        WslpSysSetValue(Box, 0, NULL);
        WslpSysSetValue(Box, 1, NULL);
        WslpSysSetValue(Box, 2, NULL);
    }
}

/**
 * Updates the panel and the header.
 *
 * \remarks Distribution, session, kernel and guest memory values come from the provider's
 * latest snapshot, which exists while this page (or the WSL tab) is visible; they stay empty
 * until the first snapshot arrives.
 */
static VOID WslpSysUpdatePanel(
    VOID
    )
{
    PWSL_SNAPSHOT snapshot = WslReferenceLatestSnapshot();
    PWSL_PROCESS_FRAME vmFrame = WslpSysGetVmFrame(snapshot);
    PWSL_SESSION session = snapshot && snapshot->Sessions && snapshot->Sessions->Count == 1 ? snapshot->Sessions->Items[0] : NULL;
    PPH_SERVICE_ITEM serviceItem;
    PPH_STRING value;
    PPH_STRING version;
    ULONG numberOfVms;

    // VM: WSL
    WslpSysSetVmValues(WslSysBoxWslVm, WslSysVmProcessItem);

    if (vmFrame && vmFrame->MemoryTotal != 0)
    {
        PPH_STRING used = PhFormatSize(vmFrame->MemoryTotal - min(vmFrame->MemoryAvailable, vmFrame->MemoryTotal), ULONG_MAX);
        PPH_STRING total = PhFormatSize(vmFrame->MemoryTotal, ULONG_MAX);

        WslpSysSetValue(WslSysBoxWslVm, 3, PhFormatString(L"%s / %s", used->Buffer, total->Buffer));
        PhDereferenceObject(used);
        PhDereferenceObject(total);
    }
    else
    {
        WslpSysSetValue(WslSysBoxWslVm, 3, NULL);
    }

    // VM: session. The process is only attributed while exactly one session runs.
    PhSetWindowText(WslSysPanelBoxes[WslSysBoxSessionVm], session ? PhaFormatString(L"VM: %s", session->Name->Buffer)->Buffer : WslSysBoxTitles[WslSysBoxSessionVm]);
    WslpSysSetVmValues(WslSysBoxSessionVm, session ? WslSysSessionVmProcessItem : NULL);

    if (session && session->State == WslDistroStateRunning)
    {
        ULONG running = 0;

        for (ULONG i = 0; i < session->Containers->Count; i++)
            running += ((PWSL_CONTAINER)session->Containers->Items[i])->Running;

        WslpSysSetValue(WslSysBoxSessionVm, 3, PhFormatString(L"%lu running, %lu exited", running, session->Containers->Count - running));
    }
    else if (session)
    {
        // Containers are not queried while the session VM is stopped, as that would start it.
        WslpSysSetValue(WslSysBoxSessionVm, 3, PhCreateString2(WslGetDistroStateText(session->State)));
    }
    else
    {
        WslpSysSetValue(WslSysBoxSessionVm, 3, NULL);
    }

    // Distributions
    if (snapshot)
    {
        PPH_STRING defaultName = NULL;
        ULONG running = 0;
        ULONG64 vhdTotal = 0;

        for (ULONG i = 0; i < snapshot->Distributions->Count; i++)
        {
            PWSL_DISTRO_ITEM distro = snapshot->Distributions->Items[i];

            running += distro->State == WslDistroStateRunning;
            vhdTotal += distro->VhdSize;

            if (distro->Default)
                defaultName = distro->Name;
        }

        WslpSysSetValue(WslSysBoxDistributions, 0, PhFormatUInt64(snapshot->Distributions->Count, FALSE));
        WslpSysSetValue(WslSysBoxDistributions, 1, NT_SUCCESS(snapshot->RunningQueryStatus) ? PhFormatUInt64(running, FALSE) : PhCreateString(L"Unknown"));
        WslpSysSetValue(WslSysBoxDistributions, 2, defaultName ? PhReferenceObject(defaultName) : NULL);
        WslpSysSetValue(WslSysBoxDistributions, 3, PhFormatSize(vhdTotal, ULONG_MAX));
    }
    else
    {
        for (ULONG i = 0; i < WSL_SYS_ROWS; i++)
            WslpSysSetValue(WslSysBoxDistributions, i, NULL);
    }

    // Service
    if (serviceItem = PhReferenceServiceItem(&WslpServiceName))
    {
        WslpSysSetValue(WslSysBoxService, 0, PhCreateString2(PhGetServiceStateString(serviceItem->State)));
        PhDereferenceObject(serviceItem);
    }
    else
    {
        WslpSysSetValue(WslSysBoxService, 0, PhCreateString(L"Not installed"));
    }

    WslpSysSetValue(WslSysBoxService, 1, snapshot && snapshot->Sessions ? PhFormatUInt64(snapshot->Sessions->Count, FALSE) : NULL);

    // Settings that .wslconfig does not set are shown as "default" rather than as a value this
    // plugin would have to assume.
    if (value = WslpSysGetWslConfigValue(L"wsl2", L"vmIdleTimeout"))
        PhMoveReference(&value, PhFormatString(L"%s ms", value->Buffer));
    WslpSysSetValue(WslSysBoxService, 2, value ? value : PhCreateString(L"default"));

    if (!(value = WslpSysGetWslConfigValue(L"experimental", L"autoMemoryReclaim")))
        value = WslpSysGetWslConfigValue(L"wsl2", L"autoMemoryReclaim");
    WslpSysSetValue(WslSysBoxService, 3, value ? value : PhCreateString(L"default"));

    // Header: "2 VMs, WSL 2.9.12.0, kernel 6.18.40.1", separated by middle dots.
    numberOfVms = (WslSysVmProcessItem ? 1 : 0) + (snapshot && snapshot->Sessions ? snapshot->Sessions->Count : (WslSysSessionVmProcessItem ? 1 : 0));
    version = WslpSysGetWslVersion();

    {
        PPH_STRING versionText = version ? PhFormatString(L" \u00b7 WSL %s", version->Buffer) : PhReferenceEmptyString();
        PPH_STRING kernelText = PhReferenceEmptyString();

        if (vmFrame && vmFrame->KernelRelease)
        {
            PH_STRINGREF kernel;
            PH_STRINGREF rest;

            // "6.18.40.1-microsoft-standard-WSL2" is shown as "6.18.40.1".
            PhSplitStringRefAtChar(&vmFrame->KernelRelease->sr, L'-', &kernel, &rest);
            PhMoveReference(&kernelText, PhFormatString(L" \u00b7 kernel %.*s", (INT)(kernel.Length / sizeof(WCHAR)), kernel.Buffer));
        }

        value = PhFormatString(L"%lu %s%s%s", numberOfVms, numberOfVms == 1 ? L"VM" : L"VMs", PhGetStringOrEmpty(versionText), PhGetStringOrEmpty(kernelText));
        PhSetWindowText(GetDlgItem(WslSysDialog, IDC_HEADER), PhGetStringOrEmpty(value));

        PhClearReference(&value);
        PhClearReference(&versionText);
        PhClearReference(&kernelText);
    }

    PhClearReference(&snapshot);
}

/**
 * Places the four boxes side by side over the width of the panel, with the rows of each box.
 */
static VOID WslpSysLayoutPanel(
    _In_ HWND WindowHandle
    )
{
    RECT clientRect;
    LONG padding = PhScaleToDisplay(6, WslSysSection->Parameters->WindowDpi);
    LONG rowHeight = PhScaleToDisplay(15, WslSysSection->Parameters->WindowDpi);
    LONG topPadding = PhScaleToDisplay(16, WslSysSection->Parameters->WindowDpi);
    LONG boxWidth;

    PhGetClientRect(WindowHandle, &clientRect);
    boxWidth = (clientRect.right - padding * (WSL_SYS_BOXES - 1)) / WSL_SYS_BOXES;

    for (ULONG box = 0; box < WSL_SYS_BOXES; box++)
    {
        LONG left = box * (boxWidth + padding);
        LONG innerWidth = boxWidth - padding * 2;

        MoveWindow(WslSysPanelBoxes[box], left, 0, boxWidth, clientRect.bottom, FALSE);

        for (ULONG row = 0; row < WSL_SYS_ROWS; row++)
        {
            LONG top = topPadding + row * rowHeight;

            MoveWindow(WslSysPanelLabels[box][row], left + padding, top, innerWidth / 2, rowHeight, FALSE);
            MoveWindow(WslSysPanelValues[box][row], left + padding + innerWidth / 2, top, innerWidth - innerWidth / 2, rowHeight, FALSE);
        }
    }

    InvalidateRect(WindowHandle, NULL, TRUE);
}

static INT_PTR CALLBACK WslpSysPanelDialogProc(
    _In_ HWND WindowHandle,
    _In_ UINT WindowMessage,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
    )
{
    switch (WindowMessage)
    {
    case WM_INITDIALOG:
        {
            HFONT font = GetWindowFont(WindowHandle);

            // The controls are made here from the tables, rather than listed in the template.
            for (ULONG box = 0; box < WSL_SYS_BOXES; box++)
            {
                WslSysPanelBoxes[box] = CreateWindow(L"BUTTON", WslSysBoxTitles[box], WS_CHILD | WS_VISIBLE | BS_GROUPBOX | WS_CLIPSIBLINGS, 0, 0, 0, 0, WindowHandle, NULL, NULL, NULL);
                SetWindowFont(WslSysPanelBoxes[box], font, FALSE);
                PhInitializeThemeWindowGroupBoxEx(WslSysPanelBoxes[box]);

                for (ULONG row = 0; row < WSL_SYS_ROWS; row++)
                {
                    WslSysPanelLabels[box][row] = CreateWindow(L"STATIC", WslSysRowLabels[box][row], WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, 0, 0, 0, 0, WindowHandle, NULL, NULL, NULL);
                    WslSysPanelValues[box][row] = CreateWindow(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_RIGHT | SS_ENDELLIPSIS, 0, 0, 0, 0, WindowHandle, NULL, NULL, NULL);
                    SetWindowFont(WslSysPanelLabels[box][row], font, FALSE);
                    SetWindowFont(WslSysPanelValues[box][row], font, FALSE);
                }

                // The group box is drawn behind its rows.
                SetWindowPos(WslSysPanelBoxes[box], HWND_BOTTOM, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
            }
        }
        break;
    case WM_SIZE:
        {
            WslpSysLayoutPanel(WindowHandle);
        }
        break;
    case WM_CTLCOLORBTN:
        return HANDLE_WM_CTLCOLORBTN(WindowHandle, wParam, lParam, PhWindowThemeControlColor);
    case WM_CTLCOLORDLG:
        return HANDLE_WM_CTLCOLORDLG(WindowHandle, wParam, lParam, PhWindowThemeControlColor);
    case WM_CTLCOLORSTATIC:
        return HANDLE_WM_CTLCOLORSTATIC(WindowHandle, wParam, lParam, PhWindowThemeControlColor);
    }

    return FALSE;
}

static INT_PTR CALLBACK WslpSysDialogProc(
    _In_ HWND WindowHandle,
    _In_ UINT WindowMessage,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
    )
{
    switch (WindowMessage)
    {
    case WM_INITDIALOG:
        {
            PPH_LAYOUT_ITEM graphItem;
            PPH_LAYOUT_ITEM panelItem;
            RECT margin;

            PhInitializeGraphState(&WslSysCpuGraphState);
            PhInitializeGraphState(&WslSysPrivateGraphState);

            WslSysDialog = WindowHandle;
            PhSetWindowExStyle(WindowHandle, WS_EX_TRANSPARENT, 0);

            PhInitializeLayoutManager(&WslSysLayoutManager, WindowHandle);
            graphItem = PhAddLayoutItem(&WslSysLayoutManager, GetDlgItem(WindowHandle, IDC_GRAPH_LAYOUT), NULL, PH_ANCHOR_ALL);
            panelItem = PhAddLayoutItem(&WslSysLayoutManager, GetDlgItem(WindowHandle, IDC_PANEL_LAYOUT), NULL, PH_ANCHOR_LEFT | PH_ANCHOR_RIGHT | PH_ANCHOR_BOTTOM);
            PhAddLayoutItem(&WslSysLayoutManager, GetDlgItem(WindowHandle, IDC_HEADER), NULL, PH_ANCHOR_TOP | PH_ANCHOR_RIGHT);
            WslSysGraphMargin = graphItem->Margin;
            WslSysGraphMarginScaled = WslSysGraphMargin;
            PhGetMarginDpiValue(&WslSysGraphMarginScaled, WslSysSection->Parameters->WindowDpi, TRUE);

            SetWindowFont(GetDlgItem(WindowHandle, IDC_TITLE), WslSysSection->Parameters->LargeFont, FALSE);

            WslSysPanel = PhCreateDialog(PluginInstance->DllBase, MAKEINTRESOURCE(IDD_SYSINFO_WSLPANEL), WindowHandle, WslpSysPanelDialogProc, NULL);
            ShowWindow(WslSysPanel, SW_SHOW);

            margin = panelItem->Margin;
            PhGetMarginDpiValue(&margin, WslSysSection->Parameters->WindowDpi, TRUE);
            PhAddLayoutItemEx(&WslSysLayoutManager, WslSysPanel, NULL, PH_ANCHOR_LEFT | PH_ANCHOR_RIGHT | PH_ANCHOR_BOTTOM, &margin);

            WslpSysCreateGraphs();
            WslpSysUpdatePanel();

            // The panel needs distributions, sessions and guest data, which only the provider reads.
            WslSetProviderEnabled(WSL_PROVIDER_SYSINFO, TRUE);

            PhInitializeWindowTheme(WindowHandle, !!PhGetIntegerSetting(SETTING_ENABLE_THEME_SUPPORT));
        }
        break;
    case WM_DESTROY:
        {
            WslSetProviderEnabled(WSL_PROVIDER_SYSINFO, FALSE);
            PhDeleteLayoutManager(&WslSysLayoutManager);
        }
        break;
    case WM_DPICHANGED_AFTERPARENT:
        {
            WslSysGraphMarginScaled = WslSysGraphMargin;
            PhGetMarginDpiValue(&WslSysGraphMarginScaled, WslSysSection->Parameters->WindowDpi, TRUE);

            if (WslSysSection->Parameters->LargeFont)
                SetWindowFont(GetDlgItem(WindowHandle, IDC_TITLE), WslSysSection->Parameters->LargeFont, FALSE);

            WslpSysInvalidateGraphs();
            PhLayoutManagerUpdate(&WslSysLayoutManager, WslSysSection->Parameters->WindowDpi);
            PhLayoutManagerLayout(&WslSysLayoutManager);
            WslpSysLayoutGraphs(WindowHandle);
        }
        break;
    case WM_SIZE:
        {
            WslpSysInvalidateGraphs();
            PhLayoutManagerLayout(&WslSysLayoutManager);
            WslpSysLayoutGraphs(WindowHandle);
        }
        break;
    case WM_CTLCOLORBTN:
        return HANDLE_WM_CTLCOLORBTN(WindowHandle, wParam, lParam, PhWindowThemeControlColor);
    case WM_CTLCOLORDLG:
        return HANDLE_WM_CTLCOLORDLG(WindowHandle, wParam, lParam, PhWindowThemeControlColor);
    case WM_CTLCOLORSTATIC:
        return HANDLE_WM_CTLCOLORSTATIC(WindowHandle, wParam, lParam, PhWindowThemeControlColor);
    }

    return FALSE;
}

_Function_class_(PH_SYSINFO_SECTION_CALLBACK)
static BOOLEAN WslpSysSectionCallback(
    _In_ PPH_SYSINFO_SECTION Section,
    _In_ PH_SYSINFO_SECTION_MESSAGE Message,
    _In_ PVOID Parameter1,
    _In_ PVOID Parameter2
    )
{
    switch (Message)
    {
    case SysInfoDestroy:
        {
            WslSetProviderEnabled(WSL_PROVIDER_SYSINFO, FALSE);

            if (WslSysDialog)
            {
                PhDeleteGraphState(&WslSysCpuGraphState);
                PhDeleteGraphState(&WslSysPrivateGraphState);

                // SysInfoViewChanging checks the handles, so they must not outlive the dialog.
                WslSysCpuGraphHandle = NULL;
                WslSysPrivateGraphHandle = NULL;
                WslSysDialog = NULL;
            }

            PhClearReference(&WslSysVmProcessItem);
            PhClearReference(&WslSysSessionVmProcessItem);
        }
        return TRUE;
    case SysInfoTick:
        {
            // The VM process comes and goes with WSL, so look it up again on every tick.
            PhMoveReference(&WslSysVmProcessItem, WslReferenceVmProcessItem(&WslSysVmCandidates, NULL));
            PhMoveReference(&WslSysSessionVmProcessItem, WslReferenceSessionVmProcessItem());

            if (WslSysDialog)
            {
                WslpSysInvalidateGraphs();
                Graph_Update(WslSysCpuGraphHandle);
                Graph_Update(WslSysPrivateGraphHandle);
                WslpSysUpdatePanel();
            }
        }
        return TRUE;
    case SysInfoViewChanging:
        {
            PH_SYSINFO_VIEW_TYPE view = (PH_SYSINFO_VIEW_TYPE)PtrToUlong(Parameter1);
            PPH_SYSINFO_SECTION section = (PPH_SYSINFO_SECTION)Parameter2;

            // Every section gets this message on every view change. The section dialog is only
            // hidden when another view is shown, so the provider follows the view, not the dialog.
            WslSetProviderEnabled(WSL_PROVIDER_SYSINFO, view == SysInfoSectionView && section == Section);

            if (view == SysInfoSummaryView || section != Section)
                return TRUE;

            if (WslSysCpuGraphHandle)
            {
                WslpSysInvalidateGraphs();
                Graph_Draw(WslSysCpuGraphHandle);
                Graph_Draw(WslSysPrivateGraphHandle);
            }
        }
        return TRUE;
    case SysInfoCreateDialog:
        {
            PPH_SYSINFO_CREATE_DIALOG createDialog = Parameter1;

            createDialog->Instance = PluginInstance->DllBase;
            createDialog->Template = MAKEINTRESOURCE(IDD_SYSINFO_WSL);
            createDialog->DialogProc = WslpSysDialogProc;
        }
        return TRUE;
    case SysInfoGraphGetDrawInfo:
        {
            PPH_GRAPH_DRAW_INFO drawInfo = Parameter1;

            // The summary graph shows CPU of both VMs, stacked, like the built-in CPU section.
            drawInfo->Flags = PH_GRAPH_USE_GRID_X | PH_GRAPH_USE_GRID_Y | PH_GRAPH_USE_LINE_2;
            Section->Parameters->ColorSetupFunction(drawInfo, PhGetIntegerSetting(WSL_SYS_COLOR_WSL_VM), PhGetIntegerSetting(WSL_SYS_COLOR_SESSION_VM), Section->Parameters->WindowDpi);
            PhGetDrawInfoGraphBuffers(&Section->GraphState.Buffers, drawInfo, WslpSysGetHistoryCount());

            if (!Section->GraphState.Valid)
            {
                WslpSysFillCpuData(drawInfo, Section->GraphState.Data1, Section->GraphState.Data2);

                Section->GraphState.Valid = TRUE;
            }
        }
        return TRUE;
    case SysInfoGraphGetTooltipText:
        {
            PPH_SYSINFO_GRAPH_GET_TOOLTIP_TEXT getTooltipText = Parameter1;

            if (!WslSysVmProcessItem && !WslSysSessionVmProcessItem)
                return FALSE;

            PhMoveReference(&Section->GraphState.TooltipText, WslpSysGetCpuTooltip(getTooltipText->Index));
            getTooltipText->Text = PhGetStringRef(Section->GraphState.TooltipText);
        }
        return TRUE;
    case SysInfoGraphDrawPanel:
        {
            PPH_SYSINFO_DRAW_PANEL drawPanel = Parameter1;

            drawPanel->Title = PhCreateString(L"WSL");

            if (WslSysVmProcessItem)
            {
                PH_FORMAT format[4];

                // CPU: %.2f%%\n%s
                PhInitFormatS(&format[0], L"CPU: ");
                PhInitFormatF(&format[1], WslSysVmProcessItem->CpuUsage * 100, 2);
                PhInitFormatS(&format[2], L"%\n");
                PhInitFormatSize(&format[3], WslSysVmProcessItem->VmCounters.PagefileUsage);

                drawPanel->SubTitle = PhFormat(format, RTL_NUMBER_OF(format), 0);
            }
            else
            {
                drawPanel->SubTitle = PhCreateString2(WslpSysGetVmStateText());
            }
        }
        return TRUE;
    }

    return FALSE;
}

/**
 * Adds the WSL section to the System Information window.
 *
 * \param Pointers The plugin system information pointers.
 */
VOID WslSystemInformationInitializing(
    _In_ PPH_PLUGIN_SYSINFO_POINTERS Pointers
    )
{
    PH_SYSINFO_SECTION section;

    memset(&section, 0, sizeof(PH_SYSINFO_SECTION));
    PhInitializeStringRef(&section.Name, L"WSL");
    section.Flags = 0;
    section.Callback = WslpSysSectionCallback;

    WslSysSection = Pointers->CreateSection(&section);
}
