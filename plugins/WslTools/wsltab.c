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

// The tab is a tree: the WSL 2 virtual machine with its distributions below it, and WSL 1
// distributions at the root because they do not run in the VM. Each running distribution
// lists its Linux processes below it. Each running WSLC session is a root node with its
// containers below it.

typedef enum _WSL_TREE_COLUMN
{
    WSLTNC_NAME,
    WSLTNC_PID, // Windows PID of a VM process
    WSLTNC_LINUXPID,
    WSLTNC_TYPE,
    WSLTNC_STATE,
    WSLTNC_CPU,
    WSLTNC_MEMORY, // What it measures depends on the row; see WslpGetMemoryTooltip
    WSLTNC_IMAGE, // Container image, distribution OS or VM kernel
    WSLTNC_PORTS,
    WSLTNC_DISK,
    WSLTNC_STATUS, // Uptime, or a container's status
    WSLTNC_VERSION,
    WSLTNC_LOCATION,
    // New columns go last: the IDs are stored in the saved column layout and sort.
    WSLTNC_MAXIMUM
} WSL_TREE_COLUMN;

typedef enum _WSL_NODE_TYPE
{
    WslNodeTypeVm,
    WslNodeTypeDistro,
    WslNodeTypeLinuxProcess,
    WslNodeTypeSession,
    WslNodeTypeContainer
} WSL_NODE_TYPE;

typedef struct _WSL_NODE
{
    PH_TREENEW_NODE Node;
    WSL_NODE_TYPE Type;
    PPH_STRING Id; // Distribution id; NULL for the VM node
    PWSL_DISTRO_ITEM Distro; // Owned by WslCurrentSnapshot; distribution nodes only
    PWSL_LINUX_PROCESS LinuxProcess; // Owned by the distribution's frame; process nodes only
    PWSL_PROCESS_FRAME Frame; // The frame LinuxProcess belongs to; process nodes only
    PWSL_SESSION Session; // Owned by WslCurrentSnapshot; session and container nodes
    PWSL_CONTAINER Container; // Owned by WslCurrentSnapshot; container nodes only
    PPH_STRING NameText; // Distribution name, followed by " *" for the default one
    PPH_STRING TooltipText; // Cached name column tooltip
    PPH_STRING StateText; // Cached state of a container that exited, e.g. "Exited (137)"
    PPH_STRING ImageText; // Cached "kernel <version>" of the VM
    PPH_STRING StatusText; // Cached uptime or container status
    // VM node: its distribution nodes, which WslDistroNodes owns.
    // Distribution, session and container nodes: their process and container nodes, which they own.
    PPH_LIST Children;
    BOOLEAN Seen; // Scratch flag while a snapshot is applied

    PH_STRINGREF TextCache[WSLTNC_MAXIMUM];
    WCHAR PidText[PH_INT32_STR_LEN_1];
    WCHAR CpuText[PH_INT32_STR_LEN_1];
    WCHAR MemoryText[PH_INT64_STR_LEN_1];
    WCHAR VhdSizeText[PH_INT64_STR_LEN_1];
    WCHAR VersionText[PH_INT32_STR_LEN_1];
} WSL_NODE, *PWSL_NODE;

typedef struct _WSL_ACTION_CONTEXT
{
    PPH_STRING FileName; // wsl.exe or wslc.exe; a cached string the context does not own
    PPH_STRING Arguments;
    PPH_STRING Description;
    NTSTATUS Status;
    PPH_STRING Message; // the tool's own error text, if it printed one
} WSL_ACTION_CONTEXT, *PWSL_ACTION_CONTEXT;

static PPH_MAIN_TAB_PAGE WslPage = NULL;
static HWND WslTreeNewHandle = NULL;
static ULONG WslTreeNewSortColumn = WSLTNC_NAME;
static PH_SORT_ORDER WslTreeNewSortOrder = AscendingSortOrder;
static BOOLEAN WslTabSelected = FALSE;

static PWSL_SNAPSHOT WslCurrentSnapshot = NULL;
static PWSL_NODE WslVmNode = NULL;
static PPH_LIST WslDistroNodes = NULL; // PWSL_NODE, all distributions
static PPH_LIST WslRootNodes = NULL; // PWSL_NODE, the VM node, WSL 1 distributions and sessions
static PPH_LIST WslSessionNodes = NULL; // PWSL_NODE, all WSLC sessions
static PPH_PROCESS_ITEM WslVmProcessItem = NULL;
static ULONG WslVmCandidates = 0;
// The session VM process, only while exactly one session runs, because nothing links a
// session VM to a session name.
static PPH_PROCESS_ITEM WslSessionVmProcessItem = NULL;
// A row asked for by "Go to WSL" before the tab had its rows; tried once on the next snapshot.
static WSL_VM_SELECTION WslPendingVmSelection = WslVmSelectionNone;

static CONST PH_STRINGREF WslPageText = PH_STRINGREF_INIT(L"WSL");
static CONST PH_STRINGREF WslVmNodeText = PH_STRINGREF_INIT(L"WSL");
static CONST PH_STRINGREF WslEmptyText = PH_STRINGREF_INIT(L"No WSL distributions are registered for this user.");
static CONST PH_STRINGREF WslAmbiguousVmText = PH_STRINGREF_INIT(L"Several virtual machine processes exist; the WSL one cannot be identified.");
static CONST PH_STRINGREF WslDefaultMarker = PH_STRINGREF_INIT(L" *");
static CONST PH_STRINGREF WslDefaultTooltipText = PH_STRINGREF_INIT(L"Default distribution");
static CONST PH_STRINGREF WslGuestUptimeTooltipText = PH_STRINGREF_INIT(L"Time the VM has been running since the start, from the VM's own clock. It leaves out time the VM was paused, e.g. while the host was asleep.");

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
    if (Type != WslNodeTypeLinuxProcess)
        node->Children = PhCreateList(4);

    // A distribution or container can have hundreds of processes; start collapsed to keep the overview.
    if (Type == WslNodeTypeDistro || Type == WslNodeTypeContainer)
        node->Node.Expanded = FALSE;

    return node;
}

/**
 * Frees a tree node, and the process nodes of a distribution node.
 *
 * \param Node The node.
 */
static VOID WslpDestroyNode(
    _In_ PWSL_NODE Node
    )
{
    if (Node->Type == WslNodeTypeDistro || Node->Type == WslNodeTypeSession || Node->Type == WslNodeTypeContainer)
    {
        for (ULONG i = 0; i < Node->Children->Count; i++)
            WslpDestroyNode(Node->Children->Items[i]);
    }

    PhClearReference(&Node->Id);
    PhClearReference(&Node->NameText);
    PhClearReference(&Node->TooltipText);
    PhClearReference(&Node->StateText);
    PhClearReference(&Node->ImageText);
    PhClearReference(&Node->StatusText);
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
    PhClearReference(&Node->TooltipText);
    PhClearReference(&Node->StateText);
    PhClearReference(&Node->ImageText);
    PhClearReference(&Node->StatusText);
    PhInvalidateTreeNewNode(&Node->Node, TN_CACHE_COLOR);
}

