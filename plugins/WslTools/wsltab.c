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

// The tab is a two-level tree: the WSL 2 virtual machine with its distributions below it,
// and WSL 1 distributions at the root because they do not run in the VM.

typedef enum _WSL_TREE_COLUMN
{
    WSLTNC_NAME,
    WSLTNC_STATE,
    WSLTNC_VERSION,
    WSLTNC_PID,
    WSLTNC_CPU,
    WSLTNC_PRIVATEBYTES,
    WSLTNC_VHDSIZE,
    WSLTNC_LOCATION,
    WSLTNC_MAXIMUM
} WSL_TREE_COLUMN;

typedef enum _WSL_NODE_TYPE
{
    WslNodeTypeVm,
    WslNodeTypeDistro
} WSL_NODE_TYPE;

typedef struct _WSL_NODE
{
    PH_TREENEW_NODE Node;
    WSL_NODE_TYPE Type;
    PPH_STRING Id; // Distribution id; NULL for the VM node
    PWSL_DISTRO_ITEM Distro; // Owned by WslCurrentSnapshot; NULL for the VM node
    PPH_STRING NameText; // Distribution name, followed by " *" for the default one
    PPH_LIST Children; // VM node only
    BOOLEAN Seen; // Scratch flag while a snapshot is applied

    PH_STRINGREF TextCache[WSLTNC_MAXIMUM];
    WCHAR PidText[PH_INT32_STR_LEN_1];
    WCHAR CpuText[PH_INT32_STR_LEN_1];
    WCHAR PrivateBytesText[PH_INT64_STR_LEN_1];
    WCHAR VhdSizeText[PH_INT64_STR_LEN_1];
    WCHAR VersionText[PH_INT32_STR_LEN_1];
} WSL_NODE, *PWSL_NODE;

typedef struct _WSL_ACTION_CONTEXT
{
    PPH_STRING Arguments;
    PPH_STRING Description;
} WSL_ACTION_CONTEXT, *PWSL_ACTION_CONTEXT;

static PPH_MAIN_TAB_PAGE WslPage = NULL;
static HWND WslTreeNewHandle = NULL;
static ULONG WslTreeNewSortColumn = WSLTNC_NAME;
static PH_SORT_ORDER WslTreeNewSortOrder = AscendingSortOrder;
static BOOLEAN WslTabSelected = FALSE;

static PWSL_SNAPSHOT WslCurrentSnapshot = NULL;
static PWSL_NODE WslVmNode = NULL;
static PPH_LIST WslDistroNodes = NULL; // PWSL_NODE, all distributions
static PPH_LIST WslRootNodes = NULL; // PWSL_NODE, the VM node and WSL 1 distributions
static PPH_PROCESS_ITEM WslVmProcessItem = NULL;
static ULONG WslVmCandidates = 0;

static CONST PH_STRINGREF WslPageText = PH_STRINGREF_INIT(L"WSL");
static CONST PH_STRINGREF WslVmNodeText = PH_STRINGREF_INIT(L"WSL 2 virtual machine");
static CONST PH_STRINGREF WslEmptyText = PH_STRINGREF_INIT(L"No WSL distributions are registered for this user.");
static CONST PH_STRINGREF WslAmbiguousVmText = PH_STRINGREF_INIT(L"Several virtual machine processes exist; the WSL one cannot be identified.");
static CONST PH_STRINGREF WslDefaultMarker = PH_STRINGREF_INIT(L" *");
static CONST PH_STRINGREF WslDefaultTooltipText = PH_STRINGREF_INIT(L"Default distribution");

/**
 * Creates a tree node.
 *
 * \param Type The node type.
 * \param Id The distribution id, or NULL for the VM node.
 * \return The new node.
 */
static PWSL_NODE WslpCreateNode(
    _In_ WSL_NODE_TYPE Type,
    _In_opt_ PPH_STRING Id
    )
{
    PWSL_NODE node;

    node = PhAllocateZero(sizeof(WSL_NODE));
    PhInitializeTreeNewNode(&node->Node);
    node->Node.TextCache = node->TextCache;
    node->Node.TextCacheSize = WSLTNC_MAXIMUM;
    node->Type = Type;

    if (Id)
        PhSetReference(&node->Id, Id);
    if (Type == WslNodeTypeVm)
        node->Children = PhCreateList(4);

    return node;
}

/**
 * Frees a tree node. Children are not freed; they are owned by WslDistroNodes.
 *
 * \param Node The node.
 */
