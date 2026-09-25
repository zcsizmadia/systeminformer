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
#include <toolstatusintf.h>

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
    // Container columns, hidden by default. These come from the container list.
    WSLTNC_CONTAINERID,
    WSLTNC_COMMAND,
    WSLTNC_CREATED,
    WSLTNC_IPADDRESS,
    WSLTNC_NETWORK,
    WSLTNC_MOUNTS,
    WSLTNC_COMPOSE,
    WSLTNC_HEALTH,
    // These need the inspect output of each container, which is only asked for while one is shown.
    WSLTNC_PLATFORM,
    WSLTNC_RESTART,
    WSLTNC_EXITCODE,
    WSLTNC_MEMORYLIMIT,
    WSLTNC_CPULIMIT,
    WSLTNC_PRIVILEGED,
    WSLTNC_USER,
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
    PWSL_SESSION Session; // Owned by WslCurrentSnapshot; session nodes and their container nodes
    PWSL_ENGINE Engine; // Owned by WslCurrentSnapshot; the engine a distribution hosts, and its container nodes
    PWSL_CONTAINER Container; // Owned by WslCurrentSnapshot; container nodes only
    PPH_STRING NameText; // Distribution name, followed by " *" for the default one
    PPH_STRING TooltipText; // Cached name column tooltip
    PPH_STRING StateText; // Cached state of a container that exited, e.g. "Exited (137)"
    PPH_STRING ImageText; // Cached Image / OS text of the VM, a session or an engine's distribution
    PPH_STRING StatusText; // Cached uptime or container status
    // VM node: its distribution nodes, which WslDistroNodes owns.
    // Distribution, session and container nodes: their process and container nodes, which they own.
    PPH_LIST Children;
    ULONG64 CreateTime; // Tick count when the node was added, for the new row highlight; 0 for no highlight
    ULONG64 RemoveTime; // Tick count when the item went away; the node stays for the removed row highlight
    PWSL_SNAPSHOT RemovedSnapshot; // Keeps what the pointers above point into while a removed node stays
    BOOLEAN Seen; // Scratch flag while a snapshot is applied
    BOOLEAN Populated; // The children were matched once, so later ones are new and are highlighted
    BOOLEAN ShowsContainers; // Distribution nodes: the children are an engine's containers, not processes
    ULONG InitProcessId; // Container nodes: the container's first process, or 0 if none runs

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
    PPH_STRING Arguments; // Command line arguments, or the request path for an engine
    PPH_STRING PipeName; // A Docker API engine to send the request to, instead of running FileName
    PCSTR Method; // The request method for an engine, e.g. "POST"
    PPH_STRING Description;
    NTSTATUS Status;
    PPH_STRING Message; // the tool's own error text, if it printed one
} WSL_ACTION_CONTEXT, *PWSL_ACTION_CONTEXT;

// Drops the clean page cache of a VM's kernel, and compacts memory where the kernel can, so
// that WSL returns the freed memory to Windows. Nothing is lost; files are read again when
// needed. The script is one double-quoted argument, so it must not contain double quotes.
#define WSL_RECLAIM_SCRIPT \
    L"sync; echo 1 > /proc/sys/vm/drop_caches && " \
    L"{ [ -w /proc/sys/vm/compact_memory ] && echo 1 > /proc/sys/vm/compact_memory; true; }"

static PPH_MAIN_TAB_PAGE WslPage = NULL;
static HWND WslTreeNewHandle = NULL;
static ULONG WslTreeNewSortColumn = WSLTNC_NAME;
static PH_SORT_ORDER WslTreeNewSortOrder = AscendingSortOrder;
static BOOLEAN WslTabSelected = FALSE;
static BOOLEAN WslUpdateAutomatically = TRUE; // View > Update automatically

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

// New and removed rows are highlighted like on the Processes tab, with its settings. They are read
// for each snapshot, which also sets the time the highlights are measured against.
static ULONG64 WslUpdateTime = 0;
static ULONG WslHighlightingDuration = 0;
static COLORREF WslColorNew = 0;
static COLORREF WslColorRemoved = 0;

// The icons the tab adds to the process image list, by file and resource.
typedef struct _WSL_ICON
{
    PPH_STRING FileName;
    LONG IconIndex; // As for PhExtractIconEx: an index, or a negative resource id
    ULONG ImageIndex; // In the process image list; 0, the generic icon, if the file has none
} WSL_ICON, *PWSL_ICON;

static PPH_LIST WslIcons = NULL; // PWSL_ICON; a DPI change empties the image list, and so this

// The ToolStatus search box, or NULL when that plugin is not loaded.
static PTOOLSTATUS_INTERFACE WslToolStatusInterface = NULL;
static PH_CALLBACK_REGISTRATION WslSearchChangedRegistration;

static CONST PH_STRINGREF WslPageText = PH_STRINGREF_INIT(L"WSL");
static CONST PH_STRINGREF WslSearchBannerText = PH_STRINGREF_INIT(L"Search WSL");
static CONST PH_STRINGREF WslVmNodeText = PH_STRINGREF_INIT(L"WSL");
static CONST PH_STRINGREF WslEmptyText = PH_STRINGREF_INIT(L"No WSL distributions are registered for this user.");
static CONST PH_STRINGREF WslAmbiguousVmText = PH_STRINGREF_INIT(L"Several virtual machine processes exist; the WSL one cannot be identified.");
static CONST PH_STRINGREF WslDefaultMarker = PH_STRINGREF_INIT(L" *");
static CONST PH_STRINGREF WslDefaultTooltipText = PH_STRINGREF_INIT(L"Default distribution");
static CONST PH_STRINGREF WslGuestUptimeTooltipText = PH_STRINGREF_INIT(L"Time the VM has been running since the start, from the VM's own clock. It leaves out time the VM was paused, e.g. while the host was asleep.");

static BOOLEAN WslpFilterNodes(
    _In_ PPH_LIST Nodes,
    _In_ BOOLEAN Expand
    );

/**
 * Creates a tree node.
 *
 * \param Type The node type.
 * \param Id The distribution id, or NULL for the VM node.
 * \param Highlight TRUE to highlight the node as new.
 * \return The new node.
 */
static PWSL_NODE WslpCreateNode(
    _In_ WSL_NODE_TYPE Type,
    _In_opt_ PPH_STRING Id,
    _In_ BOOLEAN Highlight
    )
{
    PWSL_NODE node;

    node = PhAllocateZero(sizeof(WSL_NODE));
    PhInitializeTreeNewNode(&node->Node);
    node->Node.TextCache = node->TextCache;
    node->Node.TextCacheSize = WSLTNC_MAXIMUM;
    node->Type = Type;
    node->CreateTime = Highlight && WslHighlightingDuration ? WslUpdateTime : 0;

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
    PhClearReference(&Node->RemovedSnapshot);
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

        // A removed node stays only for its highlight; one that comes back gets a new node.
        if (!node->RemoveTime && PhEqualString(node->Id, Id, TRUE))
            return node;
    }

    return NULL;
}

/**
 * Finds a child node, e.g. the node of a container under its session.
 *
 * \param Parent The parent node.
 * \param Id The child's id, e.g. the container id.
 * \return The node, or NULL if there is none.
 */
static PWSL_NODE WslpFindChildNode(
    _In_ PWSL_NODE Parent,
    _In_ PPH_STRING Id
    )
{
    for (ULONG i = 0; i < Parent->Children->Count; i++)
    {
        PWSL_NODE node = Parent->Children->Items[i];

        // Process nodes have no id.
        if (node->Id && !node->RemoveTime && PhEqualString(node->Id, Id, TRUE))
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
    PhInvalidateTreeNewNode(&Node->Node, TN_CACHE_COLOR | TN_CACHE_ICON);
}

/**
 * Removes the nodes of a list that were not seen in the snapshot being applied.
 *
 * \param Nodes The list, e.g. a node's children.
 * \param Type The type of node the list holds. Other nodes are removed at once without a
 * highlight, e.g. a distribution's process nodes once it hosts an engine, as their items did
 * not go away.
 * \remarks A node whose item went away stays for the highlighting duration. It keeps the
 * previous snapshot, which its pointers are into, and is no longer updated.
 */
static VOID WslpRemoveUnseenNodes(
    _In_ PPH_LIST Nodes,
    _In_ WSL_NODE_TYPE Type
    )
{
    for (ULONG i = Nodes->Count; i != 0; i--)
    {
        PWSL_NODE node = Nodes->Items[i - 1];

        if (node->Seen)
            continue;

        if (node->RemoveTime)
        {
            if (WslUpdateTime - node->RemoveTime < WslHighlightingDuration)
                continue;
        }
        else if (node->Type == Type && WslHighlightingDuration && WslCurrentSnapshot)
        {
            node->RemoveTime = WslUpdateTime;
            node->RemovedSnapshot = PhReferenceObject(WslCurrentSnapshot);
            PhInvalidateTreeNewNode(&node->Node, TN_CACHE_COLOR);
            continue;
        }

        PhRemoveItemList(Nodes, i - 1);
        WslpDestroyNode(node);
    }
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

    // A distribution's children are container nodes instead while it hosts an engine; those are
    // not matched here and so are removed below.
    for (ULONG i = 0; i < children->Count; i++)
    {
        PWSL_NODE child = children->Items[i];

        child->Seen = FALSE;

        if (child->Type == WslNodeTypeLinuxProcess && !child->RemoveTime)
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
            node = WslpCreateNode(WslNodeTypeLinuxProcess, NULL, ParentNode->Populated);
            PhAddItemList(children, node);
        }

        node->LinuxProcess = process;
        node->Frame = Frame;
        node->Seen = TRUE;
        WslpInvalidateNode(node);
    }

    PhDereferenceObject(nodeTable);

    WslpRemoveUnseenNodes(children, WslNodeTypeLinuxProcess);
    ParentNode->Populated = TRUE;

    // A container's own PID, as "docker inspect" reports it, is its first process: the one
    // whose parent is outside the container. The inspect output is not needed for it.
    if (ContainerId)
    {
        PWSL_LINUX_PROCESS initProcess = NULL;

        for (ULONG i = 0; i < children->Count; i++)
        {
            PWSL_NODE node = children->Items[i];
            BOOLEAN parentInside = FALSE;

            if (node->RemoveTime)
                continue;

            for (ULONG j = 0; j < children->Count && !parentInside; j++)
            {
                PWSL_NODE other = children->Items[j];

                parentInside = !other->RemoveTime && other->LinuxProcess->ProcessId == node->LinuxProcess->ParentProcessId;
            }

            if (!parentInside && (!initProcess || node->LinuxProcess->StartTime < initProcess->StartTime))
                initProcess = node->LinuxProcess;
        }

        ParentNode->InitProcessId = initProcess ? initProcess->ProcessId : 0;
    }
}