_Function_class_(PH_HASHTABLE_EQUAL_FUNCTION)
static BOOLEAN NTAPI WslpProcessNodeEqualFunction(
    _In_ PVOID Entry1,
    _In_ PVOID Entry2
    )
{
    PWSL_LINUX_PROCESS process1 = (*(PWSL_NODE *)Entry1)->LinuxProcess;
    PWSL_LINUX_PROCESS process2 = (*(PWSL_NODE *)Entry2)->LinuxProcess;

    return process1->ProcessId == process2->ProcessId && process1->StartTime == process2->StartTime;
}

_Function_class_(PH_HASHTABLE_HASH_FUNCTION)
static ULONG NTAPI WslpProcessNodeHashFunction(
    _In_ PVOID Entry
    )
{
    PWSL_LINUX_PROCESS process = (*(PWSL_NODE *)Entry)->LinuxProcess;

    return PhHashInt32(process->ProcessId) ^ PhHashInt64(process->StartTime);
}

/**
 * Matches the process nodes of a distribution or container node to the processes of a frame.
 *
 * \param ParentNode The distribution or container node.
 * \param Frame The distribution's or session's latest frame, or NULL.
 * \param ContainerId For a container, its short ID: only processes in that container are
 * shown. NULL for a distribution.
 * \remarks Processes are matched by PID and start time, so a reused PID gets a new node and
 * selection stays on the process it was on. The nodes still point at the previous frame,
 * which the previous snapshot keeps alive until the update is done.
 */
static VOID WslpUpdateProcessNodes(
    _In_ PWSL_NODE ParentNode,
    _In_opt_ PWSL_PROCESS_FRAME Frame,
    _In_opt_ PPH_STRING ContainerId
    )
{
    PPH_LIST processes = Frame ? Frame->Processes : NULL;
    PPH_LIST children = ParentNode->Children;
    PPH_HASHTABLE nodeTable;

    nodeTable = PhCreateHashtable(sizeof(PWSL_NODE), WslpProcessNodeEqualFunction, WslpProcessNodeHashFunction, children->Count + 1);

    for (ULONG i = 0; i < children->Count; i++)
    {
        PWSL_NODE child = children->Items[i];

        child->Seen = FALSE;
        PhAddEntryHashtable(nodeTable, &child);
    }

    for (ULONG i = 0; processes && i < processes->Count; i++)
    {
        PWSL_LINUX_PROCESS process = processes->Items[i];
        WSL_NODE lookupNode;
        PWSL_NODE lookupNodePtr = &lookupNode;
        PWSL_NODE *entry;
        PWSL_NODE node = NULL;

        // "wslc list" prints the short container ID; the cgroup has the full one.
        if (ContainerId && (!process->ContainerId || !PhStartsWithString(process->ContainerId, ContainerId, TRUE)))
            continue;

        lookupNode.LinuxProcess = process;

        if (entry = PhFindEntryHashtable(nodeTable, &lookupNodePtr))
            node = *entry;

        if (!node)
        {
            node = WslpCreateNode(WslNodeTypeLinuxProcess, NULL);
            PhAddItemList(children, node);
        }

        node->LinuxProcess = process;
        node->Frame = Frame;
        node->Seen = TRUE;
        WslpInvalidateNode(node);
    }

    PhDereferenceObject(nodeTable);

    for (ULONG i = children->Count; i != 0; i--)
    {
        PWSL_NODE node = children->Items[i - 1];

        if (!node->Seen)
        {
            PhRemoveItemList(children, i - 1);
            WslpDestroyNode(node);
        }
    }
}

/**
 * Matches the session nodes, and their container nodes, to the sessions of a snapshot.
 *
 * \param Sessions The running sessions, or NULL.
 * \remarks Sessions are matched by name and containers by ID, so selection and expansion
 * survive refreshes. There are few of either, so the matching is linear.
 */
static VOID WslpUpdateSessionNodes(
    _In_opt_ PPH_LIST Sessions
    )
{
    for (ULONG i = 0; i < WslSessionNodes->Count; i++)
        ((PWSL_NODE)WslSessionNodes->Items[i])->Seen = FALSE;

    for (ULONG i = 0; Sessions && i < Sessions->Count; i++)
    {
        PWSL_SESSION session = Sessions->Items[i];
        PWSL_NODE sessionNode = NULL;

        for (ULONG j = 0; j < WslSessionNodes->Count; j++)
        {
            PWSL_NODE node = WslSessionNodes->Items[j];

            if (PhEqualString(node->Id, session->Name, FALSE))
            {
                sessionNode = node;
                break;
            }
        }

        if (!sessionNode)
        {
            sessionNode = WslpCreateNode(WslNodeTypeSession, session->Name);
            PhAddItemList(WslSessionNodes, sessionNode);
        }

        sessionNode->Session = session;
        sessionNode->Seen = TRUE;

        if (!sessionNode->NameText)
        {
            static CONST PH_STRINGREF prefix = PH_STRINGREF_INIT(L"WSLC: ");

            sessionNode->NameText = PhConcatStringRef2(&prefix, &session->Name->sr);
        }
        WslpInvalidateNode(sessionNode);

        for (ULONG j = 0; j < sessionNode->Children->Count; j++)
            ((PWSL_NODE)sessionNode->Children->Items[j])->Seen = FALSE;

        for (ULONG j = 0; j < session->Containers->Count; j++)
        {
            PWSL_CONTAINER container = session->Containers->Items[j];
            PWSL_NODE containerNode = NULL;

            for (ULONG k = 0; k < sessionNode->Children->Count; k++)
            {
                PWSL_NODE node = sessionNode->Children->Items[k];

                if (PhEqualString(node->Id, container->Id, TRUE))
                {
                    containerNode = node;
                    break;
                }
            }

            if (!containerNode)
            {
                containerNode = WslpCreateNode(WslNodeTypeContainer, container->Id);
                PhAddItemList(sessionNode->Children, containerNode);
            }

            containerNode->Session = session;
            containerNode->Container = container;
            containerNode->Seen = TRUE;
            WslpInvalidateNode(containerNode);
            WslpUpdateProcessNodes(containerNode, session->Processes, container->Id);
        }

        for (ULONG j = sessionNode->Children->Count; j != 0; j--)
        {
            PWSL_NODE node = sessionNode->Children->Items[j - 1];

            if (!node->Seen)
            {
                PhRemoveItemList(sessionNode->Children, j - 1);
                WslpDestroyNode(node);
            }
        }
    }

    // Remove the nodes of sessions that ended.
    for (ULONG i = WslSessionNodes->Count; i != 0; i--)
    {
        PWSL_NODE node = WslSessionNodes->Items[i - 1];

        if (!node->Seen)
        {
            PhRemoveItemList(WslSessionNodes, i - 1);
            WslpDestroyNode(node);
        }
    }
}

/**
 * Selects the row of a VM and scrolls it into view.
 *
 * \return FALSE if the row does not exist (yet).
 */