static VOID WslpDestroyNode(
    _In_ PWSL_NODE Node
    )
{
    PhClearReference(&Node->Id);
    PhClearReference(&Node->NameText);
    PhClearReference(&Node->Children);
    PhFree(Node);
}

/**
 * Finds the node of a distribution.
 *
 * \param Id The distribution id.
 * \return The node, or NULL if there is none.
 */
static PWSL_NODE WslpFindDistroNode(
    _In_ PPH_STRING Id
    )
{
    for (ULONG i = 0; i < WslDistroNodes->Count; i++)
    {
        PWSL_NODE node = WslDistroNodes->Items[i];

        if (PhEqualString(node->Id, Id, TRUE))
            return node;
    }

    return NULL;
}

/**
 * Clears the cached text and color of a node so they are recomputed on the next paint.
 *
 * \param Node The node.
 */
static VOID WslpInvalidateNode(
    _In_ PWSL_NODE Node
    )
{
    memset(Node->TextCache, 0, sizeof(Node->TextCache));
    PhInvalidateTreeNewNode(&Node->Node, TN_CACHE_COLOR);
}

/**
 * Applies a new snapshot from the provider. Runs on the GUI thread.
 *
 * \param Parameter The snapshot. This function takes ownership of the reference.
 * \remarks Nodes are matched by distribution id, so selection and expansion survive refreshes.
 */
VOID NTAPI WslOnSnapshotUpdated(
    _In_ PVOID Parameter
    )
{
    PWSL_SNAPSHOT snapshot = Parameter;
    BOOLEAN hasWsl2 = FALSE;

    // The tab window can be gone while a snapshot is still queued.
    if (!WslTreeNewHandle)
    {
        PhDereferenceObject(snapshot);
        return;
    }

    for (ULONG i = 0; i < WslDistroNodes->Count; i++)
        ((PWSL_NODE)WslDistroNodes->Items[i])->Seen = FALSE;

    for (ULONG i = 0; i < snapshot->Distributions->Count; i++)
    {
        PWSL_DISTRO_ITEM distro = snapshot->Distributions->Items[i];
        PWSL_NODE node;

        if (!(node = WslpFindDistroNode(distro->Id)))
        {
            node = WslpCreateNode(WslNodeTypeDistro, distro->Id);
            PhAddItemList(WslDistroNodes, node);
        }

        node->Distro = distro;
        node->Seen = TRUE;

        if (distro->Default)
            PhMoveReference(&node->NameText, PhConcatStringRef2(&distro->Name->sr, &WslDefaultMarker));
        else
            PhSetReference(&node->NameText, distro->Name);

        if (distro->Version == 2)
            hasWsl2 = TRUE;
    }

    // Remove nodes of distributions that were unregistered.
    for (ULONG i = WslDistroNodes->Count; i != 0; i--)
    {
        PWSL_NODE node = WslDistroNodes->Items[i - 1];

        if (!node->Seen)
        {
            PhRemoveItemList(WslDistroNodes, i - 1);
            WslpDestroyNode(node);
        }
    }

    // The previous snapshot owned the distribution items the nodes pointed to.
    PhMoveReference(&WslCurrentSnapshot, snapshot);

    if (hasWsl2 && !WslVmNode)
        WslVmNode = WslpCreateNode(WslNodeTypeVm, NULL);

    PhClearList(WslRootNodes);

    if (WslVmNode)
    {
        PhClearList(WslVmNode->Children);
        PhAddItemList(WslRootNodes, WslVmNode);
    }

    for (ULONG i = 0; i < WslDistroNodes->Count; i++)
    {
        PWSL_NODE node = WslDistroNodes->Items[i];

        if (WslVmNode && node->Distro->Version == 2)
            PhAddItemList(WslVmNode->Children, node);
        else
            PhAddItemList(WslRootNodes, node);
    }

    if (WslVmNode)
        WslpInvalidateNode(WslVmNode);

    for (ULONG i = 0; i < WslDistroNodes->Count; i++)
        WslpInvalidateNode(WslDistroNodes->Items[i]);

    TreeNew_NodesStructured(WslTreeNewHandle);
}

/**
 * Refreshes the VM process columns. Runs on the GUI thread after each process provider update.
 */
VOID WslOnProcessesUpdated(
    VOID
    )
{
    if (!WslTreeNewHandle || !WslTabSelected)
        return;

    PhMoveReference(&WslVmProcessItem, WslReferenceVmProcessItem(&WslVmCandidates));

    if (WslVmNode)
    {
        WslpInvalidateNode(WslVmNode);
        InvalidateRect(WslTreeNewHandle, NULL, FALSE);
    }
}