/**
 * Matches the children of a distribution that hosts Docker API engines to the engines'
 * containers, each with its processes, like the containers of a WSLC session.
 *
 * \param DistroNode The distribution node.
 * \param Engines The engines of the snapshot; those placed in the distribution are shown,
 * e.g. both Docker and Podman.
 * \param Frame The distribution's processes, or NULL.
 * \remarks The engines' own processes, e.g. dockerd and containerd, are not shown.
 */
static VOID WslpUpdateEngineNodes(
    _In_ PWSL_NODE DistroNode,
    _In_ PPH_LIST Engines,
    _In_opt_ PWSL_PROCESS_FRAME Frame
    )
{
    for (ULONG i = 0; i < DistroNode->Children->Count; i++)
        ((PWSL_NODE)DistroNode->Children->Items[i])->Seen = FALSE;

    for (ULONG e = 0; e < Engines->Count; e++)
    {
        PWSL_ENGINE engine = Engines->Items[e];

        if (!PhEqualString(engine->DistroId, DistroNode->Distro->Id, TRUE))
            continue;

        for (ULONG i = 0; i < engine->Containers->Count; i++)
        {
            PWSL_CONTAINER container = engine->Containers->Items[i];
            PWSL_NODE containerNode = WslpFindChildNode(DistroNode, container->Id);

            // The distribution's process nodes are removed below, as they are never seen here.
            if (containerNode && containerNode->Type != WslNodeTypeContainer)
                containerNode = NULL;

            if (!containerNode)
            {
                containerNode = WslpCreateNode(WslNodeTypeContainer, container->Id, DistroNode->Populated);
                PhAddItemList(DistroNode->Children, containerNode);
            }

            containerNode->Session = NULL;
            containerNode->Engine = engine;
            containerNode->Container = container;
            containerNode->Seen = TRUE;
            WslpInvalidateNode(containerNode);
            WslpUpdateProcessNodes(containerNode, Frame, container->Id);
        }
    }

    WslpRemoveUnseenNodes(DistroNode->Children, WslNodeTypeContainer);
    DistroNode->Populated = TRUE;
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

            if (!node->RemoveTime && PhEqualString(node->Id, session->Name, FALSE))
            {
                sessionNode = node;
                break;
            }
        }

        if (!sessionNode)
        {
            sessionNode = WslpCreateNode(WslNodeTypeSession, session->Name, !!WslCurrentSnapshot);
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
            PWSL_NODE containerNode = WslpFindChildNode(sessionNode, container->Id);

            if (!containerNode)
            {
                containerNode = WslpCreateNode(WslNodeTypeContainer, container->Id, sessionNode->Populated);
                PhAddItemList(sessionNode->Children, containerNode);
            }

            containerNode->Session = session;
            containerNode->Engine = NULL;
            containerNode->Container = container;
            containerNode->Seen = TRUE;
            WslpInvalidateNode(containerNode);
            WslpUpdateProcessNodes(containerNode, session->Processes, container->Id);
        }

        WslpRemoveUnseenNodes(sessionNode->Children, WslNodeTypeContainer);
        sessionNode->Populated = TRUE;
    }

    // Remove the nodes of sessions that ended.
    WslpRemoveUnseenNodes(WslSessionNodes, WslNodeTypeSession);
}

/**
 * Gets the node of the only running session.
 *
 * \return The node, or NULL if no session or several run.
 */
static PWSL_NODE WslpGetOnlySessionNode(
    VOID
    )
{
    PWSL_NODE found = NULL;

    for (ULONG i = 0; i < WslSessionNodes->Count; i++)
    {
        PWSL_NODE node = WslSessionNodes->Items[i];

        if (node->RemoveTime)
            continue;
        if (found)
            return NULL;

        found = node;
    }

    return found;
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
    else if (Selection == WslVmSelectionSession)
        node = WslpGetOnlySessionNode();

    if (!node)
        return FALSE;

    TreeNew_DeselectRange(WslTreeNewHandle, 0, -1);
    TreeNew_FocusMarkSelectNode(WslTreeNewHandle, &node->Node);
    TreeNew_EnsureVisible(WslTreeNewHandle, &node->Node);
    SetFocus(WslTreeNewHandle);

    return TRUE;
}

/**
 * Finds the Docker API engine placed in a distribution.
 *
 * \return The engine, or NULL if the distribution hosts none.
 */
