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

// The WSL section of System Information graphs the CPU usage and private bytes of the
// WSL 2 virtual machine process. The history comes from the process item itself, which
// the process provider already samples, so the section adds no sampling of its own.
// Everything here runs on the System Information window thread.

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
static HWND WslSysPanel = NULL;
static HWND WslSysPanelStateLabel = NULL;
static HWND WslSysPanelPidLabel = NULL;
static HWND WslSysPanelCpuLabel = NULL;
static HWND WslSysPanelPrivateLabel = NULL;

static PPH_PROCESS_ITEM WslSysVmProcessItem = NULL;
static ULONG WslSysVmCandidates = 0;

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
 * Fills graph data with the VM's kernel and user CPU history.
 *
 * \param DrawInfo The draw info whose LineDataCount has been set.
 * \param Data1 Receives the kernel CPU values.
 * \param Data2 Receives the user CPU values.
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
        Data1[i] = PhGetItemCircularBuffer_FLOAT(&WslSysVmProcessItem->CpuKernelHistory, i);
        Data2[i] = PhGetItemCircularBuffer_FLOAT(&WslSysVmProcessItem->CpuUserHistory, i);
    }
}

/**
 * Fills graph data with the VM's private bytes history, scaled to the largest value.
 *
 * \param DrawInfo The draw info whose LineDataCount has been set.
 * \param Data1 Receives the scaled values.
 */
static VOID WslpSysFillPrivateData(
    _Inout_ PPH_GRAPH_DRAW_INFO DrawInfo,
    _Out_writes_(DrawInfo->LineDataCount) PFLOAT Data1
    )
{
    FLOAT max = 1024 * 1024; // Minimum scaling of 1 MB

    for (ULONG i = 0; i < DrawInfo->LineDataCount; i++)
    {
        Data1[i] = (FLOAT)PhGetItemCircularBuffer_SIZE_T(&WslSysVmProcessItem->PrivateBytesHistory, i);

        if (max < Data1[i])
            max = Data1[i];
    }

    PhDivideSinglesBySingle(Data1, max, DrawInfo->LineDataCount);

    DrawInfo->LabelYFunction = PhSiSizeLabelYFunction;
    DrawInfo->LabelYFunctionParameter = max;
}

/**
 * Formats a graph tooltip.
 *
 * \param Value The value line, e.g. "CPU: 1.23%".
 * \param Index The history index the tooltip is for.
 * \return The tooltip text.
 */
static PPH_STRING WslpSysFormatTooltip(
    _In_ PPH_STRING Value,
    _In_ ULONG Index
    )
{
    PH_FORMAT format[3];

    PhInitFormatSR(&format[0], Value->sr);
    PhInitFormatC(&format[1], L'\n');
    PhInitFormatSR(&format[2], PH_AUTO_T(PH_STRING, PhGetStatisticsTimeString(NULL, Index))->sr);

    PhDereferenceObject(Value);

    return PhFormat(format, RTL_NUMBER_OF(format), 64);
}

/**
 * Gets the CPU tooltip for a history index.
 */
static PPH_STRING WslpSysGetCpuTooltip(
    _In_ ULONG Index
    )
{
    FLOAT cpu;

    cpu = PhGetItemCircularBuffer_FLOAT(&WslSysVmProcessItem->CpuKernelHistory, Index) +
        PhGetItemCircularBuffer_FLOAT(&WslSysVmProcessItem->CpuUserHistory, Index);

    return WslpSysFormatTooltip(PhFormatString(L"CPU: %.2f%%", cpu * 100), Index);
}

/**
 * Gets the private bytes tooltip for a history index.
 */