/**
 * Compares two nodes for the current sort column. The VM node always sorts first,
 * so only distribution nodes reach the column comparison.
 */
static int __cdecl WslpCompareNodes(
    _In_ void *Context,
    _In_ const void *Elem1,
    _In_ const void *Elem2
    )
{
    PWSL_NODE node1 = *(PWSL_NODE *)Elem1;
    PWSL_NODE node2 = *(PWSL_NODE *)Elem2;
    PWSL_DISTRO_ITEM distro1 = node1->Distro;
    PWSL_DISTRO_ITEM distro2 = node2->Distro;
    int sortResult = 0;

    if (node1->Type != node2->Type)
        return node1->Type == WslNodeTypeVm ? -1 : 1;

    switch (WslTreeNewSortColumn)
    {
    case WSLTNC_STATE:
        sortResult = uintcmp(distro1->State, distro2->State);
        break;
    case WSLTNC_VERSION:
        sortResult = uintcmp(distro1->Version, distro2->Version);
        break;
    case WSLTNC_VHDSIZE:
        sortResult = uint64cmp(distro1->VhdSize, distro2->VhdSize);
        break;
    case WSLTNC_LOCATION:
        sortResult = PhCompareStringWithNull(distro1->BasePath, distro2->BasePath, TRUE);
        break;
    }

    if (sortResult == 0)
        sortResult = PhCompareString(distro1->Name, distro2->Name, TRUE);

    return PhModifySort(sortResult, WslTreeNewSortOrder);
}

/**
 * Sorts a node list for the current sort column.
 */
static VOID WslpSortNodes(
    _In_ PPH_LIST Nodes
    )
{
    qsort_s(Nodes->Items, Nodes->Count, sizeof(PVOID), WslpCompareNodes, NULL);
}

/**
 * Formats the cell text of the VM node.
 */
static VOID WslpGetVmCellText(
    _In_ PWSL_NODE Node,
    _Inout_ PPH_TREENEW_GET_CELL_TEXT GetCellText
    )
{
    PPH_PROCESS_ITEM processItem = WslVmProcessItem;
    SIZE_T returnLength;
    PH_FORMAT format;

    switch (GetCellText->Id)
    {
    case WSLTNC_NAME:
        GetCellText->Text = WslVmNodeText;
        break;
    case WSLTNC_STATE:
        if (processItem)
            GetCellText->Text = *WslGetDistroStateText(WslDistroStateRunning);
        else if (WslVmCandidates > 1)
            GetCellText->Text = *WslGetDistroStateText(WslDistroStateUnknown);
        else
            GetCellText->Text = *WslGetDistroStateText(WslDistroStateStopped);
        break;
    case WSLTNC_VERSION:
        PhInitializeStringRef(&GetCellText->Text, L"2");
        break;
    case WSLTNC_PID:
        if (processItem)
        {
            PhInitFormatU(&format, HandleToUlong(processItem->ProcessId));

            if (PhFormatToBuffer(&format, 1, Node->PidText, sizeof(Node->PidText), &returnLength))
            {
                GetCellText->Text.Buffer = Node->PidText;
                GetCellText->Text.Length = returnLength - sizeof(UNICODE_NULL);
            }
        }
        break;
    case WSLTNC_CPU:
        if (processItem && processItem->CpuUsage >= 0.0001f)
        {
            PhInitFormatF(&format, processItem->CpuUsage * 100, 2);

            if (PhFormatToBuffer(&format, 1, Node->CpuText, sizeof(Node->CpuText), &returnLength))
            {
                GetCellText->Text.Buffer = Node->CpuText;
                GetCellText->Text.Length = returnLength - sizeof(UNICODE_NULL);
            }
        }
        break;
    case WSLTNC_PRIVATEBYTES:
        if (processItem)
        {
            PhInitFormatSize(&format, processItem->VmCounters.PagefileUsage);

            if (PhFormatToBuffer(&format, 1, Node->PrivateBytesText, sizeof(Node->PrivateBytesText), &returnLength))
            {
                GetCellText->Text.Buffer = Node->PrivateBytesText;
                GetCellText->Text.Length = returnLength - sizeof(UNICODE_NULL);
            }
        }
        break;
    }
}

/**
 * Formats the cell text of a distribution node.
 */