static BOOLEAN WslpSelectVmNode(
    _In_ WSL_VM_SELECTION Selection
    )
{
    PWSL_NODE node = NULL;

    if (!WslTreeNewHandle)
        return FALSE;

    if (Selection == WslVmSelectionWsl)
        node = WslVmNode;
    else if (Selection == WslVmSelectionSession && WslSessionNodes->Count == 1)
        node = WslSessionNodes->Items[0];

    if (!node)
        return FALSE;

    TreeNew_DeselectRange(WslTreeNewHandle, 0, -1);
    TreeNew_FocusMarkSelectNode(WslTreeNewHandle, &node->Node);
    TreeNew_EnsureVisible(WslTreeNewHandle, &node->Node);
    SetFocus(WslTreeNewHandle);

    return TRUE;
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

        WslpUpdateProcessNodes(node, distro->Processes, NULL);

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

    WslpUpdateSessionNodes(snapshot->Sessions);

    // The previous snapshot owned the distribution items, process frames and sessions the nodes pointed to.
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

    for (ULONG i = 0; i < WslSessionNodes->Count; i++)
        PhAddItemList(WslRootNodes, WslSessionNodes->Items[i]);

    if (WslVmNode)
        WslpInvalidateNode(WslVmNode);

    for (ULONG i = 0; i < WslDistroNodes->Count; i++)
        WslpInvalidateNode(WslDistroNodes->Items[i]);

    TreeNew_NodesStructured(WslTreeNewHandle);

    // One attempt: if the VM's row is still missing, it has stopped since the request.
    if (WslPendingVmSelection != WslVmSelectionNone)
    {
        WslpSelectVmNode(WslPendingVmSelection);
        WslPendingVmSelection = WslVmSelectionNone;
    }
}

/**
 * Switches to the WSL tab and selects the row of a VM, for "Go to WSL" on a vmmem process.
 *
 * \param Selection Which row to select.
 * \remarks When the tab has never been shown, its rows only exist after the first snapshot,
 * so the selection is kept and tried when that snapshot arrives.
 */
VOID WslSelectVmNode(
    _In_ WSL_VM_SELECTION Selection
    )
{
    if (!WslPage)
        return;

    SystemInformer_SelectTabPage(WslPage->Index);

    WslPendingVmSelection = WslpSelectVmNode(Selection) ? WslVmSelectionNone : Selection;
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
    PhMoveReference(&WslSessionVmProcessItem, WslSessionNodes->Count == 1 ? WslReferenceSessionVmProcessItem() : NULL);

    if (WslVmNode)
        WslpInvalidateNode(WslVmNode);

    for (ULONG i = 0; i < WslSessionNodes->Count; i++)
        WslpInvalidateNode(WslSessionNodes->Items[i]);

    InvalidateRect(WslTreeNewHandle, NULL, FALSE);
}

/**
 * Gets the memory value of a node for the Memory column.
 */
static ULONG64 WslpGetNodeMemory(
    _In_ PWSL_NODE Node
    )
{
    switch (Node->Type)
    {
    case WslNodeTypeVm:
        return WslVmProcessItem ? WslVmProcessItem->VmCounters.PagefileUsage : 0;
    case WslNodeTypeDistro:
        return Node->Distro->Processes ? Node->Distro->Processes->ResidentBytes : 0;
    case WslNodeTypeLinuxProcess:
        return Node->LinuxProcess->ResidentBytes;
    case WslNodeTypeSession:
        if (WslSessionVmProcessItem)
            return WslSessionVmProcessItem->VmCounters.PagefileUsage;
        return Node->Session->HaveStats ? Node->Session->MemoryBytes : 0;
    case WslNodeTypeContainer:
        return Node->Container->HaveStats ? Node->Container->MemoryBytes : 0;
    }

    return 0;
}

/**
 * Gets what the Memory column measures for a node, for its tooltip.
 */
static PCPH_STRINGREF WslpGetMemoryTooltip(
    _In_ PWSL_NODE Node
    )
{
    static CONST PH_STRINGREF vmText = PH_STRINGREF_INIT(L"Private bytes of the VM process: the memory the VM holds on the host.");
    static CONST PH_STRINGREF distroText = PH_STRINGREF_INIT(L"Sum of the resident sets of the distribution's processes. Shared pages are counted once per process.");
    static CONST PH_STRINGREF processText = PH_STRINGREF_INIT(L"Resident set of the Linux process.");
    static CONST PH_STRINGREF containerText = PH_STRINGREF_INIT(L"Memory usage as reported by wslc stats.");
    static CONST PH_STRINGREF sessionText = PH_STRINGREF_INIT(L"Sum of the memory usage of the session's running containers.");
    static CONST PH_STRINGREF sessionVmText = PH_STRINGREF_INIT(L"Private bytes of the session VM process: the memory the VM holds on the host.");

    switch (Node->Type)
    {
    case WslNodeTypeVm:
        return &vmText;
    case WslNodeTypeDistro:
        return &distroText;
    case WslNodeTypeLinuxProcess:
        return &processText;
    case WslNodeTypeSession:
        return WslSessionVmProcessItem ? &sessionVmText : &sessionText;
    default:
        return &containerText;
    }
}

/**
 * Gets how long a Linux process has been running.
 *
 * \param Frame The frame the process is from.
 * \param StartTime The process start time, in clock ticks after boot.
 * \return The run time in PH ticks (100 ns), or 0 if unknown.
 */
static ULONG64 WslpGetLinuxRunTime(
    _In_ PWSL_PROCESS_FRAME Frame,
    _In_ ULONG64 StartTime
    )
{
    DOUBLE seconds;

    if (Frame->TicksPerSecond == 0)
        return 0;

    seconds = Frame->Uptime - (DOUBLE)StartTime / Frame->TicksPerSecond;

    return seconds > 0 ? (ULONG64)(seconds * PH_TICKS_PER_SEC) : 0;
}

/**
 * Gets how long a distribution has been running, from the start time of its init (PID 1).
 */
static ULONG64 WslpGetDistroRunTime(
    _In_ PWSL_DISTRO_ITEM Distro
    )
{
    if (!Distro->Processes || Distro->State != WslDistroStateRunning)
        return 0;

    for (ULONG i = 0; i < Distro->Processes->Processes->Count; i++)
    {
        PWSL_LINUX_PROCESS process = Distro->Processes->Processes->Items[i];

        if (process->ProcessId == 1)
            return WslpGetLinuxRunTime(Distro->Processes, process->StartTime);
    }

    return 0;
}

/**
 * Gets the display state of a container. An exited container shows its exit code, which
 * wslc only puts in the status text, e.g. "Exited (137) 2 days ago".
 */
static PPH_STRING WslpGetContainerStateText(
    _In_ PWSL_CONTAINER Container
    )
{
    static CONST PH_STRINGREF exitedPrefix = PH_STRINGREF_INIT(L"Exited (");
    ULONG_PTR close;

    if (!Container->Running && Container->Status &&
        PhStartsWithStringRef(&Container->Status->sr, &exitedPrefix, TRUE) &&
        (close = PhFindCharInStringRef(&Container->Status->sr, L')', FALSE)) != SIZE_MAX)
    {
        PH_STRINGREF state;

        state.Buffer = Container->Status->Buffer;
        state.Length = (close + 1) * sizeof(WCHAR);

        return PhCreateString2(&state);
    }

    return Container->State ? PhReferenceObject(Container->State) : PhReferenceEmptyString();
}

/**
 * Gets the kernel version shown for the VM, e.g. "kernel 6.18.40.1", from the kernel release
 * a process collector reported. The suffix after the version ("-microsoft-standard-WSL2")
 * is left out.
 */