static PWSL_ENGINE WslpFindDistroEngine(
    _In_opt_ PPH_LIST Engines,
    _In_ PPH_STRING DistroId
    )
{
    for (ULONG i = 0; Engines && i < Engines->Count; i++)
    {
        PWSL_ENGINE engine = Engines->Items[i];

        if (PhEqualString(engine->DistroId, DistroId, TRUE))
            return engine;
    }

    return NULL;
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

    WslUpdateTime = NtGetTickCount64();
    WslHighlightingDuration = PhGetIntegerSetting(L"HighlightingDuration");
    WslColorNew = PhGetIntegerSetting(L"ColorNew");
    WslColorRemoved = PhGetIntegerSetting(L"ColorRemoved");

    for (ULONG i = 0; i < WslDistroNodes->Count; i++)
        ((PWSL_NODE)WslDistroNodes->Items[i])->Seen = FALSE;

    for (ULONG i = 0; i < snapshot->Distributions->Count; i++)
    {
        PWSL_DISTRO_ITEM distro = snapshot->Distributions->Items[i];
        PWSL_NODE node;

        if (!(node = WslpFindDistroNode(distro->Id)))
        {
            // The first snapshot is what is there, not what is new.
            node = WslpCreateNode(WslNodeTypeDistro, distro->Id, !!WslCurrentSnapshot);
            PhAddItemList(WslDistroNodes, node);
        }

        node->Distro = distro;
        node->Seen = TRUE;

        if (distro->Default)
            PhMoveReference(&node->NameText, PhConcatStringRef2(&distro->Name->sr, &WslDefaultMarker));
        else
            PhSetReference(&node->NameText, distro->Name);

        node->Engine = WslpFindDistroEngine(snapshot->Engines, distro->Id);

        // The containers that replace the processes once an engine is found, or the other way
        // around, were there before, so they are not highlighted as new.
        if (node->ShowsContainers != !!node->Engine)
        {
            node->ShowsContainers = !!node->Engine;
            node->Populated = FALSE;
        }

        if (node->Engine)
            WslpUpdateEngineNodes(node, snapshot->Engines, distro->Processes);
        else
            WslpUpdateProcessNodes(node, distro->Processes, NULL);

        if (distro->Version == 2)
            hasWsl2 = TRUE;
    }

    // Remove nodes of distributions that were unregistered.
    WslpRemoveUnseenNodes(WslDistroNodes, WslNodeTypeDistro);

    WslpUpdateSessionNodes(snapshot->Sessions);

    // The previous snapshot owned the distribution items, process frames and sessions the nodes pointed to.
    PhMoveReference(&WslCurrentSnapshot, snapshot);

    if (hasWsl2 && !WslVmNode)
        WslVmNode = WslpCreateNode(WslNodeTypeVm, NULL, FALSE);

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

    // New nodes start shown, so the search is applied again after every refresh.
    WslpFilterNodes(WslRootNodes, FALSE);
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

    PhMoveReference(&WslVmProcessItem, WslReferenceVmProcessItem(&WslVmCandidates, NULL));
    PhMoveReference(&WslSessionVmProcessItem, WslpGetOnlySessionNode() ? WslReferenceSessionVmProcessItem() : NULL);

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
    static CONST PH_STRINGREF engineContainerText = PH_STRINGREF_INIT(L"Sum of the resident sets of the container's processes. Shared pages are counted once per process.");
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
        return Node->Engine ? &engineContainerText : &containerText;
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
 * Appends a "Label: value" line to a container tooltip, if there is a value.
 */
static VOID WslpAppendTooltipLine(
    _Inout_ PPH_STRING_BUILDER Builder,
    _In_ PCWSTR Label,
    _In_opt_ PPH_STRING Value
    )
{
    // A long command line would make the tooltip as wide as the screen.
    static CONST SIZE_T maximumLength = 200 * sizeof(WCHAR);
    PH_STRINGREF value;

    if (PhIsNullOrEmptyString(Value))
        return;

    value = Value->sr;

    if (Builder->String->Length != 0)
        PhAppendCharStringBuilder(Builder, L'\n');

    PhAppendStringBuilder2(Builder, (PWSTR)Label);
    PhAppendStringBuilder2(Builder, L": ");

    if (value.Length > maximumLength)
    {
        value.Length = maximumLength;
        PhAppendStringBuilder(Builder, &value);
        PhAppendCharStringBuilder(Builder, L'\x2026');
    }
    else
    {
        PhAppendStringBuilder(Builder, &value);
    }
}

/**
 * Formats the name column tooltip of a container from what the container list gives; the
 * Inspect window has the rest.
 */
static PPH_STRING WslpGetContainerTooltip(
    _In_ PWSL_CONTAINER Container
    )
{
    PH_STRING_BUILDER builder;
    PPH_STRING image = NULL;

    PhInitializeStringBuilder(&builder, 256);

    if (!PhIsNullOrEmptyString(Container->Image) && !PhIsNullOrEmptyString(Container->ImageId))
        image = PhFormatString(L"%s (%s)", Container->Image->Buffer, Container->ImageId->Buffer);
    else if (Container->Image)
        PhSetReference(&image, Container->Image);

    WslpAppendTooltipLine(&builder, L"Image", image);
    WslpAppendTooltipLine(&builder, L"Command", Container->Command);
    WslpAppendTooltipLine(&builder, L"Created", Container->Created);
    WslpAppendTooltipLine(&builder, L"Status", Container->Status);
    WslpAppendTooltipLine(&builder, L"Ports", Container->Ports);
    WslpAppendTooltipLine(&builder, L"Health", Container->Health);
    WslpAppendTooltipLine(&builder, L"Network", Container->Networks);
    WslpAppendTooltipLine(&builder, L"IP address", Container->IpAddresses);
    WslpAppendTooltipLine(&builder, L"Mounts", Container->Mounts);
    WslpAppendTooltipLine(&builder, L"Compose", Container->Compose);

    PhClearReference(&image);

    return PhFinalStringBuilderString(&builder);
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
 * Gets the Image / OS text of a distribution: its OS, or the Docker API engines it hosts, e.g.
 * "Docker Desktop 4.92.0, Docker 29.8.0" with a middle dot, separated by ", " when there are several.
 */
static PH_STRINGREF WslpGetDistroImageText(
    _In_ PWSL_NODE Node
    )
{
    if (!Node->Engine)
        return PhGetStringRef(Node->Distro->OsName);

    if (!Node->ImageText && WslCurrentSnapshot && WslCurrentSnapshot->Engines)
    {
        PPH_LIST engines = WslCurrentSnapshot->Engines;

        for (ULONG i = 0; i < engines->Count; i++)
        {
            PWSL_ENGINE engine = engines->Items[i];
            PPH_STRING part;

            if (!engine->ProductText || !PhEqualString(engine->DistroId, Node->Distro->Id, TRUE))
                continue;

            if (engine->EngineText)
                part = PhFormatString(L"%s \u00b7 %s", engine->ProductText->Buffer, engine->EngineText->Buffer);
            else
                part = PhReferenceObject(engine->ProductText);

            if (!part)
                continue;

            if (Node->ImageText)
            {
                PhMoveReference(&Node->ImageText, PhFormatString(L"%s, %s", Node->ImageText->Buffer, part->Buffer));
                PhDereferenceObject(part);
            }
            else
            {
                Node->ImageText = part;
            }
        }
    }

    return Node->ImageText ? Node->ImageText->sr : PhGetStringRef(Node->Distro->OsName);
}

/**
 * Compares two nodes for the current sort column.
 *
 * \remarks Siblings of different types, e.g. the VM, WSL 1 distributions and sessions at the
 * root, are grouped by type in WSL_NODE_TYPE order whatever the sort order.
 */
/**
 * Compares containers by a column that shows their inspect details. Containers without
 * details sort first.
 */
static int WslpCompareContainerDetails(
    _In_opt_ PWSL_CONTAINER_DETAILS Details1,
    _In_opt_ PWSL_CONTAINER_DETAILS Details2
    )
{
    if (!Details1 || !Details2)
        return intcmp(!!Details1, !!Details2);

    switch (WslTreeNewSortColumn)
    {
    case WSLTNC_PLATFORM:
        return PhCompareStringWithNull(Details1->Platform, Details2->Platform, TRUE);
    case WSLTNC_RESTART:
        return uintcmp(Details1->RestartCount, Details2->RestartCount);
    case WSLTNC_EXITCODE:
        if (!Details1->ExitText || !Details2->ExitText)
            return intcmp(!!Details1->ExitText, !!Details2->ExitText);
        return intcmp(Details1->ExitCode, Details2->ExitCode);
    case WSLTNC_MEMORYLIMIT:
        return uint64cmp(Details1->MemoryLimit, Details2->MemoryLimit);
    case WSLTNC_CPULIMIT:
        return uint64cmp(Details1->NanoCpus, Details2->NanoCpus);
    case WSLTNC_PRIVILEGED:
        return intcmp(Details1->Privileged, Details2->Privileged);
    case WSLTNC_USER:
        return PhCompareStringWithNull(Details1->User, Details2->User, TRUE);
    }

    return 0;
}

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
            {
                // By what the column shows, which for an engine's distribution is the engine.
                PH_STRINGREF text1 = WslpGetDistroImageText(node1);
                PH_STRINGREF text2 = WslpGetDistroImageText(node2);

                sortResult = PhCompareStringRef(&text1, &text2, TRUE);
            }
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
        case WSLTNC_LINUXPID:
            sortResult = uintcmp(node1->InitProcessId, node2->InitProcessId);
            break;
        case WSLTNC_CONTAINERID:
            sortResult = PhCompareString(container1->Id, container2->Id, TRUE);
            break;
        case WSLTNC_COMMAND:
            sortResult = PhCompareStringWithNull(container1->Command, container2->Command, TRUE);
            break;
        case WSLTNC_CREATED:
            sortResult = int64cmp(container1->CreatedTime.QuadPart, container2->CreatedTime.QuadPart);
            break;
        case WSLTNC_IPADDRESS:
            sortResult = PhCompareStringWithNull(container1->IpAddresses, container2->IpAddresses, TRUE);
            break;
        case WSLTNC_NETWORK:
            sortResult = PhCompareStringWithNull(container1->Networks, container2->Networks, TRUE);
            break;
        case WSLTNC_MOUNTS:
            sortResult = PhCompareStringWithNull(container1->Mounts, container2->Mounts, TRUE);
            break;
        case WSLTNC_COMPOSE:
            sortResult = PhCompareStringWithNull(container1->Compose, container2->Compose, TRUE);
            break;
        case WSLTNC_HEALTH:
            sortResult = PhCompareStringWithNull(container1->Health, container2->Health, TRUE);
            break;
        default:
            sortResult = WslpCompareContainerDetails(container1->Details, container2->Details);
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
    _Inout_updates_bytes_(BufferLength) PWSTR Buffer,
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
        PWSL_NODE node = WslDistroNodes->Items[i];
        PWSL_DISTRO_ITEM distro = node->Distro;

        if (!node->RemoveTime && distro->Version == 2 && distro->State == WslDistroStateRunning)
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
            PPH_STRING version;

            // The installed WSL, e.g. "WSL 2.9.12"; System Information shows the kernel.
            if (version = WslGetWslVersion())
            {
                static CONST PH_STRINGREF prefix = PH_STRINGREF_INIT(L"WSL ");

                if (!Node->ImageText)
                    Node->ImageText = PhConcatStringRef2(&prefix, &version->sr);

                GetCellText->Text = Node->ImageText->sr;
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
        GetCellText->Text = WslpGetDistroImageText(Node);
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
        GetCellText->Text = *WslGetDistroStateText(session->State);
        break;
    case WSLTNC_IMAGE:
        {
            PPH_STRING version;

            // The wslc that manages the session, e.g. "wslc 2.9.12".
            if (version = WslGetWslcVersion())
            {
                static CONST PH_STRINGREF prefix = PH_STRINGREF_INIT(L"wslc ");

                if (!Node->ImageText)
                    Node->ImageText = PhConcatStringRef2(&prefix, &version->sr);

                GetCellText->Text = Node->ImageText->sr;
            }
        }
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
    PWSL_CONTAINER_DETAILS details = container->Details;

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
    case WSLTNC_LINUXPID:
        if (Node->InitProcessId)
            WslpSetNumberCellText(GetCellText, Node, Node->InitProcessId);
        break;
    case WSLTNC_CONTAINERID:
        // As short as the Docker CLI shows it; a Docker API gives the full ID.
        GetCellText->Text = container->Id->sr;
        GetCellText->Text.Length = min(GetCellText->Text.Length, 12 * sizeof(WCHAR));
        break;
    case WSLTNC_COMMAND:
        GetCellText->Text = PhGetStringRef(container->Command);
        break;
    case WSLTNC_CREATED:
        GetCellText->Text = PhGetStringRef(container->Created);
        break;
    case WSLTNC_IPADDRESS:
        GetCellText->Text = PhGetStringRef(container->IpAddresses);
        break;
    case WSLTNC_NETWORK:
        GetCellText->Text = PhGetStringRef(container->Networks);
        break;
    case WSLTNC_MOUNTS:
        GetCellText->Text = PhGetStringRef(container->Mounts);
        break;
    case WSLTNC_COMPOSE:
        GetCellText->Text = PhGetStringRef(container->Compose);
        break;
    case WSLTNC_HEALTH:
        GetCellText->Text = PhGetStringRef(container->Health);
        break;
    case WSLTNC_PLATFORM:
        GetCellText->Text = PhGetStringRef(container->Platform ? container->Platform : (details ? details->Platform : NULL));
        break;
    case WSLTNC_RESTART:
        if (details)
            GetCellText->Text = PhGetStringRef(details->RestartText);
        break;
    case WSLTNC_EXITCODE:
        if (details)
            GetCellText->Text = PhGetStringRef(details->ExitText);
        break;
    case WSLTNC_MEMORYLIMIT:
        if (details)
            GetCellText->Text = PhGetStringRef(details->MemoryLimitText);
        break;
    case WSLTNC_CPULIMIT:
        if (details)
            GetCellText->Text = PhGetStringRef(details->CpuLimitText);
        break;
    case WSLTNC_PRIVILEGED:
        if (details)
            GetCellText->Text = PhGetStringRef(details->PrivilegedText);
        break;
    case WSLTNC_USER:
        if (details)
            GetCellText->Text = PhGetStringRef(details->User);
        break;
    }
}

/**
 * Gets the text of a cell of any node type.
 */
static VOID WslpGetNodeCellText(
    _In_ PWSL_NODE Node,
    _Inout_ PPH_TREENEW_GET_CELL_TEXT GetCellText
    )
{
    if (Node->Type == WslNodeTypeVm)
        WslpGetVmCellText(Node, GetCellText);
    else if (Node->Type == WslNodeTypeDistro)
        WslpGetDistroCellText(Node, GetCellText);
    else if (Node->Type == WslNodeTypeLinuxProcess)
        WslpGetProcessCellText(Node, Node->Frame, GetCellText);
    else if (Node->Type == WslNodeTypeSession)
        WslpGetSessionCellText(Node, GetCellText);
    else
        WslpGetContainerCellText(Node, GetCellText);
}

/**
 * Determines whether a node matches the text in the ToolStatus search box.
 *
 * \return TRUE if there is no search, or the text matches one of the text columns of the
 * node, or a container's id.
 */
static BOOLEAN WslpNodeMatchesSearch(
    _In_ PWSL_NODE Node
    )
{
    static CONST ULONG columns[] =
    {
        WSLTNC_NAME, WSLTNC_PID, WSLTNC_LINUXPID, WSLTNC_TYPE, WSLTNC_STATE, WSLTNC_IMAGE, WSLTNC_PORTS, WSLTNC_STATUS,
        WSLTNC_COMMAND, WSLTNC_IPADDRESS, WSLTNC_NETWORK, WSLTNC_MOUNTS, WSLTNC_COMPOSE, WSLTNC_HEALTH, WSLTNC_PLATFORM, WSLTNC_USER
    };

    if (!WslToolStatusInterface || !WslToolStatusInterface->GetSearchMatchHandle())
        return TRUE;

    // The rows show the short container id; a search for the full one should match too.
    if (Node->Type == WslNodeTypeContainer && WslToolStatusInterface->WordMatch(&Node->Id->sr))
        return TRUE;

    for (ULONG i = 0; i < RTL_NUMBER_OF(columns); i++)
    {
        PH_TREENEW_GET_CELL_TEXT getCellText;

        memset(&getCellText, 0, sizeof(PH_TREENEW_GET_CELL_TEXT));
        getCellText.Node = &Node->Node;
        getCellText.Id = columns[i];
        WslpGetNodeCellText(Node, &getCellText);

        if (getCellText.Text.Length != 0 && WslToolStatusInterface->WordMatch(&getCellText.Text))
            return TRUE;
    }

    return FALSE;
}

/**
 * Shows the nodes that match the search, and the nodes above them.
 *
 * \param Nodes The nodes of one level of the tree.
 * \param Expand TRUE to expand a node whose children match, so that the matches can be seen.
 * \return TRUE if any of the nodes is shown.
 * \remarks TreeNew moves the children of a hidden node to the top level, so a node stays
 * shown whenever one of its descendants is.
 */
static BOOLEAN WslpFilterNodes(
    _In_ PPH_LIST Nodes,
    _In_ BOOLEAN Expand
    )
{
    BOOLEAN anyVisible = FALSE;

    for (ULONG i = 0; i < Nodes->Count; i++)
    {
        PWSL_NODE node = Nodes->Items[i];
        BOOLEAN childVisible = node->Children ? WslpFilterNodes(node->Children, Expand) : FALSE;

        if (childVisible && Expand)
            node->Node.Expanded = TRUE;

        node->Node.Visible = childVisible || WslpNodeMatchesSearch(node);
        anyVisible |= node->Node.Visible;
    }

    return anyVisible;
}

/**
 * Applies the search when its text changes. Runs on the GUI thread.
 */
_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI WslpSearchChangedHandler(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    if (!WslTreeNewHandle || !WslRootNodes)
        return;

    // Expand only here, not on every refresh, so rows the user collapses stay collapsed.
    WslpFilterNodes(WslRootNodes, !!WslToolStatusInterface->GetSearchMatchHandle());
    TreeNew_NodesStructured(WslTreeNewHandle);
}

/**
 * Focuses the tree when Enter or Down is pressed in the search box, selecting the first row
 * if asked to.
 */
_Function_class_(TOOLSTATUS_TAB_ACTIVATE_CONTENT)
static VOID NTAPI WslpToolStatusActivateContent(
    _In_ BOOLEAN Select
    )
{
    SetFocus(WslTreeNewHandle);

    if (Select && TreeNew_GetFlatNodeCount(WslTreeNewHandle) > 0)
    {
        PPH_TREENEW_NODE node = TreeNew_GetFlatNode(WslTreeNewHandle, 0);

        TreeNew_DeselectRange(WslTreeNewHandle, 0, -1);
        TreeNew_FocusMarkSelectNode(WslTreeNewHandle, node);
        TreeNew_EnsureVisible(WslTreeNewHandle, node);
    }
}

/**
 * Gives ToolStatus the tree, e.g. for the search box to focus.
 */
_Function_class_(TOOLSTATUS_GET_TREENEW_HANDLE)
static HWND NTAPI WslpToolStatusGetTreeNewHandle(
    VOID
    )
{
    return WslTreeNewHandle;
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
 * Frees an action context.
 */
static VOID WslpFreeActionContext(
    _In_ PWSL_ACTION_CONTEXT Context
    )
{
    PhClearReference(&Context->Message);
    PhClearReference(&Context->PipeName);
    PhDereferenceObject(Context->Arguments);
    PhDereferenceObject(Context->Description);
    PhFree(Context);
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

    WslpFreeActionContext(context);
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
 * Gets the private bytes of a VM process, as the process provider last read them.
 *
 * \return The private bytes, or 0 if the process is gone. The VM process cannot be opened
 * without administrative rights, so it is not queried directly.
 */
static SIZE_T WslpGetVmPrivateBytes(
    _In_ HANDLE ProcessId
    )
{
    PPH_PROCESS_ITEM processItem;
    SIZE_T privateBytes = 0;

    if (processItem = PhReferenceProcessItem(ProcessId))
    {
        privateBytes = processItem->VmCounters.PagefileUsage;
        PhDereferenceObject(processItem);
    }

    return privateBytes;
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

    if (context->PipeName)
    {
        PPH_BYTES path = PhConvertUtf16ToUtf8Ex(context->Arguments->Buffer, context->Arguments->Length);
        ULONG statusCode;

        status = WslEngineRequest(context->PipeName, context->Method, path->Buffer, WSL_ACTION_TIMEOUT_MS, &statusCode, &output);
        PhDereferenceObject(path);

        // 304 is e.g. stopping a container that already stopped.
        if (NT_SUCCESS(status) && !(statusCode >= 200 && statusCode < 300) && statusCode != 304)
        {
            status = STATUS_UNSUCCESSFUL;

            if (!(context->Message = WslGetEngineErrorMessage(output)))
                context->Message = PhFormatString(L"The engine answered with HTTP status %lu.", statusCode);
        }
    }
    else
    {
        status = WslRunCommandEx(context->FileName, &context->Arguments->sr, WSL_ACTION_TIMEOUT_MS, &output, TRUE);

        if (output && !NT_SUCCESS(status))
            context->Message = WslpGetCommandErrorMessage(output);
    }

    PhClearReference(&output);
    context->Status = status;

    WslRefreshProvider();

    if (NT_SUCCESS(status))
        WslpFreeActionContext(context);
    else
        SystemInformer_Invoke(WslpShowActionError, context);

    return STATUS_SUCCESS;
}

/**
 * Starts an action in the background, a wsl.exe or wslc.exe command or a Docker API request.
 *
 * \param FileName The executable, from WslGetWslFileName or WslGetWslcFileName, or NULL with
 * PipeName.
 * \param PipeName The pipe of a Docker API engine, or NULL to run FileName.
 * \param Method The request method for PipeName, e.g. "POST".
 * \param Arguments The command line arguments, or the request path for PipeName, from
 * PhFormatString. This function takes ownership of the string; NULL does nothing.
 * \param Description The error text shown if the action fails.
 */
static VOID WslpStartAction(
    _In_opt_ PPH_STRING FileName,
    _In_opt_ PPH_STRING PipeName,
    _In_opt_ PCSTR Method,
    _In_opt_ PPH_STRING Arguments,
    _In_ PCWSTR Description
    )
{
    PWSL_ACTION_CONTEXT context;

    if (!Arguments)
        return;

    context = PhAllocateZero(sizeof(WSL_ACTION_CONTEXT));
    context->FileName = FileName;
    PhSetReference(&context->PipeName, PipeName);
    context->Method = Method;
    context->Arguments = Arguments;
    context->Description = PhCreateString(Description);

    if (!NT_SUCCESS(PhCreateThread2(WslpActionThread, context)))
        WslpFreeActionContext(context);
}

// A reclaim runs its command on a worker thread and then watches the VM process give the
// memory back to Windows, while a dialog on a thread of its own shows the progress. The dialog
// can be closed at any time; the worker then finishes without it.
typedef enum _WSL_RECLAIM_PHASE
{
    WslReclaimPhaseRunning, // The command is running
    WslReclaimPhaseWaiting, // Waiting for WSL to return the memory
    WslReclaimPhaseDone,
    WslReclaimPhaseFailed
} WSL_RECLAIM_PHASE;

typedef struct _WSL_RECLAIM_CONTEXT
{
    LONG RefCount; // One for the dialog thread, one for the worker thread
    PPH_STRING FileName; // wsl.exe or wslc.exe; a cached string the context does not own
    PPH_STRING Arguments;
    HANDLE ProcessId; // The VM process whose private bytes are shown, or NULL if unknown
    PCWSTR Name; // The VM for the texts, e.g. "WSL VM"
    SIZE_T Before; // Private bytes of the VM process before the reclaim
    volatile SIZE_T Current; // Private bytes of the VM process now; set by the worker
    NTSTATUS Status;
    PPH_STRING Message; // The command's own error text, if it printed one
    volatile LONG Phase; // WSL_RECLAIM_PHASE; set by the worker after the fields above
    BOOLEAN DialogFinished; // The dialog shows the result; used only by the dialog thread
    PPH_STRING ContentText; // The text the dialog shows; used only by the dialog thread
} WSL_RECLAIM_CONTEXT, *PWSL_RECLAIM_CONTEXT;

// A reclaim runs, so the menu item is disabled.
static LONG WslReclaimActive = 0;

/**
 * Releases a reference to a reclaim context, and frees it with the last one.
 */
static VOID WslpDereferenceReclaim(
    _In_ PWSL_RECLAIM_CONTEXT Context
    )
{
    if (_InterlockedDecrement(&Context->RefCount) != 0)
        return;

    PhClearReference(&Context->Arguments);
    PhClearReference(&Context->Message);
    PhClearReference(&Context->ContentText);
    PhFree(Context);
}

/**
 * Waits for WSL to return reclaimed memory to Windows, keeping Context->Current up to date.
 *
 * \remarks Measured on WSL 2.9.12: the private bytes stay the same for several seconds, most
 * of the memory is back after about 11 seconds, and a little more follows for another half
 * minute. So the wait only looks for the end once the fall has started, and gives up after 30
 * seconds.
 */
static VOID WslpWaitForReclaim(
    _Inout_ PWSL_RECLAIM_CONTEXT Context
    )
{
    SIZE_T history[4] = { 0 };
    ULONG fallingSince = ULONG_MAX;
    LARGE_INTEGER interval;

    PhTimeoutFromMilliseconds(&interval, 1000);

    for (ULONG seconds = 0; seconds < 30; seconds++)
    {
        SIZE_T privateBytes;

        NtDelayExecution(FALSE, &interval);

        if (!(privateBytes = WslpGetVmPrivateBytes(Context->ProcessId)))
            break;

        Context->Current = privateBytes;

        if (fallingSince == ULONG_MAX && privateBytes + 64 * 1024 * 1024 < Context->Before)
            fallingSince = seconds;

        // Done once it fell by less than 16 MB over the last 3 seconds of the fall.
        if (fallingSince != ULONG_MAX && seconds >= fallingSince + 3 &&
            history[(seconds + 1) % 4] - min(history[(seconds + 1) % 4], privateBytes) < 16 * 1024 * 1024)
        {
            break;
        }

        history[seconds % 4] = privateBytes;
    }
}

/**
 * Runs the reclaim command and waits for the memory to be returned.
 */
_Function_class_(USER_THREAD_START_ROUTINE)
static NTSTATUS NTAPI WslpReclaimWorkerThread(
    _In_ PVOID Parameter
    )
{
    PWSL_RECLAIM_CONTEXT context = Parameter;
    PPH_BYTES output = NULL;
    NTSTATUS status;

    status = WslRunCommandEx(context->FileName, &context->Arguments->sr, WSL_ACTION_TIMEOUT_MS, &output, TRUE);

    if (NT_SUCCESS(status))
    {
        if (context->ProcessId)
        {
            WriteRelease(&context->Phase, WslReclaimPhaseWaiting);
            WslpWaitForReclaim(context);
        }

        WriteRelease(&context->Phase, WslReclaimPhaseDone);
    }
    else
    {
        if (output)
            context->Message = WslpGetCommandErrorMessage(output);

        context->Status = status;
        WriteRelease(&context->Phase, WslReclaimPhaseFailed);
    }

    PhClearReference(&output);
    WslRefreshProvider();
    WriteRelease(&WslReclaimActive, FALSE);
    WslpDereferenceReclaim(context);

    return STATUS_SUCCESS;
}

/**
 * Shows the state of a reclaim in its dialog. Runs on the dialog thread.
 */
static VOID WslpUpdateReclaimDialog(
    _In_ HWND WindowHandle,
    _Inout_ PWSL_RECLAIM_CONTEXT Context
    )
{
    WSL_RECLAIM_PHASE phase = (WSL_RECLAIM_PHASE)ReadAcquire(&Context->Phase);
    SIZE_T current = Context->Current;
    PPH_STRING text = NULL;

    if (Context->DialogFinished)
        return;

    switch (phase)
    {
    case WslReclaimPhaseRunning:
        text = PhFormatString(L"Dropping the caches of the %s...", Context->Name);
        break;
    case WslReclaimPhaseWaiting:
    case WslReclaimPhaseDone:
        if (Context->ProcessId && current && current < Context->Before)
        {
            PPH_STRING returned = PhFormatSize(Context->Before - current, ULONG_MAX);
            PPH_STRING before = PhFormatSize(Context->Before, ULONG_MAX);
            PPH_STRING after = PhFormatSize(current, ULONG_MAX);

            text = PhFormatString(
                phase == WslReclaimPhaseDone ?
                    L"%s: %s returned to Windows, from %s to %s of private bytes. WSL can return a little more over the next seconds." :
                    L"%s: %s returned to Windows so far, from %s to %s of private bytes...",
                Context->Name,
                returned->Buffer,
                before->Buffer,
                after->Buffer
                );

            PhDereferenceObject(returned);
            PhDereferenceObject(before);
            PhDereferenceObject(after);
        }
        else if (phase == WslReclaimPhaseWaiting)
        {
            text = PhFormatString(L"The caches of the %s were dropped. Waiting for WSL to return the memory to Windows...", Context->Name);
        }
        else if (Context->ProcessId)
        {
            text = PhFormatString(L"%s: the caches were dropped, but no memory was returned to Windows within 30 seconds.", Context->Name);
        }
        else
        {
            text = PhFormatString(L"The caches of the %s were dropped.", Context->Name);
        }
        break;
    case WslReclaimPhaseFailed:
        text = Context->Message ? PhReferenceObject(Context->Message) : PhGetStatusMessage(Context->Status, 0);
        break;
    }

    if (phase == WslReclaimPhaseDone || phase == WslReclaimPhaseFailed)
    {
        Context->DialogFinished = TRUE;

        SendMessage(WindowHandle, TDM_SET_PROGRESS_BAR_MARQUEE, FALSE, 0);
        SendMessage(WindowHandle, TDM_SET_MARQUEE_PROGRESS_BAR, FALSE, 0);
        SendMessage(WindowHandle, TDM_SET_PROGRESS_BAR_POS, 100, 0);

        if (phase == WslReclaimPhaseFailed)
        {
            SendMessage(WindowHandle, TDM_SET_PROGRESS_BAR_STATE, PBST_ERROR, 0);
            SendMessage(WindowHandle, TDM_UPDATE_ICON, TDIE_ICON_MAIN, (LPARAM)TD_ERROR_ICON);
            SendMessage(WindowHandle, TDM_SET_ELEMENT_TEXT, TDE_MAIN_INSTRUCTION, (LPARAM)L"Unable to reclaim memory");
        }
        else
        {
            SendMessage(WindowHandle, TDM_SET_ELEMENT_TEXT, TDE_MAIN_INSTRUCTION, (LPARAM)L"Memory reclaimed");
        }
    }

    // Only a changed text is set, as setting it lays the dialog out again.
    if (text && !(Context->ContentText && PhEqualString(text, Context->ContentText, FALSE)))
    {
        PhMoveReference(&Context->ContentText, text);
        SendMessage(WindowHandle, TDM_SET_ELEMENT_TEXT, TDE_CONTENT, (LPARAM)Context->ContentText->Buffer);
    }
    else
    {
        PhClearReference(&text);
    }
}

/**
 * Task dialog callback of a reclaim.
 */
_Function_class_(PFTASKDIALOGCALLBACK)
static HRESULT CALLBACK WslpReclaimDialogCallback(
    _In_ HWND WindowHandle,
    _In_ UINT Notification,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam,
    _In_ LONG_PTR Context
    )
{
    switch (Notification)
    {
    case TDN_CREATED:
        SendMessage(WindowHandle, TDM_SET_PROGRESS_BAR_MARQUEE, TRUE, 30);
        WslpUpdateReclaimDialog(WindowHandle, (PWSL_RECLAIM_CONTEXT)Context);
        break;
    case TDN_TIMER:
        WslpUpdateReclaimDialog(WindowHandle, (PWSL_RECLAIM_CONTEXT)Context);
        break;
    }

    return S_OK;
}

/**
 * Shows the dialog of a reclaim. It has no owner, so the main window stays usable.
 */
_Function_class_(USER_THREAD_START_ROUTINE)
static NTSTATUS NTAPI WslpReclaimDialogThread(
    _In_ PVOID Parameter
    )
{
    PWSL_RECLAIM_CONTEXT context = Parameter;
    TASKDIALOGCONFIG config = { sizeof(TASKDIALOGCONFIG) };
    PH_AUTO_POOL autoPool;

    PhInitializeAutoPool(&autoPool);

    config.dwFlags = TDF_SHOW_MARQUEE_PROGRESS_BAR | TDF_CALLBACK_TIMER | TDF_ALLOW_DIALOG_CANCELLATION | TDF_CAN_BE_MINIMIZED;
    config.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    config.pszWindowTitle = L"System Informer";
    config.pszMainIcon = TD_INFORMATION_ICON;
    config.pszMainInstruction = L"Reclaiming memory";
    config.pszContent = L" ";
    config.pfCallback = WslpReclaimDialogCallback;
    config.lpCallbackData = (LONG_PTR)context;

    PhShowTaskDialog(&config, NULL, NULL, NULL);

    PhDeleteAutoPool(&autoPool);
    WslpDereferenceReclaim(context);

    return STATUS_SUCCESS;
}

/**
 * Starts reclaiming the memory of a VM, with a dialog that shows the progress.
 *
 * \param FileName wsl.exe or wslc.exe.
 * \param Arguments The command that runs WSL_RECLAIM_SCRIPT in the VM, from PhFormatString.
 * This function takes ownership of the string; NULL does nothing.
 * \param ProcessId The VM process, or NULL if it is not known, which leaves out the numbers.
 * \param Name The VM for the texts, e.g. "WSL VM".
 */
static VOID WslpStartReclaim(
    _In_ PPH_STRING FileName,
    _In_opt_ PPH_STRING Arguments,
    _In_opt_ HANDLE ProcessId,
    _In_ PCWSTR Name
    )
{
    PWSL_RECLAIM_CONTEXT context;
    NTSTATUS status;

    if (!Arguments)
        return;

    // One reclaim at a time; the menu item is disabled meanwhile.
    if (_InterlockedCompareExchange(&WslReclaimActive, TRUE, FALSE) != FALSE)
    {
        PhDereferenceObject(Arguments);
        return;
    }

    context = PhAllocateZero(sizeof(WSL_RECLAIM_CONTEXT));
    context->RefCount = 2;
    context->FileName = FileName;
    context->Arguments = Arguments;
    context->ProcessId = ProcessId;
    context->Name = Name;
    context->Phase = WslReclaimPhaseRunning;

    // Read before the command runs, so the dialog can compare against it.
    if (ProcessId)
        context->Before = context->Current = WslpGetVmPrivateBytes(ProcessId);

    if (!NT_SUCCESS(PhCreateThread2(WslpReclaimDialogThread, context)))
        WslpDereferenceReclaim(context);

    // Without the worker the dialog shows why, rather than waiting for it.
    if (!NT_SUCCESS(status = PhCreateThread2(WslpReclaimWorkerThread, context)))
    {
        context->Status = status;
        WriteRelease(&context->Phase, WslReclaimPhaseFailed);
        WriteRelease(&WslReclaimActive, FALSE);
        WslpDereferenceReclaim(context);
    }
}

/**
 * Gets a running WSL 2 distribution to reclaim the WSL VM's memory in. All of them share the
 * VM's kernel, so any one frees the cache of all.
 *
 * \return The default distribution if it runs, else another running one, or NULL.
 */
static PWSL_DISTRO_ITEM WslpGetReclaimDistro(
    VOID
    )
{
    PWSL_DISTRO_ITEM found = NULL;

    for (ULONG i = 0; i < WslDistroNodes->Count; i++)
    {
        PWSL_NODE node = WslDistroNodes->Items[i];
        PWSL_DISTRO_ITEM distro = node->Distro;

        if (node->RemoveTime || distro->Version != 2 || distro->State != WslDistroStateRunning || !WslIsSafeDistroName(distro->Name))
            continue;

        if (distro->Default)
            return distro;

        if (!found)
            found = distro;
    }

    return found;
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
                    NULL,
                    NULL,
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
                WslpStartAction(WslGetWslFileName(), NULL, NULL, PhCreateString(L"--shutdown"), L"Unable to shut down WSL.");
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

            // The session VM for a session and its containers; the WSL VM for everything else,
            // including the containers of an engine, which run in a distribution.
            if (node->Type == WslNodeTypeSession || (node->Type == WslNodeTypeContainer && !node->Engine))
                WslpGoToVmProcess(WslSessionVmProcessItem);
            else
                WslpGoToVmProcess(WslVmProcessItem);
        }
        break;
    case ID_WSL_RECLAIMMEMORY:
        {
            PWSL_DISTRO_ITEM distro;

            if (!node)
                break;

            // Only a VM that already runs, as a command in a stopped one would start it.
            if (node->Type == WslNodeTypeVm && (distro = WslpGetReclaimDistro()))
            {
                WslpStartReclaim(
                    WslGetWslFileName(),
                    PhFormatString(L"--distribution %s --user root --cd / --exec /bin/sh -c \"%s\"", distro->Name->Buffer, WSL_RECLAIM_SCRIPT),
                    WslVmProcessItem ? WslVmProcessItem->ProcessId : NULL,
                    L"WSL VM"
                    );
            }
            else if (node->Type == WslNodeTypeSession && node->Session->State == WslDistroStateRunning &&
                WslIsSafeSessionName(node->Session->Name) && WslGetWslcFileName())
            {
                // "session run" runs as root in the session VM.
                WslpStartReclaim(
                    WslGetWslcFileName(),
                    PhFormatString(L"--session \"%s\" system session run /bin/sh -c \"%s\"", node->Session->Name->Buffer, WSL_RECLAIM_SCRIPT),
                    WslSessionVmProcessItem ? WslSessionVmProcessItem->ProcessId : NULL,
                    L"WSLC session VM"
                    );
            }
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
    case ID_WSL_CONTAINERINSPECT:
        {
            NTSTATUS status;

            if (!node || !node->Container)
                break;

            if (node->Engine)
                status = WslShowEngineContainerInspect(node->Engine->PipeName, node->Container->Id, node->Container->Name);
            else
                status = WslShowContainerInspect(node->Session->Name, node->Container->Id, node->Container->Name);

            if (!NT_SUCCESS(status))
                PhShowStatus(WindowHandle, L"Unable to inspect the container.", status, 0);
        }
        break;
    case ID_WSL_CONTAINERSTOP:
    case ID_WSL_CONTAINERRESTART:
    case ID_WSL_CONTAINERKILL:
    case ID_WSL_CONTAINERREMOVE:
        {
            PCWSTR verb;
            PPH_STRING pipeName = NULL;
            PPH_STRING sessionName = NULL;
            PPH_STRING containerId;
            PPH_STRING containerName;
            BOOLEAN confirmed = TRUE;

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

            if (!node || !node->Container || (!node->Engine && !WslGetWslcFileName()))
                break;

            if ((node->Engine ? !WslIsSafePipeName(node->Engine->PipeName) : !WslIsSafeSessionName(node->Session->Name)) ||
                !WslIsSafeContainerId(node->Container->Id))
            {
                PhShowStatus(WindowHandle, L"Unable to control the container.", STATUS_INVALID_PARAMETER, 0);
                break;
            }

            // The confirmation runs a message loop, in which a new snapshot can replace the node
            // and the engine or session it points to, so what the action needs is kept first.
            if (node->Engine)
                PhSetReference(&pipeName, node->Engine->PipeName);
            else
                PhSetReference(&sessionName, node->Session->Name);

            containerId = PhReferenceObject(node->Container->Id);
            containerName = PhReferenceObject(node->Container->Name);
            node = NULL;

            // Killing skips the container's shutdown and removing deletes its filesystem, so both
            // ask first; stop and restart do not.
            if (Id == ID_WSL_CONTAINERKILL)
            {
                confirmed = PhShowConfirmMessage(
                    WindowHandle,
                    L"kill",
                    containerName->Buffer,
                    L"The container's processes will be killed without a chance to shut down.",
                    TRUE
                    );
            }
            else if (Id == ID_WSL_CONTAINERREMOVE)
            {
                confirmed = PhShowConfirmMessage(
                    WindowHandle,
                    L"remove",
                    containerName->Buffer,
                    L"The container and any changes made to its filesystem will be deleted.",
                    TRUE
                    );
            }

            // The Docker API has the same actions: POST /containers/<id>/<verb>, and DELETE to remove.
            if (confirmed && pipeName)
            {
                WslpStartAction(
                    NULL,
                    pipeName,
                    Id == ID_WSL_CONTAINERREMOVE ? "DELETE" : "POST",
                    Id == ID_WSL_CONTAINERREMOVE ?
                        PhFormatString(L"/containers/%s", containerId->Buffer) :
                        PhFormatString(L"/containers/%s/%s", containerId->Buffer, verb),
                    L"Unable to control the container."
                    );
            }
            else if (confirmed)
            {
                WslpStartAction(
                    WslGetWslcFileName(),
                    NULL,
                    NULL,
                    PhFormatString(L"--session \"%s\" %s %s", sessionName->Buffer, verb, containerId->Buffer),
                    L"Unable to control the container."
                    );
            }

            PhClearReference(&pipeName);
            PhClearReference(&sessionName);
            PhDereferenceObject(containerId);
            PhDereferenceObject(containerName);
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

    // A removed row is only shown for its highlight; what it names is gone.
    if (!node || node->RemoveTime)
        return;

    menu = PhCreateEMenu();

    if (node->Type == WslNodeTypeVm)
    {
        PhInsertEMenuItem(menu, PhCreateEMenuItem(0, ID_WSL_GOTOPROCESS, L"&Go to process", NULL, NULL), ULONG_MAX);
        PhInsertEMenuItem(menu, PhCreateEMenuSeparator(), ULONG_MAX);
        PhInsertEMenuItem(menu, PhCreateEMenuItem(0, ID_WSL_RECLAIMMEMORY, L"Reclaim &memory", NULL, NULL), ULONG_MAX);
        PhInsertEMenuItem(menu, PhCreateEMenuItem(0, ID_WSL_SHUTDOWN, L"&Shut down WSL", NULL, NULL), ULONG_MAX);
        PhSetFlagsEMenuItem(menu, ID_WSL_GOTOPROCESS, PH_EMENU_DEFAULT, PH_EMENU_DEFAULT);

        if (!WslVmProcessItem)
            PhEnableEMenuItem(menu, ID_WSL_GOTOPROCESS, FALSE);

        // Reclaiming runs in a distribution that is already running, so that it never starts
        // one, and one reclaim at a time.
        if (!WslpGetReclaimDistro() || ReadAcquire(&WslReclaimActive))
            PhEnableEMenuItem(menu, ID_WSL_RECLAIMMEMORY, FALSE);
    }
    else if (node->Type == WslNodeTypeContainer)
    {
        // The same items for WSLC containers and for the containers of a Docker API engine.
        PhInsertEMenuItem(menu, PhCreateEMenuItem(0, ID_WSL_CONTAINERINSPECT, L"&Inspect...", NULL, NULL), ULONG_MAX);
        PhInsertEMenuItem(menu, PhCreateEMenuSeparator(), ULONG_MAX);
        PhInsertEMenuItem(menu, PhCreateEMenuItem(0, ID_WSL_CONTAINERSTOP, L"S&top", NULL, NULL), ULONG_MAX);
        PhInsertEMenuItem(menu, PhCreateEMenuItem(0, ID_WSL_CONTAINERRESTART, L"&Restart", NULL, NULL), ULONG_MAX);
        PhInsertEMenuItem(menu, PhCreateEMenuItem(0, ID_WSL_CONTAINERKILL, L"&Kill", NULL, NULL), ULONG_MAX);

        // A running container cannot be removed, so Remove is only offered once it stopped.
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
            PhEnableEMenuItem(menu, ID_WSL_CONTAINERSTOP, FALSE);
            PhEnableEMenuItem(menu, ID_WSL_CONTAINERKILL, FALSE);
            PhEnableEMenuItem(menu, ID_WSL_CONTAINEROPENPORT, FALSE);
        }

        // An engine's containers run in the WSL VM, a session's in the session VM.
        if (!(node->Engine ? WslVmProcessItem : WslSessionVmProcessItem))
            PhEnableEMenuItem(menu, ID_WSL_GOTOPROCESS, FALSE);
    }
    else if (node->Type == WslNodeTypeSession)
    {
        PhInsertEMenuItem(menu, PhCreateEMenuItem(0, ID_WSL_GOTOPROCESS, L"&Go to VM process", NULL, NULL), ULONG_MAX);
        PhInsertEMenuItem(menu, PhCreateEMenuItem(0, ID_WSL_RECLAIMMEMORY, L"Reclaim &memory", NULL, NULL), ULONG_MAX);

        if (!WslSessionVmProcessItem)
            PhEnableEMenuItem(menu, ID_WSL_GOTOPROCESS, FALSE);

        // Any "--session" command starts a stopped session VM.
        if (node->Session->State != WslDistroStateRunning || !WslIsSafeSessionName(node->Session->Name) || ReadAcquire(&WslReclaimActive))
            PhEnableEMenuItem(menu, ID_WSL_RECLAIMMEMORY, FALSE);
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
 * Gets an icon's index in the process image list, and adds the icon the first time.
 *
 * \param FileName An executable or DLL, or an .ico file.
 * \param IconIndex As for PhExtractIconEx: an index, or a negative resource id. Not used for
 * an .ico file.
 * \return The index, or 0, the generic icon, if the file has no such icon.
 */
static ULONG WslpGetIconImageIndex(
    _In_ PPH_STRING FileName,
    _In_ LONG IconIndex
    )
{
    static CONST PH_STRINGREF icoExtension = PH_STRINGREF_INIT(L".ico");
    PWSL_ICON entry;
    HICON icon = NULL;
    LONG dpi;
    LONG width;
    LONG height;

    if (!WslIcons)
        WslIcons = PhCreateList(8);

    for (ULONG i = 0; i < WslIcons->Count; i++)
    {
        entry = WslIcons->Items[i];

        if (entry->IconIndex == IconIndex && PhEqualString(entry->FileName, FileName, TRUE))
            return entry->ImageIndex;
    }

    dpi = PhGetWindowDpi(WslTreeNewHandle);
    width = PhGetSystemMetrics(SM_CXSMICON, dpi);
    height = PhGetSystemMetrics(SM_CYSMICON, dpi);

    // PhImageListExtractIcon reads only executables, and always the first icon.
    if (PhEndsWithStringRef(&FileName->sr, &icoExtension, TRUE))
        icon = LoadImage(NULL, FileName->Buffer, IMAGE_ICON, width, height, LR_LOADFROMFILE);
    else
        PhExtractIconEx(&FileName->sr, FALSE, IconIndex, width, height, width, height, NULL, &icon);

    // A failure is kept too, so that the file is not read again on every refresh.
    entry = PhAllocate(sizeof(WSL_ICON));
    entry->FileName = PhReferenceObject(FileName);
    entry->IconIndex = IconIndex;
    entry->ImageIndex = 0;

    if (icon)
    {
        entry->ImageIndex = PhImageListAddIcon(PhGetProcessSmallImageList(), icon);
        DestroyIcon(icon);
    }

    PhAddItemList(WslIcons, entry);

    return entry->ImageIndex;
}

/**
 * Forgets the icons added to the process image list, after System Informer emptied it for a
 * new DPI.
 */
static VOID WslpClearIcons(
    VOID
    )
{
    if (!WslIcons)
        return;

    for (ULONG i = 0; i < WslIcons->Count; i++)
    {
        PWSL_ICON entry = WslIcons->Items[i];

        PhDereferenceObject(entry->FileName);
        PhFree(entry);
    }

    PhClearList(WslIcons);
}

/**
 * Clears the cached icons of nodes and their children, so they are looked up again.
 */
static VOID WslpInvalidateNodeIcons(
    _In_ PPH_LIST Nodes
    )
{
    for (ULONG i = 0; i < Nodes->Count; i++)
    {
        PWSL_NODE node = Nodes->Items[i];

        PhInvalidateTreeNewNode(&node->Node, TN_CACHE_ICON);

        if (node->Children)
            WslpInvalidateNodeIcons(node->Children);
    }
}

/**
 * Gets the icon of the process that serves an engine's pipe, e.g. Docker Desktop.
 *
 * \return The index in the process image list, or 0 if the process or its icon is not known yet.
 */
static ULONG WslpGetEngineIconIndex(
    _In_ PWSL_ENGINE Engine
    )
{
    PPH_PROCESS_ITEM processItem;
    ULONG index = 0;

    if (Engine->ServerProcessId && (processItem = PhReferenceProcessItem(Engine->ServerProcessId)))
    {
        if (processItem->IconEntry)
            index = processItem->IconEntry->SmallIconIndex;

        PhDereferenceObject(processItem);
    }

    return index;
}

/**
 * Gets the icon of a node: the WSL icon for the VM and sessions, the distribution's own icon,
 * the icon of the program that serves an engine, and a cube for containers.
 *
 * \return The index in the process image list; 0, the generic icon, for Linux processes.
 */
static ULONG WslpGetNodeIconIndex(
    _In_ PWSL_NODE Node
    )
{
    static PH_INITONCE initOnce = PH_INITONCE_INIT;
    static PPH_STRING wslFileName = NULL;
    static PPH_STRING imageresFileName = NULL;
    // The "3D Objects" icon, a cube; a resource id, as icon indexes differ between Windows versions.
    static CONST LONG containerIconId = -198;
    ULONG index = 0;

    if (PhBeginInitOnce(&initOnce))
    {
        static CONST PH_STRINGREF wslPath = PH_STRINGREF_INIT(L"%ProgramW6432%\\WSL\\wsl.exe");
        static CONST PH_STRINGREF imageresPath = PH_STRINGREF_INIT(L"\\System32\\imageres.dll");
        PH_STRINGREF systemRoot;

        // The wsl.exe of the WSL package has the WSL icon; the one in System32 has Tux.
        if ((wslFileName = PhExpandEnvironmentStrings(&wslPath)) && !PhDoesFileExistWin32(wslFileName->Buffer))
            PhClearReference(&wslFileName);

        if (!wslFileName)
            wslFileName = PhReferenceObject(WslGetWslFileName());

        PhGetSystemRoot(&systemRoot);
        imageresFileName = PhConcatStringRef2(&systemRoot, &imageresPath);

        PhEndInitOnce(&initOnce);
    }

    switch (Node->Type)
    {
    case WslNodeTypeVm:
        index = WslpGetIconImageIndex(wslFileName, 0);
        break;
    case WslNodeTypeSession:
        index = WslpGetIconImageIndex(WslGetWslcFileName() ? WslGetWslcFileName() : wslFileName, 0);
        break;
    case WslNodeTypeDistro:
        {
            static CONST PH_STRINGREF iconName = PH_STRINGREF_INIT(L"\\shortcut.ico");

            if (Node->Engine)
                index = WslpGetEngineIconIndex(Node->Engine);

            // Distributions from the Store and from wsl --install put their icon here.
            if (!index && Node->Distro->BasePath)
            {
                PPH_STRING iconFileName = PhConcatStringRef2(&Node->Distro->BasePath->sr, &iconName);

                index = WslpGetIconImageIndex(iconFileName, 0);
                PhDereferenceObject(iconFileName);
            }

            if (!index)
                index = WslpGetIconImageIndex(wslFileName, 0);
        }
        break;
    case WslNodeTypeContainer:
        index = WslpGetIconImageIndex(imageresFileName, containerIconId);
        break;
    }

    return index;
}

/**
 * Tells the provider whether a column that needs the containers' inspect output is shown, and
 * asks for a refresh when one was just added, so that it fills without waiting a refresh.
 */
static VOID WslpUpdateDetailsWanted(
    VOID
    )
{
    static CONST ULONG columns[] = { WSLTNC_PLATFORM, WSLTNC_RESTART, WSLTNC_EXITCODE, WSLTNC_MEMORYLIMIT, WSLTNC_CPULIMIT, WSLTNC_PRIVILEGED, WSLTNC_USER };
    PH_TREENEW_COLUMN column;
    LONG wanted = FALSE;

    for (ULONG i = 0; i < RTL_NUMBER_OF(columns) && !wanted; i++)
    {
        if (TreeNew_GetColumn(WslTreeNewHandle, columns[i], &column) && column.Visible)
            wanted = TRUE;
    }

    if (InterlockedExchange(&WslContainerDetailsWanted, wanted) != wanted && wanted)
        WslRefreshProvider();
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

            WslpGetNodeCellText(node, getCellText);
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
            else if (node->Type == WslNodeTypeSession)
                stopped = node->Session->State != WslDistroStateRunning;
            else if (node->Type == WslNodeTypeContainer)
                stopped = !node->Container->Running;
            else
                stopped = FALSE;

            getNodeColor->Flags = TN_CACHE;

            // A highlight ends on the first snapshot after the duration, which clears this cache.
            if (node->RemoveTime)
            {
                getNodeColor->BackColor = WslColorRemoved;
                getNodeColor->Flags |= TN_AUTO_FORECOLOR;
            }
            else if (node->CreateTime && WslUpdateTime - node->CreateTime < WslHighlightingDuration)
            {
                getNodeColor->BackColor = WslColorNew;
                getNodeColor->Flags |= TN_AUTO_FORECOLOR;
            }
            else if (stopped)
            {
                getNodeColor->ForeColor = GetSysColor(COLOR_GRAYTEXT);
            }
        }
        return TRUE;
    case TreeNewGetNodeIcon:
        {
            PPH_TREENEW_GET_NODE_ICON getNodeIcon = Parameter1;

            // The tree draws an index into the process image list.
            getNodeIcon->Icon = (HICON)(ULONG_PTR)WslpGetNodeIconIndex((PWSL_NODE)getNodeIcon->Node);
            getNodeIcon->Flags = TN_CACHE;
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
                    node->TooltipText = WslpGetContainerTooltip(container);

                getCellTooltip->Text = PhGetStringRef(node->TooltipText);
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

            // The menu can show or hide columns, also through "Choose columns...".
            WslpUpdateDetailsWanted();
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
    TreeNew_SetImageList(WindowHandle, PhGetProcessSmallImageList());

    PhAddTreeNewColumn(WindowHandle, WSLTNC_NAME, TRUE, L"Name", 200, PH_ALIGN_LEFT, 0, 0);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_PID, TRUE, L"PID", 50, PH_ALIGN_RIGHT, 1, DT_RIGHT);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_LINUXPID, TRUE, L"PID (Linux)", 75, PH_ALIGN_RIGHT, 2, DT_RIGHT);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_TYPE, TRUE, L"Type", 90, PH_ALIGN_LEFT, 3, 0);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_STATE, TRUE, L"State", 90, PH_ALIGN_LEFT, 4, 0);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_CPU, TRUE, L"CPU", 45, PH_ALIGN_RIGHT, 5, DT_RIGHT);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_MEMORY, TRUE, L"Memory", 80, PH_ALIGN_RIGHT, 6, DT_RIGHT);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_IMAGE, TRUE, L"Image / OS", 220, PH_ALIGN_LEFT, 7, 0);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_PORTS, TRUE, L"Ports", 130, PH_ALIGN_LEFT, 8, 0);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_DISK, TRUE, L"Disk", 70, PH_ALIGN_RIGHT, 9, DT_RIGHT);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_STATUS, TRUE, L"Uptime / Status", 120, PH_ALIGN_LEFT, 10, 0);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_VERSION, FALSE, L"Version", 50, PH_ALIGN_RIGHT, ULONG_MAX, DT_RIGHT);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_LOCATION, FALSE, L"Location", 300, PH_ALIGN_LEFT, ULONG_MAX, DT_PATH_ELLIPSIS);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_CONTAINERID, FALSE, L"Container ID", 100, PH_ALIGN_LEFT, ULONG_MAX, 0);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_COMMAND, FALSE, L"Command", 200, PH_ALIGN_LEFT, ULONG_MAX, 0);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_CREATED, FALSE, L"Created", 140, PH_ALIGN_LEFT, ULONG_MAX, 0);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_IPADDRESS, FALSE, L"IP address", 110, PH_ALIGN_LEFT, ULONG_MAX, 0);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_NETWORK, FALSE, L"Network", 90, PH_ALIGN_LEFT, ULONG_MAX, 0);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_MOUNTS, FALSE, L"Mounts", 160, PH_ALIGN_LEFT, ULONG_MAX, 0);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_COMPOSE, FALSE, L"Compose project", 120, PH_ALIGN_LEFT, ULONG_MAX, 0);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_HEALTH, FALSE, L"Health", 70, PH_ALIGN_LEFT, ULONG_MAX, 0);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_PLATFORM, FALSE, L"Platform", 90, PH_ALIGN_LEFT, ULONG_MAX, 0);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_RESTART, FALSE, L"Restart policy", 100, PH_ALIGN_LEFT, ULONG_MAX, 0);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_EXITCODE, FALSE, L"Exit code", 110, PH_ALIGN_LEFT, ULONG_MAX, 0);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_MEMORYLIMIT, FALSE, L"Memory limit", 80, PH_ALIGN_RIGHT, ULONG_MAX, DT_RIGHT);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_CPULIMIT, FALSE, L"CPU limit", 60, PH_ALIGN_RIGHT, ULONG_MAX, DT_RIGHT);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_PRIVILEGED, FALSE, L"Privileged", 70, PH_ALIGN_LEFT, ULONG_MAX, 0);
    PhAddTreeNewColumn(WindowHandle, WSLTNC_USER, FALSE, L"User", 80, PH_ALIGN_LEFT, ULONG_MAX, 0);

    TreeNew_SetTriState(WindowHandle, TRUE);
    TreeNew_SetSort(WindowHandle, WSLTNC_NAME, AscendingSortOrder);

    settings = PhGetStringSetting(SETTING_NAME_TREE_LIST_COLUMNS);
    PhCmLoadSettings(WindowHandle, &settings->sr);
    PhDereferenceObject(settings);

    sortSettings = PhGetIntegerPairSetting(SETTING_NAME_TREE_LIST_SORT);
    TreeNew_SetSort(WindowHandle, (ULONG)sortSettings.X, (PH_SORT_ORDER)sortSettings.Y);
    WslpUpdateDetailsWanted();

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
_Function_class_(PH_MAIN_TAB_PAGE_CALLBACK)
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

            WslSetProviderEnabled(WSL_PROVIDER_TAB, WslTabSelected && WslUpdateAutomatically);
        }
        break;
    case MainTabPageUpdateAutomaticallyChanged:
        {
            // Like the Network tab: the provider runs only while the tab is shown and updating.
            WslUpdateAutomatically = !!PtrToUlong(Parameter1);
            WslSetProviderEnabled(WSL_PROVIDER_TAB, WslTabSelected && WslUpdateAutomatically);
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
    case MainTabPageDpiChanged:
        {
            // System Informer has just emptied the process image list and added its own icons again.
            WslpClearIcons();

            if (WslTreeNewHandle)
            {
                WslpInvalidateNodeIcons(WslRootNodes);
                InvalidateRect(WslTreeNewHandle, NULL, FALSE);
            }
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

    // ToolStatus enables its search box only for tabs that register with it.
    if (WslPage && (WslToolStatusInterface = PhGetPluginInterfaceZ(TOOLSTATUS_INTERFACE_NAME, TOOLSTATUS_INTERFACE_VERSION)))
    {
        PTOOLSTATUS_TAB_INFO tabInfo;

        tabInfo = WslToolStatusInterface->RegisterTabInfo(WslPage->Index, &WslSearchBannerText);
        tabInfo->ActivateContent = WslpToolStatusActivateContent;
        tabInfo->GetTreeNewHandle = WslpToolStatusGetTreeNewHandle;

        PhRegisterCallback(WslToolStatusInterface->SearchChangedEvent, WslpSearchChangedHandler, NULL, &WslSearchChangedRegistration);
    }
}