static VOID WslpGetDistroCellText(
    _In_ PWSL_NODE Node,
    _Inout_ PPH_TREENEW_GET_CELL_TEXT GetCellText
    )
{
    PWSL_DISTRO_ITEM distro = Node->Distro;
    SIZE_T returnLength;
    PH_FORMAT format;

    switch (GetCellText->Id)
    {
    case WSLTNC_NAME:
        GetCellText->Text = PhGetStringRef(Node->NameText);
        break;
    case WSLTNC_STATE:
        GetCellText->Text = *WslGetDistroStateText(distro->State);
        break;
    case WSLTNC_VERSION:
        if (distro->Version != 0)
        {
            PhInitFormatU(&format, distro->Version);

            if (PhFormatToBuffer(&format, 1, Node->VersionText, sizeof(Node->VersionText), &returnLength))
            {
                GetCellText->Text.Buffer = Node->VersionText;
                GetCellText->Text.Length = returnLength - sizeof(UNICODE_NULL);
            }
        }
        break;
    case WSLTNC_VHDSIZE:
        if (distro->VhdSize != 0)
        {
            PhInitFormatSize(&format, distro->VhdSize);

            if (PhFormatToBuffer(&format, 1, Node->VhdSizeText, sizeof(Node->VhdSizeText), &returnLength))
            {
                GetCellText->Text.Buffer = Node->VhdSizeText;
                GetCellText->Text.Length = returnLength - sizeof(UNICODE_NULL);
            }
        }
        break;
    case WSLTNC_LOCATION:
        GetCellText->Text = PhGetStringRef(distro->BasePath);
        break;
    }
}

/**
 * Gets the selected node, or NULL if nothing or more than one node is selected.
 */
static PWSL_NODE WslpGetSelectedNode(
    VOID
    )
{
    PWSL_NODE selectedNode = NULL;
    ULONG count = TreeNew_GetFlatNodeCount(WslTreeNewHandle);

    for (ULONG i = 0; i < count; i++)
    {
        PWSL_NODE node = (PWSL_NODE)TreeNew_GetFlatNode(WslTreeNewHandle, i);

        if (node->Node.Selected)
        {
            if (selectedNode)
                return NULL;

            selectedNode = node;
        }
    }

    return selectedNode;
}

/**
 * Shows an action error. Runs on the GUI thread.
 *
 * \param Parameter The action context. This function frees it.
 */
static VOID NTAPI WslpShowActionError(
    _In_ PVOID Parameter
    )
{
    PWSL_ACTION_CONTEXT context = Parameter;

    PhShowStatus(SystemInformer_GetWindowHandle(), PhGetString(context->Description), STATUS_UNSUCCESSFUL, 0);

    PhDereferenceObject(context->Arguments);
    PhDereferenceObject(context->Description);
    PhFree(context);
}

/**
 * Runs a wsl.exe action off the GUI thread; "--shutdown" can take several seconds.
 */
_Function_class_(USER_THREAD_START_ROUTINE)
static NTSTATUS NTAPI WslpActionThread(
    _In_ PVOID Parameter
    )
{
    PWSL_ACTION_CONTEXT context = Parameter;
    NTSTATUS status;

    status = WslRunCommand(&context->Arguments->sr, NULL);

    WslRefreshProvider();

    if (NT_SUCCESS(status))
    {
        PhDereferenceObject(context->Arguments);
        PhDereferenceObject(context->Description);
        PhFree(context);
    }
    else
    {
        SystemInformer_Invoke(WslpShowActionError, context);
    }

    return STATUS_SUCCESS;
}

/**
 * Starts a wsl.exe action in the background.
 *
 * \param Arguments The wsl.exe arguments.
 * \param Description The error text shown if the action fails.
 */
static VOID WslpStartAction(
    _In_ PPH_STRING Arguments,
    _In_ PCWSTR Description
    )
{
    PWSL_ACTION_CONTEXT context;

    context = PhAllocateZero(sizeof(WSL_ACTION_CONTEXT));
    context->Arguments = Arguments;
    context->Description = PhCreateString(Description);

    if (!NT_SUCCESS(PhCreateThread2(WslpActionThread, context)))
    {
        PhDereferenceObject(context->Arguments);
        PhDereferenceObject(context->Description);
        PhFree(context);
    }
}

/**
 * Handles a command from the context menu or keyboard.
 *
 * \param WindowHandle The tree window handle.
 * \param Id The command id.
 */