static PPH_STRING WslpGetKernelText(
    VOID
    )
{
    for (ULONG i = 0; i < WslDistroNodes->Count; i++)
    {
        PWSL_DISTRO_ITEM distro = ((PWSL_NODE)WslDistroNodes->Items[i])->Distro;
        PH_STRINGREF version;
        PH_STRINGREF rest;

        if (distro->Version != 2 || !distro->Processes || !distro->Processes->KernelRelease)
            continue;

        PhSplitStringRefAtChar(&distro->Processes->KernelRelease->sr, L'-', &version, &rest);

        return PhCreateString2(&version);
    }

    return NULL;
}

/**
 * Compares two nodes for the current sort column.
 *
 * \remarks Siblings of different types, e.g. the VM, WSL 1 distributions and sessions at the
 * root, are grouped by type in WSL_NODE_TYPE order whatever the sort order.
 */
static int __cdecl WslpCompareNodes(
    _In_ void *Context,
    _In_ const void *Elem1,
    _In_ const void *Elem2
    )
{
    PWSL_NODE node1 = *(PWSL_NODE *)Elem1;
    PWSL_NODE node2 = *(PWSL_NODE *)Elem2;
    int sortResult = 0;

    if (node1->Type != node2->Type)
        return intcmp(node1->Type, node2->Type);

    if (WslTreeNewSortColumn == WSLTNC_MEMORY)
    {
        sortResult = uint64cmp(WslpGetNodeMemory(node1), WslpGetNodeMemory(node2));
    }
    else if (node1->Type == WslNodeTypeLinuxProcess)
    {
        PWSL_LINUX_PROCESS process1 = node1->LinuxProcess;
        PWSL_LINUX_PROCESS process2 = node2->LinuxProcess;

        switch (WslTreeNewSortColumn)
        {
        case WSLTNC_STATE:
            sortResult = uintcmp(process1->State, process2->State);
            break;
        case WSLTNC_LINUXPID:
            sortResult = uintcmp(process1->ProcessId, process2->ProcessId);
            break;
        case WSLTNC_CPU:
            sortResult = singlecmp(process1->CpuUsage, process2->CpuUsage);
            break;
        case WSLTNC_STATUS:
            // Longest running first in ascending order, like the other uptime columns.
            sortResult = uint64cmp(process2->StartTime, process1->StartTime);
            break;
        }

        if (sortResult == 0)
            sortResult = PhCompareString(process1->Name, process2->Name, TRUE);
        if (sortResult == 0)
            sortResult = uintcmp(process1->ProcessId, process2->ProcessId);
    }
    else if (node1->Type == WslNodeTypeDistro)
    {
        PWSL_DISTRO_ITEM distro1 = node1->Distro;
        PWSL_DISTRO_ITEM distro2 = node2->Distro;

        switch (WslTreeNewSortColumn)
        {
        case WSLTNC_STATE:
            sortResult = uintcmp(distro1->State, distro2->State);
            break;
        case WSLTNC_TYPE:
        case WSLTNC_VERSION:
            sortResult = uintcmp(distro1->Version, distro2->Version);
            break;
        case WSLTNC_CPU:
            sortResult = singlecmp(distro1->Processes ? distro1->Processes->CpuUsage : 0, distro2->Processes ? distro2->Processes->CpuUsage : 0);
            break;
        case WSLTNC_IMAGE:
            sortResult = PhCompareStringWithNull(distro1->OsName, distro2->OsName, TRUE);
            break;
        case WSLTNC_DISK:
            sortResult = uint64cmp(distro1->VhdSize, distro2->VhdSize);
            break;
        case WSLTNC_STATUS:
            sortResult = uint64cmp(WslpGetDistroRunTime(distro1), WslpGetDistroRunTime(distro2));
            break;
        case WSLTNC_LOCATION:
            sortResult = PhCompareStringWithNull(distro1->BasePath, distro2->BasePath, TRUE);
            break;
        }

        if (sortResult == 0)
            sortResult = PhCompareString(distro1->Name, distro2->Name, TRUE);
    }
    else if (node1->Type == WslNodeTypeContainer)
    {
        PWSL_CONTAINER container1 = node1->Container;
        PWSL_CONTAINER container2 = node2->Container;

        switch (WslTreeNewSortColumn)
        {
        case WSLTNC_STATE:
            sortResult = PhCompareStringWithNull(container1->State, container2->State, TRUE);
            break;
        case WSLTNC_CPU:
            sortResult = singlecmp(container1->CpuUsage, container2->CpuUsage);
            break;
        case WSLTNC_IMAGE:
            sortResult = PhCompareStringWithNull(container1->Image, container2->Image, TRUE);
            break;
        case WSLTNC_PORTS:
            sortResult = PhCompareStringWithNull(container1->Ports, container2->Ports, TRUE);
            break;
        case WSLTNC_STATUS:
            sortResult = PhCompareStringWithNull(container1->Status, container2->Status, TRUE);
            break;
        }

        if (sortResult == 0)
            sortResult = PhCompareString(container1->Name, container2->Name, TRUE);
    }
    else if (node1->Type == WslNodeTypeSession)
    {
        sortResult = PhCompareString(node1->Session->Name, node2->Session->Name, TRUE);
    }

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
 * Formats a cell into a node buffer.
 *
 * \param GetCellText The cell request.
 * \param Format The format items.
 * \param Count The number of format items.
 * \param Buffer The node buffer that keeps the text alive while the cell is cached.
 * \param BufferLength The size of the buffer, in bytes.
 */
static VOID WslpSetCellText(
    _Inout_ PPH_TREENEW_GET_CELL_TEXT GetCellText,
    _In_reads_(Count) PPH_FORMAT Format,
    _In_ ULONG Count,
    _Out_writes_bytes_(BufferLength) PWSTR Buffer,
    _In_ SIZE_T BufferLength
    )
{
    SIZE_T returnLength;

    if (PhFormatToBuffer(Format, Count, Buffer, BufferLength, &returnLength))
    {
        GetCellText->Text.Buffer = Buffer;
        GetCellText->Text.Length = returnLength - sizeof(UNICODE_NULL);
    }
}

/**
 * Formats a number cell, e.g. a PID.
 */
static VOID WslpSetNumberCellText(
    _Inout_ PPH_TREENEW_GET_CELL_TEXT GetCellText,
    _In_ PWSL_NODE Node,
    _In_ ULONG Number
    )
{
    PH_FORMAT format;

    PhInitFormatU(&format, Number);
    WslpSetCellText(GetCellText, &format, 1, Node->PidText, sizeof(Node->PidText));
}

/**
 * Formats a CPU usage cell, leaving it empty for idle and unknown values.
 */
static VOID WslpSetCpuCellText(
    _Inout_ PPH_TREENEW_GET_CELL_TEXT GetCellText,
    _In_ PWSL_NODE Node,
    _In_ FLOAT CpuUsage
    )
{
    PH_FORMAT format;

    if (CpuUsage >= 0.0001f)
    {
        PhInitFormatF(&format, CpuUsage * 100, 2);
        WslpSetCellText(GetCellText, &format, 1, Node->CpuText, sizeof(Node->CpuText));
    }
}

/**
 * Formats a size cell, leaving it empty for zero.
 */
static VOID WslpSetSizeCellText(
    _Inout_ PPH_TREENEW_GET_CELL_TEXT GetCellText,
    _In_ ULONG64 Size,
    _Out_writes_bytes_(BufferLength) PWSTR Buffer,
    _In_ SIZE_T BufferLength
    )
{
    PH_FORMAT format;

    if (Size != 0)
    {
        PhInitFormatSize(&format, Size);
        WslpSetCellText(GetCellText, &format, 1, Buffer, BufferLength);
    }
}

/**
 * Formats an uptime cell in one unit, e.g. "15 minutes", leaving it empty when unknown.
 *
 * \remarks One unit, as in the container status that wslc prints ("Up 3 minutes"), so the
 * column reads the same for every row.
 */
static VOID WslpSetUptimeCellText(
    _Inout_ PPH_TREENEW_GET_CELL_TEXT GetCellText,
    _In_ PWSL_NODE Node,
    _In_ ULONG64 RunTime
    )
{
    ULONG64 seconds = RunTime / PH_TICKS_PER_SEC;
    ULONG64 value;
    PCWSTR unit;

    if (RunTime == 0)
        return;

    if (seconds < 60)
    {
        value = seconds;
        unit = value == 1 ? L"second" : L"seconds";
    }
    else if (seconds < 60 * 60)
    {
        value = seconds / 60;
        unit = value == 1 ? L"minute" : L"minutes";
    }
    else if (seconds < 48 * 60 * 60)
    {
        value = seconds / (60 * 60);
        unit = value == 1 ? L"hour" : L"hours";
    }
    else
    {
        value = seconds / (24 * 60 * 60);
        unit = L"days";
    }

    PhMoveReference(&Node->StatusText, PhFormatString(L"%I64u %s", value, unit));
    GetCellText->Text = Node->StatusText->sr;
}

/**
 * Gets the state of the WSL 2 VM.
 *
 * \remarks With the VM process identified, that process decides. Otherwise the VM is running
 * exactly when a WSL 2 distribution is, and its state is unknown when several VM processes
 * exist that could not be told apart.
 */
static WSL_DISTRO_STATE WslpGetVmState(
    VOID
    )
{
    if (WslVmProcessItem)
        return WslDistroStateRunning;

    for (ULONG i = 0; i < WslDistroNodes->Count; i++)
    {
        PWSL_DISTRO_ITEM distro = ((PWSL_NODE)WslDistroNodes->Items[i])->Distro;

        if (distro->Version == 2 && distro->State == WslDistroStateRunning)
            return WslDistroStateRunning;
    }

    return WslVmCandidates > 1 ? WslDistroStateUnknown : WslDistroStateStopped;
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

    switch (GetCellText->Id)
    {
    case WSLTNC_NAME:
        GetCellText->Text = WslVmNodeText;
        break;
    case WSLTNC_PID:
        if (processItem)
            WslpSetNumberCellText(GetCellText, Node, HandleToUlong(processItem->ProcessId));
        break;
    case WSLTNC_TYPE:
        PhInitializeStringRef(&GetCellText->Text, L"Utility VM");
        break;
    case WSLTNC_STATE:
        GetCellText->Text = *WslGetDistroStateText(WslpGetVmState());
        break;
    case WSLTNC_CPU:
        if (processItem)
            WslpSetCpuCellText(GetCellText, Node, processItem->CpuUsage);
        break;
    case WSLTNC_MEMORY:
        WslpSetSizeCellText(GetCellText, WslpGetNodeMemory(Node), Node->MemoryText, sizeof(Node->MemoryText));
        break;
    case WSLTNC_IMAGE:
        {
            PPH_STRING kernel;

            if (WslpGetVmState() == WslDistroStateRunning && (kernel = WslpGetKernelText()))
            {
                static CONST PH_STRINGREF prefix = PH_STRINGREF_INIT(L"kernel ");

                PhMoveReference(&Node->ImageText, PhConcatStringRef2(&prefix, &kernel->sr));
                GetCellText->Text = Node->ImageText->sr;
                PhDereferenceObject(kernel);
            }
        }
        break;
    case WSLTNC_STATUS:
        if (processItem)
        {
            LARGE_INTEGER now;

            PhQuerySystemTime(&now);
            WslpSetUptimeCellText(GetCellText, Node, now.QuadPart > processItem->CreateTime.QuadPart ? now.QuadPart - processItem->CreateTime.QuadPart : 0);
        }
        break;
    case WSLTNC_VERSION:
        PhInitializeStringRef(&GetCellText->Text, L"2");
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
    PH_FORMAT format;

    switch (GetCellText->Id)
    {
    case WSLTNC_NAME:
        GetCellText->Text = PhGetStringRef(Node->NameText);
        break;
    case WSLTNC_TYPE:
        if (distro->Version == 1)
            PhInitializeStringRef(&GetCellText->Text, L"Distribution (WSL 1)");
        else
            PhInitializeStringRef(&GetCellText->Text, L"Distribution");
        break;
    case WSLTNC_STATE:
        GetCellText->Text = *WslGetDistroStateText(distro->State);
        break;
    case WSLTNC_CPU:
        if (distro->Processes && distro->Processes->HaveCpuUsage)
            WslpSetCpuCellText(GetCellText, Node, distro->Processes->CpuUsage);
        break;
    case WSLTNC_MEMORY:
        WslpSetSizeCellText(GetCellText, WslpGetNodeMemory(Node), Node->MemoryText, sizeof(Node->MemoryText));
        break;
    case WSLTNC_IMAGE:
        GetCellText->Text = PhGetStringRef(distro->OsName);
        break;
    case WSLTNC_DISK:
        WslpSetSizeCellText(GetCellText, distro->VhdSize, Node->VhdSizeText, sizeof(Node->VhdSizeText));
        break;
    case WSLTNC_STATUS:
        WslpSetUptimeCellText(GetCellText, Node, WslpGetDistroRunTime(distro));
        break;
    case WSLTNC_VERSION:
        if (distro->Version != 0)
        {
            PhInitFormatU(&format, distro->Version);
            WslpSetCellText(GetCellText, &format, 1, Node->VersionText, sizeof(Node->VersionText));
        }
        break;
    case WSLTNC_LOCATION:
        GetCellText->Text = PhGetStringRef(distro->BasePath);
        break;
    }
}

/**
 * Formats the cell text of a Linux process node.
 */
static VOID WslpGetProcessCellText(
    _In_ PWSL_NODE Node,
    _In_ PWSL_PROCESS_FRAME Frame,
    _Inout_ PPH_TREENEW_GET_CELL_TEXT GetCellText
    )
{
    PWSL_LINUX_PROCESS process = Node->LinuxProcess;

    switch (GetCellText->Id)
    {
    case WSLTNC_NAME:
        GetCellText->Text = PhGetStringRef(process->Name);
        break;
    case WSLTNC_LINUXPID:
        WslpSetNumberCellText(GetCellText, Node, process->ProcessId);
        break;
    case WSLTNC_TYPE:
        PhInitializeStringRef(&GetCellText->Text, L"Linux process");
        break;
    case WSLTNC_STATE:
        GetCellText->Text = *WslGetLinuxProcessStateText(process->State);
        break;
    case WSLTNC_CPU:
        if (process->HaveCpuUsage)
            WslpSetCpuCellText(GetCellText, Node, process->CpuUsage);
        break;
    case WSLTNC_MEMORY:
        WslpSetSizeCellText(GetCellText, process->ResidentBytes, Node->MemoryText, sizeof(Node->MemoryText));
        break;
    case WSLTNC_STATUS:
        WslpSetUptimeCellText(GetCellText, Node, WslpGetLinuxRunTime(Frame, process->StartTime));
        break;
    }
}

/**
 * Formats the cell text of a WSLC session node.
 */
static VOID WslpGetSessionCellText(
    _In_ PWSL_NODE Node,
    _Inout_ PPH_TREENEW_GET_CELL_TEXT GetCellText
    )
{
    PWSL_SESSION session = Node->Session;

    switch (GetCellText->Id)
    {
    case WSLTNC_NAME:
        GetCellText->Text = PhGetStringRef(Node->NameText);
        break;
    case WSLTNC_PID:
        if (WslSessionVmProcessItem)
            WslpSetNumberCellText(GetCellText, Node, HandleToUlong(WslSessionVmProcessItem->ProcessId));
        break;
    case WSLTNC_TYPE:
        PhInitializeStringRef(&GetCellText->Text, L"Container VM");
        break;
    case WSLTNC_STATE:
        // wslc only lists running sessions.
        GetCellText->Text = *WslGetDistroStateText(WslDistroStateRunning);
        break;
    case WSLTNC_CPU:
        // The VM's CPU usage includes the session's own processes, not only the containers'.
        if (WslSessionVmProcessItem)
            WslpSetCpuCellText(GetCellText, Node, WslSessionVmProcessItem->CpuUsage);
        else if (session->HaveStats)
            WslpSetCpuCellText(GetCellText, Node, session->CpuUsage);
        break;
    case WSLTNC_STATUS:
        if (WslSessionVmProcessItem)
        {
            LARGE_INTEGER now;

            PhQuerySystemTime(&now);
            WslpSetUptimeCellText(GetCellText, Node, now.QuadPart > WslSessionVmProcessItem->CreateTime.QuadPart ? now.QuadPart - WslSessionVmProcessItem->CreateTime.QuadPart : 0);
        }
        break;
    case WSLTNC_MEMORY:
        WslpSetSizeCellText(GetCellText, WslpGetNodeMemory(Node), Node->MemoryText, sizeof(Node->MemoryText));
        break;
    }
}

/**
 * Formats the cell text of a container node.
 */
static VOID WslpGetContainerCellText(
    _In_ PWSL_NODE Node,
    _Inout_ PPH_TREENEW_GET_CELL_TEXT GetCellText
    )
{
    PWSL_CONTAINER container = Node->Container;

    switch (GetCellText->Id)
    {
    case WSLTNC_NAME:
        GetCellText->Text = PhGetStringRef(container->Name);
        break;
    case WSLTNC_TYPE:
        PhInitializeStringRef(&GetCellText->Text, L"Container");
        break;
    case WSLTNC_STATE:
        PhMoveReference(&Node->StateText, WslpGetContainerStateText(container));
        GetCellText->Text = Node->StateText->sr;
        break;
    case WSLTNC_CPU:
        if (container->HaveStats)
            WslpSetCpuCellText(GetCellText, Node, container->CpuUsage);
        break;
    case WSLTNC_MEMORY:
        WslpSetSizeCellText(GetCellText, WslpGetNodeMemory(Node), Node->MemoryText, sizeof(Node->MemoryText));
        break;
    case WSLTNC_IMAGE:
        GetCellText->Text = PhGetStringRef(container->Image);
        break;
    case WSLTNC_PORTS:
        GetCellText->Text = PhGetStringRef(container->Ports);
        break;
    case WSLTNC_STATUS:
        GetCellText->Text = PhGetStringRef(container->Status);
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

    // The tool's message says why, e.g. "Container '...' is not running."; the status alone
    // would only say the command failed.
    if (context->Message)
        PhShowError2(SystemInformer_GetWindowHandle(), PhGetString(context->Description), L"%s", context->Message->Buffer);
    else
        PhShowStatus(SystemInformer_GetWindowHandle(), PhGetString(context->Description), context->Status, 0);

    PhClearReference(&context->Message);
    PhDereferenceObject(context->Arguments);
    PhDereferenceObject(context->Description);
    PhFree(context);
}

/**
 * Gets the error message a failed wsl.exe or wslc.exe command printed.
 *
 * \param Output The command output (UTF-8).
 * \return The message lines, without wslc's "If this error was unexpected..." footer, or NULL
 * if the command printed nothing.
 */
static PPH_STRING WslpGetCommandErrorMessage(
    _In_ PPH_BYTES Output
    )
{
    static CONST PH_STRINGREF footer = PH_STRINGREF_INIT(L"If this error was unexpected");
    static CONST PH_STRINGREF whitespace = PH_STRINGREF_INIT(L" \t\r");
    static CONST PH_STRINGREF newLine = PH_STRINGREF_INIT(L"\n");
    PPH_STRING text;
    PPH_STRING message = NULL;
    PH_STRINGREF remaining;
    PH_STRINGREF line;

    if (!(text = PhConvertUtf8ToUtf16Ex(Output->Buffer, Output->Length)))
        return NULL;

    remaining = text->sr;

    while (remaining.Length != 0)
    {
        PhSplitStringRefAtChar(&remaining, L'\n', &line, &remaining);
        PhTrimStringRef(&line, &whitespace, 0);

        if (line.Length == 0)
            continue;
        if (PhStartsWithStringRef(&line, &footer, TRUE))
            break;

        if (message)
            PhMoveReference(&message, PhConcatStringRef3(&message->sr, &newLine, &line));
        else
            message = PhCreateString2(&line);
    }

    PhDereferenceObject(text);

    return message;
}

/**
 * Runs a wsl.exe or wslc.exe action off the GUI thread; "--shutdown" can take several seconds.
 */
_Function_class_(USER_THREAD_START_ROUTINE)
static NTSTATUS NTAPI WslpActionThread(
    _In_ PVOID Parameter
    )
{
    PWSL_ACTION_CONTEXT context = Parameter;
    NTSTATUS status;
    PPH_BYTES output = NULL;

    status = WslRunCommandEx(context->FileName, &context->Arguments->sr, &output, TRUE);
    context->Status = status;

    if (output)
    {
        if (!NT_SUCCESS(status))
            context->Message = WslpGetCommandErrorMessage(output);

        PhDereferenceObject(output);
    }

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
 * Starts a wsl.exe or wslc.exe action in the background.
 *
 * \param FileName The executable, from WslGetWslFileName or WslGetWslcFileName.
 * \param Arguments The arguments. This function takes ownership of the string.
 * \param Description The error text shown if the action fails.
 */
static VOID WslpStartAction(
    _In_ PPH_STRING FileName,
    _In_ PPH_STRING Arguments,
    _In_ PCWSTR Description
    )
{
    PWSL_ACTION_CONTEXT context;

    context = PhAllocateZero(sizeof(WSL_ACTION_CONTEXT));
    context->FileName = FileName;
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
 * Gets the first published TCP host port of a container.
 *
 * \param Ports The ports text from wslc list, e.g. "0.0.0.0:8080->80/tcp, [::]:8080->80/tcp".
 * \return The host port, or 0 if the container publishes no TCP port.
 */
static USHORT WslpGetContainerHostPort(
    _In_opt_ PPH_STRING Ports
    )
{
    static CONST PH_STRINGREF arrow = PH_STRINGREF_INIT(L"->");
    static CONST PH_STRINGREF tcp = PH_STRINGREF_INIT(L"/tcp");
    static CONST PH_STRINGREF space = PH_STRINGREF_INIT(L" ");
    PH_STRINGREF remaining;

    if (PhIsNullOrEmptyString(Ports))
        return 0;

    remaining = Ports->sr;

    while (remaining.Length != 0)
    {
        PH_STRINGREF entry;
        PH_STRINGREF host;
        PH_STRINGREF container;
        PH_STRINGREF address;
        PH_STRINGREF port;
        ULONG64 value;

        PhSplitStringRefAtChar(&remaining, L',', &entry, &remaining);
        PhTrimStringRef(&entry, &space, 0);

        // "<address>:<host port>-><container port>/<protocol>"; only published TCP ports have a host part.
        if (!PhSplitStringRefAtString(&entry, &arrow, FALSE, &host, &container) || !PhEndsWithStringRef(&container, &tcp, TRUE))
            continue;

        if (PhSplitStringRefAtLastChar(&host, L':', &address, &port) && PhStringToUInt64(&port, 10, &value) && value != 0 && value <= USHRT_MAX)
            return (USHORT)value;
    }

    return 0;
}

/**
 * Selects the process of a VM in the Processes tab.
 */
static VOID WslpGoToVmProcess(
    _In_opt_ PPH_PROCESS_ITEM ProcessItem
    )
{
    PPH_PROCESS_NODE processNode;

    if (ProcessItem && (processNode = PhFindProcessNode(ProcessItem->ProcessId)))
    {
        SystemInformer_SelectTabPage(0);
        SystemInformer_SelectProcessNode(processNode);
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

            if (!WslIsSafeDistroName(node->Distro->Name))
            {
                PhShowStatus(WindowHandle, L"Unable to terminate the distribution.", STATUS_INVALID_PARAMETER, 0);
                break;
            }

            if (PhShowConfirmMessage(
                WindowHandle,
                L"terminate",
                node->Distro->Name->Buffer,
                L"All processes in the distribution will be stopped.",
                TRUE
                ))
            {
                WslpStartAction(
                    WslGetWslFileName(),
                    PhFormatString(L"--terminate %s", node->Distro->Name->Buffer),
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
                WslpStartAction(WslGetWslFileName(), PhCreateString(L"--shutdown"), L"Unable to shut down WSL.");
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
            if (!node)
                break;

            // The WSL VM for its own row, the session VM for a session and its containers.
            if (node->Type == WslNodeTypeSession || node->Type == WslNodeTypeContainer)
                WslpGoToVmProcess(WslSessionVmProcessItem);
            else
                WslpGoToVmProcess(WslVmProcessItem);
        }
        break;
    case ID_WSL_CONTAINEROPENPORT:
        {
            USHORT port;

            if (!node || !node->Container || !(port = WslpGetContainerHostPort(node->Container->Ports)))
                break;

            PhShellExecute(WindowHandle, PhaFormatString(L"http://localhost:%hu/", port)->Buffer, NULL);
        }
        break;
    case ID_WSL_CONTAINERSHELL:
    case ID_WSL_CONTAINERLOGS:
        {
            NTSTATUS status;

            if (!node || !node->Container)
                break;

            if (!NT_SUCCESS(status = WslStartContainerConsole(node->Session->Name, node->Container->Id, Id == ID_WSL_CONTAINERLOGS)))
                PhShowStatus(WindowHandle, Id == ID_WSL_CONTAINERLOGS ? L"Unable to show the container logs." : L"Unable to open a shell.", status, 0);
        }
        break;
    case ID_WSL_CONTAINERINSPECT:
        {
            NTSTATUS status;

            if (!node || !node->Container)
                break;

            if (!NT_SUCCESS(status = WslShowContainerInspect(node->Session->Name, node->Container->Id, node->Container->Name)))
                PhShowStatus(WindowHandle, L"Unable to inspect the container.", status, 0);
        }
        break;
    case ID_WSL_CONTAINERSTOP:
    case ID_WSL_CONTAINERRESTART:
    case ID_WSL_CONTAINERKILL:
    case ID_WSL_CONTAINERREMOVE:
        {
            PCWSTR verb;

            switch (Id)
            {
            case ID_WSL_CONTAINERSTOP:
                verb = L"stop";
                break;
            case ID_WSL_CONTAINERRESTART:
                verb = L"restart";
                break;
            case ID_WSL_CONTAINERKILL:
                verb = L"kill";
                break;
            default:
                verb = L"remove";
                break;
            }

            if (!node || !node->Container || !WslGetWslcFileName())
                break;

            if (!WslIsSafeSessionName(node->Session->Name) || !WslIsSafeContainerId(node->Container->Id))
            {
                PhShowStatus(WindowHandle, L"Unable to control the container.", STATUS_INVALID_PARAMETER, 0);
                break;
            }

            // Killing skips the container's shutdown and removing deletes its filesystem, so both
            // ask first; stop and restart do not.
            if (Id == ID_WSL_CONTAINERKILL && !PhShowConfirmMessage(
                WindowHandle,
                L"kill",
                node->Container->Name->Buffer,
                L"The container's processes will be killed without a chance to shut down.",
                TRUE
                ))
            {
                break;
            }

            if (Id == ID_WSL_CONTAINERREMOVE && !PhShowConfirmMessage(
                WindowHandle,
                L"remove",
                node->Container->Name->Buffer,
                L"The container and any changes made to its filesystem will be deleted.",
                TRUE
                ))
            {
                break;
            }

            WslpStartAction(
                WslGetWslcFileName(),
                PhFormatString(L"--session \"%s\" %s %s", node->Session->Name->Buffer, verb, node->Container->Id->Buffer),
                L"Unable to control the container."
                );
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
    PPH_STRING portText = NULL; // Kept alive until the menu is destroyed
    USHORT port;

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
    else if (node->Type == WslNodeTypeContainer)
    {
        PhInsertEMenuItem(menu, PhCreateEMenuItem(0, ID_WSL_CONTAINERSHELL, L"Open &shell", NULL, NULL), ULONG_MAX);
        PhInsertEMenuItem(menu, PhCreateEMenuItem(0, ID_WSL_CONTAINERLOGS, L"&Logs", NULL, NULL), ULONG_MAX);
        PhInsertEMenuItem(menu, PhCreateEMenuItem(0, ID_WSL_CONTAINERINSPECT, L"&Inspect...", NULL, NULL), ULONG_MAX);
        PhInsertEMenuItem(menu, PhCreateEMenuSeparator(), ULONG_MAX);
        PhInsertEMenuItem(menu, PhCreateEMenuItem(0, ID_WSL_CONTAINERSTOP, L"S&top", NULL, NULL), ULONG_MAX);
        PhInsertEMenuItem(menu, PhCreateEMenuItem(0, ID_WSL_CONTAINERRESTART, L"&Restart", NULL, NULL), ULONG_MAX);
        PhInsertEMenuItem(menu, PhCreateEMenuItem(0, ID_WSL_CONTAINERKILL, L"&Kill", NULL, NULL), ULONG_MAX);

        // wslc refuses to remove a running container, so Remove is only offered once it stopped.
        if (!node->Container->Running)
            PhInsertEMenuItem(menu, PhCreateEMenuItem(0, ID_WSL_CONTAINERREMOVE, L"Re&move", NULL, NULL), ULONG_MAX);

        PhInsertEMenuItem(menu, PhCreateEMenuSeparator(), ULONG_MAX);

        if (port = WslpGetContainerHostPort(node->Container->Ports))
        {
            PhMoveReference(&portText, PhFormatString(L"&Open port %hu in browser", port));
            PhInsertEMenuItem(menu, PhCreateEMenuItem(0, ID_WSL_CONTAINEROPENPORT, portText->Buffer, NULL, NULL), ULONG_MAX);
        }

        PhInsertEMenuItem(menu, PhCreateEMenuItem(0, ID_WSL_GOTOPROCESS, L"&Go to VM process", NULL, NULL), ULONG_MAX);

        if (!node->Container->Running)
        {
            PhEnableEMenuItem(menu, ID_WSL_CONTAINERSHELL, FALSE);
            PhEnableEMenuItem(menu, ID_WSL_CONTAINERSTOP, FALSE);
            PhEnableEMenuItem(menu, ID_WSL_CONTAINERKILL, FALSE);
            PhEnableEMenuItem(menu, ID_WSL_CONTAINEROPENPORT, FALSE);
        }

        if (!WslSessionVmProcessItem)
            PhEnableEMenuItem(menu, ID_WSL_GOTOPROCESS, FALSE);
    }
    else if (node->Type == WslNodeTypeSession)
    {
        PhInsertEMenuItem(menu, PhCreateEMenuItem(0, ID_WSL_GOTOPROCESS, L"&Go to VM process", NULL, NULL), ULONG_MAX);

        if (!WslSessionVmProcessItem)
            PhEnableEMenuItem(menu, ID_WSL_GOTOPROCESS, FALSE);
    }
    else if (node->Type == WslNodeTypeDistro)
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

    // A Linux process only offers Copy, so it needs no separator.
    if (node->Type != WslNodeTypeLinuxProcess)
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
    PhClearReference(&portText);
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
            else if (node->Children)
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

            isLeaf->IsLeaf = !node->Children || node->Children->Count == 0;
        }
        return TRUE;
    case TreeNewGetCellText:
        {
            PPH_TREENEW_GET_CELL_TEXT getCellText = Parameter1;
            PWSL_NODE node = (PWSL_NODE)getCellText->Node;

            if (node->Type == WslNodeTypeVm)
                WslpGetVmCellText(node, getCellText);
            else if (node->Type == WslNodeTypeDistro)
                WslpGetDistroCellText(node, getCellText);
            else if (node->Type == WslNodeTypeLinuxProcess)
                WslpGetProcessCellText(node, node->Frame, getCellText);
            else if (node->Type == WslNodeTypeSession)
                WslpGetSessionCellText(node, getCellText);
            else
                WslpGetContainerCellText(node, getCellText);

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
                stopped = WslpGetVmState() != WslDistroStateRunning;
            else if (node->Type == WslNodeTypeDistro)
                stopped = node->Distro->State != WslDistroStateRunning;
            else if (node->Type == WslNodeTypeContainer)
                stopped = !node->Container->Running;
            else
                stopped = FALSE;

            if (stopped)
                getNodeColor->ForeColor = GetSysColor(COLOR_GRAYTEXT);

            getNodeColor->Flags = TN_CACHE;
        }
        return TRUE;
    case TreeNewGetCellTooltip:
        {
            PPH_TREENEW_GET_CELL_TOOLTIP getCellTooltip = Parameter1;
            PWSL_NODE node = (PWSL_NODE)getCellTooltip->Node;

            if (getCellTooltip->Column->Id == WSLTNC_MEMORY)
            {
                getCellTooltip->Text = *WslpGetMemoryTooltip(node);
                getCellTooltip->Unfolding = FALSE;
                getCellTooltip->MaximumWidth = ULONG_MAX;
                return TRUE;
            }

            // Distribution and Linux process uptimes come from /proc/uptime inside the VM, which
            // stops while the VM is paused; a container's status is wall-clock time from wslc.
            if (getCellTooltip->Column->Id == WSLTNC_STATUS && (node->Type == WslNodeTypeDistro || node->Type == WslNodeTypeLinuxProcess))
            {
                getCellTooltip->Text = WslGuestUptimeTooltipText;
                getCellTooltip->Unfolding = FALSE;
                getCellTooltip->MaximumWidth = ULONG_MAX;
                return TRUE;
            }

            if (getCellTooltip->Column->Id != WSLTNC_NAME)
                return FALSE;

            if (node->Type == WslNodeTypeVm && !WslVmProcessItem && WslVmCandidates > 1)
            {
                getCellTooltip->Text = WslAmbiguousVmText;
            }
            else if (node->Type == WslNodeTypeDistro && node->Distro->Default)
            {
                getCellTooltip->Text = WslDefaultTooltipText;
            }
            else if (node->Type == WslNodeTypeContainer)
            {
                PWSL_CONTAINER container = node->Container;

                if (!node->TooltipText)
                {
                    if (PhIsNullOrEmptyString(container->Ports))
                        node->TooltipText = container->Status ? PhReferenceObject(container->Status) : PhReferenceEmptyString();
                    else
                        node->TooltipText = PhFormatString(L"%s\nPorts: %s", PhGetString(container->Status), container->Ports->Buffer);
                }

                getCellTooltip->Text = node->TooltipText->sr;
            }
            else
            {
                return FALSE;
            }

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
    PhAddTreeNewColumn(WindowHandle, WSLTNC_PID, TRUE, L"PID", 50, PH_ALIGN_RIGHT, 1, DT_RIGHT);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_LINUXPID, TRUE, L"PID (Linux)", 75, PH_ALIGN_RIGHT, 2, DT_RIGHT);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_TYPE, TRUE, L"Type", 90, PH_ALIGN_LEFT, 3, 0);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_STATE, TRUE, L"State", 90, PH_ALIGN_LEFT, 4, 0);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_CPU, TRUE, L"CPU", 45, PH_ALIGN_RIGHT, 5, DT_RIGHT);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_MEMORY, TRUE, L"Memory", 80, PH_ALIGN_RIGHT, 6, DT_RIGHT);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_IMAGE, TRUE, L"Image / OS", 120, PH_ALIGN_LEFT, 7, 0);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_PORTS, TRUE, L"Ports", 130, PH_ALIGN_LEFT, 8, 0);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_DISK, TRUE, L"Disk", 70, PH_ALIGN_RIGHT, 9, DT_RIGHT);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_STATUS, TRUE, L"Uptime / Status", 120, PH_ALIGN_LEFT, 10, 0);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_VERSION, FALSE, L"Version", 50, PH_ALIGN_RIGHT, ULONG_MAX, DT_RIGHT);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_LOCATION, FALSE, L"Location", 300, PH_ALIGN_LEFT, ULONG_MAX, DT_PATH_ELLIPSIS);

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
            WslSessionNodes = PhCreateList(2);

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

            WslSetProviderEnabled(WSL_PROVIDER_TAB, WslTabSelected);
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