static PPH_STRING WslpSysGetPrivateTooltip(
    _In_ ULONG Index
    )
{
    SIZE_T privateBytes;

    privateBytes = PhGetItemCircularBuffer_SIZE_T(&WslSysVmProcessItem->PrivateBytesHistory, Index);

    return WslpSysFormatTooltip(PhFormatSize(privateBytes, ULONG_MAX), Index);
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
            WslSysSection->Parameters->ColorSetupFunction(drawInfo, PhGetIntegerSetting(SETTING_COLOR_CPU_KERNEL), PhGetIntegerSetting(SETTING_COLOR_CPU_USER), WslSysSection->Parameters->WindowDpi);

            PhGraphStateGetDrawInfo(&WslSysCpuGraphState, getDrawInfo, WslSysVmProcessItem ? WslSysVmProcessItem->CpuKernelHistory.Count : 0);

            if (!WslSysCpuGraphState.Valid)
            {
                if (WslSysVmProcessItem)
                    WslpSysFillCpuData(drawInfo, WslSysCpuGraphState.Data1, WslSysCpuGraphState.Data2);

                WslSysCpuGraphState.Valid = TRUE;
            }
        }
        break;
    case GCN_GETTOOLTIPTEXT:
        {
            PPH_GRAPH_GETTOOLTIPTEXT getTooltipText = (PPH_GRAPH_GETTOOLTIPTEXT)Header;

            if (WslSysVmProcessItem && getTooltipText->Index < getTooltipText->TotalCount)
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

            drawInfo->Flags = PH_GRAPH_USE_GRID_X | PH_GRAPH_USE_GRID_Y | PH_GRAPH_LABEL_MAX_Y;
            WslSysSection->Parameters->ColorSetupFunction(drawInfo, PhGetIntegerSetting(SETTING_COLOR_PRIVATE), 0, WslSysSection->Parameters->WindowDpi);

            PhGraphStateGetDrawInfo(&WslSysPrivateGraphState, getDrawInfo, WslSysVmProcessItem ? WslSysVmProcessItem->PrivateBytesHistory.Count : 0);

            if (!WslSysPrivateGraphState.Valid)
            {
                if (WslSysVmProcessItem)
                    WslpSysFillPrivateData(drawInfo, WslSysPrivateGraphState.Data1);

                WslSysPrivateGraphState.Valid = TRUE;
            }
        }
        break;
    case GCN_GETTOOLTIPTEXT:
        {
            PPH_GRAPH_GETTOOLTIPTEXT getTooltipText = (PPH_GRAPH_GETTOOLTIPTEXT)Header;

            if (WslSysVmProcessItem && getTooltipText->Index < getTooltipText->TotalCount)
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
 * Updates the panel below the graphs.
 */
static VOID WslpSysUpdatePanel(
    VOID
    )
{
    PhSetWindowText(WslSysPanelStateLabel, WslpSysGetVmStateText()->Buffer);

    if (WslSysVmProcessItem)
    {
        PhSetWindowText(WslSysPanelPidLabel, PhaFormatUInt64(HandleToUlong(WslSysVmProcessItem->ProcessId), FALSE)->Buffer);
        PhSetWindowText(WslSysPanelCpuLabel, PhaFormatString(L"%.2f%%", WslSysVmProcessItem->CpuUsage * 100)->Buffer);
        PhSetWindowText(WslSysPanelPrivateLabel, PhaFormatSize(WslSysVmProcessItem->VmCounters.PagefileUsage, ULONG_MAX)->Buffer);
    }
    else
    {
        PhSetWindowText(WslSysPanelPidLabel, L"");
        PhSetWindowText(WslSysPanelCpuLabel, L"");
        PhSetWindowText(WslSysPanelPrivateLabel, L"");
    }
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
            HWND groupBoxHandle;

            groupBoxHandle = GetDlgItem(WindowHandle, IDC_ZGROUPBOX_V);
            PhSetWindowStyle(groupBoxHandle, WS_CLIPSIBLINGS, WS_CLIPSIBLINGS);
            SetWindowPos(groupBoxHandle, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
            PhInitializeThemeWindowGroupBoxEx(groupBoxHandle);

            WslSysPanelStateLabel = GetDlgItem(WindowHandle, IDC_ZSTATE_V);
            WslSysPanelPidLabel = GetDlgItem(WindowHandle, IDC_ZPID_V);
            WslSysPanelCpuLabel = GetDlgItem(WindowHandle, IDC_ZCPU_V);
            WslSysPanelPrivateLabel = GetDlgItem(WindowHandle, IDC_ZPRIVATE_V);
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

            PhInitializeWindowTheme(WindowHandle, !!PhGetIntegerSetting(SETTING_ENABLE_THEME_SUPPORT));
        }
        break;
    case WM_DESTROY:
        {
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
        }
        return TRUE;
    case SysInfoTick:
        {
            // The VM process comes and goes with WSL, so look it up again on every tick.
            PhMoveReference(&WslSysVmProcessItem, WslReferenceVmProcessItem(&WslSysVmCandidates));

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

            // The summary graph shows CPU, like the built-in CPU section.
            drawInfo->Flags = PH_GRAPH_USE_GRID_X | PH_GRAPH_USE_GRID_Y | PH_GRAPH_USE_LINE_2;
            Section->Parameters->ColorSetupFunction(drawInfo, PhGetIntegerSetting(SETTING_COLOR_CPU_KERNEL), PhGetIntegerSetting(SETTING_COLOR_CPU_USER), Section->Parameters->WindowDpi);
            PhGetDrawInfoGraphBuffers(&Section->GraphState.Buffers, drawInfo, WslSysVmProcessItem ? WslSysVmProcessItem->CpuKernelHistory.Count : 0);

            if (!Section->GraphState.Valid)
            {
                if (WslSysVmProcessItem)
                    WslpSysFillCpuData(drawInfo, Section->GraphState.Data1, Section->GraphState.Data2);

                Section->GraphState.Valid = TRUE;
            }
        }
        return TRUE;
    case SysInfoGraphGetTooltipText:
        {
            PPH_SYSINFO_GRAPH_GET_TOOLTIP_TEXT getTooltipText = Parameter1;

            if (!WslSysVmProcessItem)
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