static VOID WslpHandleCommand(
    _In_ HWND WindowHandle,
    _In_ ULONG Id
    )
{
    PWSL_NODE node = WslpGetSelectedNode();

    switch (Id)
    {
    case ID_WSL_OPENSHELL:
        {
            NTSTATUS status;

            if (!node || !node->Distro)
                break;

            if (!NT_SUCCESS(status = WslStartShell(node->Distro->Name)))
                PhShowStatus(WindowHandle, L"Unable to open a shell.", status, 0);
        }
        break;
    case ID_WSL_TERMINATE:
        {
            if (!node || !node->Distro)
                break;

            // Names are validated by WSL; quotes are rejected so the argument cannot be split.
            if (PhFindCharInStringRef(&node->Distro->Name->sr, L'"', FALSE) != SIZE_MAX)
                break;

            if (PhShowConfirmMessage(
                WindowHandle,
                L"terminate",
                node->Distro->Name->Buffer,
                L"All processes in the distribution will be stopped.",
                TRUE
                ))
            {
                WslpStartAction(
                    PhFormatString(L"--terminate \"%s\"", node->Distro->Name->Buffer),
                    L"Unable to terminate the distribution."
                    );
            }
        }
        break;
    case ID_WSL_SHUTDOWN:
        {
            if (PhShowConfirmMessage(
                WindowHandle,
                L"shut down",
                L"WSL",
                L"All running distributions and the WSL 2 virtual machine will be stopped.",
                TRUE
                ))
            {
                WslpStartAction(PhCreateString(L"--shutdown"), L"Unable to shut down WSL.");
            }
        }
        break;
    case ID_WSL_OPENFILELOCATION:
        {
            PPH_STRING fileName;

            if (!node || !node->Distro)
                break;

            // Select the virtual disk when there is one, otherwise open the distribution folder.
            if (fileName = node->Distro->VhdFileName ? node->Distro->VhdFileName : node->Distro->BasePath)
                PhShellExploreFile(WindowHandle, fileName->Buffer);
        }
        break;
    case ID_WSL_GOTOPROCESS:
        {
            PPH_PROCESS_NODE processNode;

            if (!WslVmProcessItem)
                break;

            if (processNode = PhFindProcessNode(WslVmProcessItem->ProcessId))
            {
                SystemInformer_SelectTabPage(0);
                SystemInformer_SelectProcessNode(processNode);
            }
        }
        break;
    case ID_WSL_COPY:
        {
            PPH_STRING text;

            text = PhGetTreeNewText(WindowHandle, 0);
            PhSetClipboardString(WindowHandle, &text->sr);
            PhDereferenceObject(text);
        }
        break;
    }
}

/**
 * Shows the context menu for the selected node.
 */
static VOID WslpShowContextMenu(
    _In_ HWND WindowHandle,
    _In_ PPH_TREENEW_CONTEXT_MENU ContextMenuEvent
    )
{
    PWSL_NODE node = WslpGetSelectedNode();
    PPH_EMENU menu;
    PPH_EMENU_ITEM item;

    if (!node)
        return;

    menu = PhCreateEMenu();

    if (node->Type == WslNodeTypeVm)
    {
        PhInsertEMenuItem(menu, PhCreateEMenuItem(0, ID_WSL_GOTOPROCESS, L"&Go to process", NULL, NULL), ULONG_MAX);
        PhInsertEMenuItem(menu, PhCreateEMenuSeparator(), ULONG_MAX);
        PhInsertEMenuItem(menu, PhCreateEMenuItem(0, ID_WSL_SHUTDOWN, L"&Shut down WSL", NULL, NULL), ULONG_MAX);
        PhSetFlagsEMenuItem(menu, ID_WSL_GOTOPROCESS, PH_EMENU_DEFAULT, PH_EMENU_DEFAULT);

        if (!WslVmProcessItem)
            PhEnableEMenuItem(menu, ID_WSL_GOTOPROCESS, FALSE);
    }
    else
    {
        PhInsertEMenuItem(menu, PhCreateEMenuItem(0, ID_WSL_OPENSHELL, L"Open &shell", NULL, NULL), ULONG_MAX);
        PhInsertEMenuItem(menu, PhCreateEMenuItem(0, ID_WSL_TERMINATE, L"&Terminate", NULL, NULL), ULONG_MAX);
        PhInsertEMenuItem(menu, PhCreateEMenuSeparator(), ULONG_MAX);
        PhInsertEMenuItem(menu, PhCreateEMenuItem(0, ID_WSL_OPENFILELOCATION, L"Open &file location", NULL, NULL), ULONG_MAX);

        if (node->Distro->State != WslDistroStateRunning)
            PhEnableEMenuItem(menu, ID_WSL_TERMINATE, FALSE);
        if (!node->Distro->BasePath)
            PhEnableEMenuItem(menu, ID_WSL_OPENFILELOCATION, FALSE);
    }

    PhInsertEMenuItem(menu, PhCreateEMenuSeparator(), ULONG_MAX);
    PhInsertEMenuItem(menu, PhCreateEMenuItem(0, ID_WSL_COPY, L"&Copy\bCtrl+C", NULL, NULL), ULONG_MAX);
    PhInsertCopyCellEMenuItem(menu, ID_WSL_COPY, WindowHandle, ContextMenuEvent->Column);

    item = PhShowEMenu(
        menu,
        WindowHandle,
        PH_EMENU_SHOW_LEFTRIGHT,
        PH_ALIGN_LEFT | PH_ALIGN_TOP,
        ContextMenuEvent->Location.x,
        ContextMenuEvent->Location.y
        );

    if (item && !PhHandleCopyCellEMenuItem(item))
        WslpHandleCommand(WindowHandle, item->Id);

    PhDestroyEMenu(menu);
}

/**
 * Tree callback.
 */
static BOOLEAN NTAPI WslpTreeNewCallback(
    _In_ HWND WindowHandle,
    _In_ PH_TREENEW_MESSAGE Message,
    _In_ PVOID Parameter1,
    _In_ PVOID Parameter2,
    _In_ PVOID Context
    )
{
    switch (Message)
    {
    case TreeNewGetChildren:
        {
            PPH_TREENEW_GET_CHILDREN getChildren = Parameter1;
            PWSL_NODE node = (PWSL_NODE)getChildren->Node;
            PPH_LIST children;

            if (!node)
                children = WslRootNodes;
            else if (node->Type == WslNodeTypeVm)
                children = node->Children;
            else
                return FALSE;

            WslpSortNodes(children);
            getChildren->Children = (PPH_TREENEW_NODE *)children->Items;
            getChildren->NumberOfChildren = children->Count;
        }
        return TRUE;
    case TreeNewIsLeaf:
        {
            PPH_TREENEW_IS_LEAF isLeaf = Parameter1;
            PWSL_NODE node = (PWSL_NODE)isLeaf->Node;

            isLeaf->IsLeaf = node->Type != WslNodeTypeVm || node->Children->Count == 0;
        }
        return TRUE;
    case TreeNewGetCellText:
        {
            PPH_TREENEW_GET_CELL_TEXT getCellText = Parameter1;
            PWSL_NODE node = (PWSL_NODE)getCellText->Node;

            if (node->Type == WslNodeTypeVm)
                WslpGetVmCellText(node, getCellText);
            else
                WslpGetDistroCellText(node, getCellText);

            getCellText->Flags = TN_CACHE;
        }
        return TRUE;
    case TreeNewGetNodeColor:
        {
            PPH_TREENEW_GET_NODE_COLOR getNodeColor = Parameter1;
            PWSL_NODE node = (PWSL_NODE)getNodeColor->Node;
            BOOLEAN stopped;

            // Grey out what is not running, so the busy parts of WSL stand out.
            if (node->Type == WslNodeTypeVm)
                stopped = !WslVmProcessItem;
            else
                stopped = node->Distro->State != WslDistroStateRunning;

            if (stopped)
                getNodeColor->ForeColor = GetSysColor(COLOR_GRAYTEXT);

            getNodeColor->Flags = TN_CACHE;
        }
        return TRUE;
    case TreeNewGetCellTooltip:
        {
            PPH_TREENEW_GET_CELL_TOOLTIP getCellTooltip = Parameter1;
            PWSL_NODE node = (PWSL_NODE)getCellTooltip->Node;

            if (getCellTooltip->Column->Id != WSLTNC_NAME)
                return FALSE;

            if (node->Type == WslNodeTypeVm && !WslVmProcessItem && WslVmCandidates > 1)
                getCellTooltip->Text = WslAmbiguousVmText;
            else if (node->Type == WslNodeTypeDistro && node->Distro->Default)
                getCellTooltip->Text = WslDefaultTooltipText;
            else
                return FALSE;

            getCellTooltip->Unfolding = FALSE;
            getCellTooltip->MaximumWidth = ULONG_MAX;
        }
        return TRUE;
    case TreeNewSortChanged:
        {
            PPH_TREENEW_SORT_CHANGED_EVENT sorting = Parameter1;

            WslTreeNewSortColumn = sorting->SortColumn;
            WslTreeNewSortOrder = sorting->SortOrder;
            TreeNew_NodesStructured(WindowHandle);
        }
        return TRUE;
    case TreeNewKeyDown:
        {
            PPH_TREENEW_KEY_EVENT keyEvent = Parameter1;

            if (keyEvent && keyEvent->VirtualKey == 'C' && GetKeyState(VK_CONTROL) < 0)
                WslpHandleCommand(WindowHandle, ID_WSL_COPY);
        }
        return TRUE;
    case TreeNewHeaderRightClick:
        {
            PH_TN_COLUMN_MENU_DATA data;

            data.TreeNewHandle = WindowHandle;
            data.MouseEvent = Parameter1;
            data.DefaultSortColumn = WSLTNC_NAME;
            data.DefaultSortOrder = AscendingSortOrder;
            PhInitializeTreeNewColumnMenuEx(&data, PH_TN_COLUMN_MENU_SHOW_RESET_SORT);

            data.Selection = PhShowEMenu(data.Menu, WindowHandle, PH_EMENU_SHOW_LEFTRIGHT,
                PH_ALIGN_LEFT | PH_ALIGN_TOP, data.MouseEvent->ScreenLocation.x, data.MouseEvent->ScreenLocation.y);
            PhHandleTreeNewColumnMenu(&data);
            PhDeleteTreeNewColumnMenu(&data);
        }
        return TRUE;
    case TreeNewLeftDoubleClick:
        {
            PWSL_NODE node = WslpGetSelectedNode();

            // Double-clicking a distribution does nothing: its natural action, a shell,
            // would start it, and that should never happen by accident.
            if (node && node->Type == WslNodeTypeVm)
                WslpHandleCommand(WindowHandle, ID_WSL_GOTOPROCESS);
        }
        return TRUE;
    case TreeNewContextMenu:
        {
            WslpShowContextMenu(WindowHandle, Parameter1);
        }
        return TRUE;
    }

    return FALSE;
}

/**
 * Creates the tree columns and loads the saved column layout and sort.
 */
static VOID WslpInitializeTreeList(
    _In_ HWND WindowHandle
    )
{
    PPH_STRING settings;
    PH_INTEGER_PAIR sortSettings;

    WslTreeNewHandle = WindowHandle;

    PhSetControlTheme(WindowHandle, !PhGetIntegerSetting(SETTING_ENABLE_THEME_SUPPORT) ? L"explorer" : L"DarkMode_Explorer");
    TreeNew_SetRedraw(WindowHandle, FALSE);
    TreeNew_SetCallback(WindowHandle, WslpTreeNewCallback, NULL);
    TreeNew_SetEmptyText(WindowHandle, &WslEmptyText, 0);

    PhAddTreeNewColumn(WindowHandle, WSLTNC_NAME, TRUE, L"Name", 200, PH_ALIGN_LEFT, 0, 0);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_STATE, TRUE, L"State", 70, PH_ALIGN_LEFT, 1, 0);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_VERSION, TRUE, L"Version", 50, PH_ALIGN_RIGHT, 2, DT_RIGHT);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_PID, TRUE, L"PID", 50, PH_ALIGN_RIGHT, 3, DT_RIGHT);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_CPU, TRUE, L"CPU", 45, PH_ALIGN_RIGHT, 4, DT_RIGHT);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_PRIVATEBYTES, TRUE, L"Private bytes", 80, PH_ALIGN_RIGHT, 5, DT_RIGHT);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_VHDSIZE, TRUE, L"Disk file size", 80, PH_ALIGN_RIGHT, 6, DT_RIGHT);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_LOCATION, TRUE, L"Location", 300, PH_ALIGN_LEFT, 7, DT_PATH_ELLIPSIS);

    TreeNew_SetTriState(WindowHandle, TRUE);
    TreeNew_SetSort(WindowHandle, WSLTNC_NAME, AscendingSortOrder);

    settings = PhGetStringSetting(SETTING_NAME_TREE_LIST_COLUMNS);
    PhCmLoadSettings(WindowHandle, &settings->sr);
    PhDereferenceObject(settings);

    sortSettings = PhGetIntegerPairSetting(SETTING_NAME_TREE_LIST_SORT);
    TreeNew_SetSort(WindowHandle, (ULONG)sortSettings.X, (PH_SORT_ORDER)sortSettings.Y);

    TreeNew_SetRedraw(WindowHandle, TRUE);
}

/**
 * Saves the column layout and sort.
 */
static VOID WslpSaveTreeListSettings(
    VOID
    )
{
    PPH_STRING settings;
    PH_INTEGER_PAIR sortSettings;
    ULONG sortColumn;
    PH_SORT_ORDER sortOrder;

    if (!WslTreeNewHandle)
        return;

    settings = PhCmSaveSettings(WslTreeNewHandle);
    PhSetStringSetting2(SETTING_NAME_TREE_LIST_COLUMNS, &settings->sr);
    PhDereferenceObject(settings);

    TreeNew_GetSort(WslTreeNewHandle, &sortColumn, &sortOrder);
    sortSettings.X = sortColumn;
    sortSettings.Y = sortOrder;
    PhSetIntegerPairSetting(SETTING_NAME_TREE_LIST_SORT, sortSettings);
}

/**
 * Main window tab page callback.
 */
static BOOLEAN WslpPageCallback(
    _In_ PPH_MAIN_TAB_PAGE Page,
    _In_ PH_MAIN_TAB_PAGE_MESSAGE Message,
    _In_opt_ PVOID Parameter1,
    _In_opt_ PVOID Parameter2
    )
{
    switch (Message)
    {
    case MainTabPageCreateWindow:
        {
            HWND windowHandle;
            ULONG thinRows;
            ULONG treelistBorder;
            ULONG treelistCustomColors;
            PH_TREENEW_CREATEPARAMS treelistCreateParams = { 0 };

            thinRows = PhGetIntegerSetting(SETTING_THIN_ROWS) ? TN_STYLE_THIN_ROWS : 0;
            treelistBorder = PhGetIntegerSetting(SETTING_TREE_LIST_BORDER_ENABLE) ? WS_BORDER : 0;
            treelistCustomColors = PhGetIntegerSetting(SETTING_TREE_LIST_CUSTOM_COLORS_ENABLE) ? TN_STYLE_CUSTOM_COLORS : 0;

            if (treelistCustomColors)
            {
                treelistCreateParams.TextColor = PhGetIntegerSetting(SETTING_TREE_LIST_CUSTOM_COLOR_TEXT);
                treelistCreateParams.FocusColor = PhGetIntegerSetting(SETTING_TREE_LIST_CUSTOM_COLOR_FOCUS);
                treelistCreateParams.SelectionColor = PhGetIntegerSetting(SETTING_TREE_LIST_CUSTOM_COLOR_SELECTION);
            }

            windowHandle = PhCreateWindow(
                PH_TREENEW_CLASSNAME,
                NULL,
                WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | TN_STYLE_DOUBLE_BUFFERED | thinRows | treelistBorder | treelistCustomColors,
                0,
                0,
                0,
                0,
                Parameter2,
                NULL,
                NULL,
                &treelistCreateParams
                );

            if (!windowHandle)
                return FALSE;

            if (PhGetIntegerSetting(SETTING_ENABLE_THEME_SUPPORT))
            {
                PhInitializeWindowTheme(windowHandle, TRUE);
                TreeNew_ThemeSupport(windowHandle, TRUE);
            }

            WslDistroNodes = PhCreateList(4);
            WslRootNodes = PhCreateList(4);

            WslpInitializeTreeList(windowHandle);

            if (Parameter1)
                *(HWND *)Parameter1 = windowHandle;
        }
        return TRUE;
    case MainTabPageSaveSettings:
        {
            WslpSaveTreeListSettings();
        }
        return TRUE;
    case MainTabPageSelected:
        {
            WslTabSelected = !!PtrToUlong(Parameter1);

            if (WslTabSelected)
                WslOnProcessesUpdated();

            WslSetProviderEnabled(WslTabSelected);
        }
        break;
    case MainTabPageExportContent:
        {
            PPH_MAIN_TAB_PAGE_EXPORT_CONTENT exportContent = Parameter1;
            PPH_LIST lines;

            if (!exportContent || !WslTreeNewHandle)
                return FALSE;

            lines = PhGetGenericTreeNewLines(WslTreeNewHandle, exportContent->Mode);

            for (ULONG i = 0; i < lines->Count; i++)
            {
                PPH_STRING line = lines->Items[i];

                PhWriteStringAsUtf8FileStream(exportContent->FileStream, &line->sr);
                PhDereferenceObject(line);
                PhWriteStringAsUtf8FileStream2(exportContent->FileStream, L"\r\n");
            }

            PhDereferenceObject(lines);
        }
        return TRUE;
    case MainTabPageFontChanged:
        {
            HFONT font = (HFONT)Parameter1;

            if (WslTreeNewHandle)
                SetWindowFont(WslTreeNewHandle, font, TRUE);
        }
        break;
    }

    return FALSE;
}

/**
 * Registers the WSL tab with the main window.
 */
VOID WslInitializeTab(
    VOID
    )
{
    PH_MAIN_TAB_PAGE page;

    memset(&page, 0, sizeof(PH_MAIN_TAB_PAGE));
    page.Name = WslPageText;
    page.Callback = WslpPageCallback;
    WslPage = PhPluginCreateTabPage(&page);
}
