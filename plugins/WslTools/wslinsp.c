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
#include <json.h>

// The Inspect window shows the JSON that "wslc inspect" returns for a container as a tree
// of names and values. The window is modeless, so several containers can be compared.

// JSON_C_TO_STRING_NOSLASHESCAPE: phlib honours it, but json.h does not define it.
#define WSL_JSON_NO_SLASH_ESCAPE 0x0010

typedef enum _WSL_INSPECT_COLUMN
{
    WSLICNC_NAME,
    WSLICNC_VALUE,
    WSLICNC_MAXIMUM
} WSL_INSPECT_COLUMN;

typedef struct _WSL_INSPECT_NODE
{
    PH_TREENEW_NODE Node;
    PPH_STRING Name;
    PPH_STRING Value;
    PPH_LIST Children; // PWSL_INSPECT_NODE, owned
    PH_STRINGREF TextCache[WSLICNC_MAXIMUM];
} WSL_INSPECT_NODE, *PWSL_INSPECT_NODE;

typedef struct _WSL_INSPECT_CONTEXT
{
    PPH_STRING SessionName; // The WSLC session, or NULL for an engine container
    PPH_STRING PipeName; // The Docker API engine, or NULL for a WSLC container
    PPH_STRING ContainerId;
    PPH_STRING ContainerName;
    NTSTATUS Status;
    PPH_BYTES Output; // The JSON as wslc printed it or the engine returned it

    // Used by the window.
    HWND WindowHandle;
    HWND TreeNewHandle;
    PH_LAYOUT_MANAGER LayoutManager;
    PPH_LIST RootNodes; // PWSL_INSPECT_NODE
} WSL_INSPECT_CONTEXT, *PWSL_INSPECT_CONTEXT;

/**
 * Frees an inspect node and its children.
 */
static VOID WslpDestroyInspectNode(
    _In_ PWSL_INSPECT_NODE Node
    )
{
    for (ULONG i = 0; i < Node->Children->Count; i++)
        WslpDestroyInspectNode(Node->Children->Items[i]);

    PhDereferenceObject(Node->Children);
    PhClearReference(&Node->Name);
    PhClearReference(&Node->Value);
    PhFree(Node);
}

/**
 * Frees an inspect context.
 */
static VOID WslpFreeInspectContext(
    _In_ PWSL_INSPECT_CONTEXT Context
    )
{
    if (Context->RootNodes)
    {
        for (ULONG i = 0; i < Context->RootNodes->Count; i++)
            WslpDestroyInspectNode(Context->RootNodes->Items[i]);

        PhDereferenceObject(Context->RootNodes);
    }

    PhClearReference(&Context->SessionName);
    PhClearReference(&Context->PipeName);
    PhClearReference(&Context->ContainerId);
    PhClearReference(&Context->ContainerName);
    PhClearReference(&Context->Output);
    PhFree(Context);
}

/**
 * Gets the text shown for a JSON value: a string without its quotes, a number, true, false
 * or null, or the number of members of an object or array.
 */
static PPH_STRING WslpGetJsonValueText(
    _In_ PVOID Object
    )
{
    PH_BYTES_BUILDER builder;
    PPH_BYTES bytes;
    PPH_STRING text;
    ULONG type = PhGetJsonObjectType(Object);

    if (type == PH_JSON_OBJECT_TYPE_OBJECT)
    {
        PPH_LIST members = PhGetJsonObjectAsArrayList(Object);
        ULONG count = members ? members->Count : 0;

        if (members)
        {
            for (ULONG i = 0; i < members->Count; i++)
                PhFree(members->Items[i]);

            PhDereferenceObject(members);
        }

        return count == 0 ? PhCreateString(L"{}") : PhFormatString(L"{%lu}", count);
    }

    if (type == PH_JSON_OBJECT_TYPE_ARRAY)
    {
        ULONG count = PhGetJsonArrayLength(Object);

        return count == 0 ? PhCreateString(L"[]") : PhFormatString(L"[%lu]", count);
    }

    PhInitializeBytesBuilder(&builder, 64);
    PhJsonObjectToString(&builder, Object, 0, WSL_JSON_NO_SLASH_ESCAPE);
    bytes = PhFinalBytesBuilderBytes(&builder);

    // Drop the quotes around a string; escapes inside it stay, which only matters for
    // quotes, backslashes and control characters.
    if (type == PH_JSON_OBJECT_TYPE_STRING && bytes->Length >= 2)
        text = PhConvertUtf8ToUtf16Ex(bytes->Buffer + 1, bytes->Length - 2);
    else
        text = PhConvertUtf8ToUtf16Ex(bytes->Buffer, bytes->Length);

    PhDereferenceObject(bytes);

    return text;
}

/**
 * Creates the node of one JSON value, with nodes for its members or elements.
 *
 * \param Name The member name or "[index]". The node takes ownership; NULL gives an empty name.
 * \param Object The JSON value.
 */
static PWSL_INSPECT_NODE WslpCreateInspectNode(
    _In_opt_ PPH_STRING Name,
    _In_ PVOID Object
    )
{
    PWSL_INSPECT_NODE node;
    ULONG type = PhGetJsonObjectType(Object);

    node = PhAllocateZero(sizeof(WSL_INSPECT_NODE));
    PhInitializeTreeNewNode(&node->Node);
    node->Node.TextCache = node->TextCache;
    node->Node.TextCacheSize = WSLICNC_MAXIMUM;
    node->Name = Name ? Name : PhReferenceEmptyString();
    node->Value = WslpGetJsonValueText(Object);
    node->Children = PhCreateList(1);

    if (type == PH_JSON_OBJECT_TYPE_OBJECT)
    {
        PPH_LIST members;

        if (members = PhGetJsonObjectAsArrayList(Object))
        {
            for (ULONG i = 0; i < members->Count; i++)
            {
                PJSON_ARRAY_LIST_OBJECT member = members->Items[i];

                PhAddItemList(node->Children, WslpCreateInspectNode(PhConvertUtf8ToUtf16(member->Key), member->Entry));
                PhFree(member);
            }

            PhDereferenceObject(members);
        }
    }
    else if (type == PH_JSON_OBJECT_TYPE_ARRAY)
    {
        ULONG count = PhGetJsonArrayLength(Object);

        for (ULONG i = 0; i < count; i++)
            PhAddItemList(node->Children, WslpCreateInspectNode(PhFormatString(L"[%lu]", i), PhGetJsonArrayIndexObject(Object, i)));
    }

    return node;
}

/**
 * Builds the root nodes from the inspect output.
 *
 * \remarks wslc prints an array with one object per inspected ID, like docker inspect. For
 * one container the array is left out, so its members are the root nodes.
 */
static PPH_LIST WslpCreateInspectRootNodes(
    _In_ PPH_BYTES Output
    )
{
    PPH_LIST rootNodes = PhCreateList(8);
    PVOID document;
    PVOID root;

    if (!NT_SUCCESS(PhCreateJsonParserEx(&document, Output, FALSE)) || !document)
        return rootNodes;

    root = document;

    if (PhGetJsonObjectType(root) == PH_JSON_OBJECT_TYPE_ARRAY && PhGetJsonArrayLength(root) == 1)
        root = PhGetJsonArrayIndexObject(root, 0);

    if (root && PhGetJsonObjectType(root) == PH_JSON_OBJECT_TYPE_OBJECT)
    {
        PWSL_INSPECT_NODE node = WslpCreateInspectNode(PhCreateString(L""), root);

        // The members of the root object are the root nodes; the wrapper node is not shown.
        for (ULONG i = 0; i < node->Children->Count; i++)
            PhAddItemList(rootNodes, node->Children->Items[i]);

        PhClearList(node->Children);
        WslpDestroyInspectNode(node);
    }
    else if (root)
    {
        PhAddItemList(rootNodes, WslpCreateInspectNode(PhCreateString(L"(value)"), root));
    }

    PhFreeJsonObject(document);

    return rootNodes;
}

static BOOLEAN NTAPI WslpInspectTreeNewCallback(
    _In_ HWND WindowHandle,
    _In_ PH_TREENEW_MESSAGE Message,
    _In_ PVOID Parameter1,
    _In_ PVOID Parameter2,
    _In_ PVOID Context
    )
{
    PWSL_INSPECT_CONTEXT context = Context;

    switch (Message)
    {
    case TreeNewGetChildren:
        {
            PPH_TREENEW_GET_CHILDREN getChildren = Parameter1;
            PWSL_INSPECT_NODE node = (PWSL_INSPECT_NODE)getChildren->Node;
            PPH_LIST children = node ? node->Children : context->RootNodes;

            getChildren->Children = (PPH_TREENEW_NODE *)children->Items;
            getChildren->NumberOfChildren = children->Count;
        }
        return TRUE;
    case TreeNewIsLeaf:
        {
            PPH_TREENEW_IS_LEAF isLeaf = Parameter1;

            isLeaf->IsLeaf = ((PWSL_INSPECT_NODE)isLeaf->Node)->Children->Count == 0;
        }
        return TRUE;
    case TreeNewGetCellText:
        {
            PPH_TREENEW_GET_CELL_TEXT getCellText = Parameter1;
            PWSL_INSPECT_NODE node = (PWSL_INSPECT_NODE)getCellText->Node;

            getCellText->Text = PhGetStringRef(getCellText->Id == WSLICNC_NAME ? node->Name : node->Value);
            getCellText->Flags = TN_CACHE;
        }
        return TRUE;
    case TreeNewKeyDown:
        {
            PPH_TREENEW_KEY_EVENT keyEvent = Parameter1;

            if (keyEvent && keyEvent->VirtualKey == 'C' && GetKeyState(VK_CONTROL) < 0)
            {
                PPH_STRING text = PhGetTreeNewText(WindowHandle, 0);

                PhSetClipboardString(WindowHandle, &text->sr);
                PhDereferenceObject(text);
            }
        }
        return TRUE;
    }

    return FALSE;
}

static INT_PTR CALLBACK WslpInspectDialogProc(
    _In_ HWND WindowHandle,
    _In_ UINT WindowMessage,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
    )
{
    PWSL_INSPECT_CONTEXT context;

    if (WindowMessage == WM_INITDIALOG)
    {
        context = (PWSL_INSPECT_CONTEXT)lParam;
        PhSetWindowContext(WindowHandle, PH_WINDOW_CONTEXT_DEFAULT, context);
    }
    else
    {
        context = PhGetWindowContext(WindowHandle, PH_WINDOW_CONTEXT_DEFAULT);
    }

    if (!context)
        return FALSE;

    switch (WindowMessage)
    {
    case WM_INITDIALOG:
        {
            context->WindowHandle = WindowHandle;
            context->TreeNewHandle = GetDlgItem(WindowHandle, IDC_INSPECT_TREE);
            context->RootNodes = WslpCreateInspectRootNodes(context->Output);

            PhSetApplicationWindowIcon(WindowHandle);
            PhSetWindowText(WindowHandle, PhaFormatString(L"Inspect: %s", PhGetString(context->ContainerName))->Buffer);

            PhSetControlTheme(context->TreeNewHandle, !PhGetIntegerSetting(SETTING_ENABLE_THEME_SUPPORT) ? L"explorer" : L"DarkMode_Explorer");
            TreeNew_SetCallback(context->TreeNewHandle, WslpInspectTreeNewCallback, context);
            PhAddTreeNewColumn(context->TreeNewHandle, WSLICNC_NAME, TRUE, L"Name", 200, PH_ALIGN_LEFT, 0, 0);
            PhAddTreeNewColumn(context->TreeNewHandle, WSLICNC_VALUE, TRUE, L"Value", 400, PH_ALIGN_LEFT, 1, DT_END_ELLIPSIS);
            TreeNew_NodesStructured(context->TreeNewHandle);

            PhInitializeLayoutManager(&context->LayoutManager, WindowHandle);
            PhAddLayoutItem(&context->LayoutManager, context->TreeNewHandle, NULL, PH_ANCHOR_ALL);
            PhAddLayoutItem(&context->LayoutManager, GetDlgItem(WindowHandle, IDC_COPYJSON), NULL, PH_ANCHOR_RIGHT | PH_ANCHOR_BOTTOM);
            PhAddLayoutItem(&context->LayoutManager, GetDlgItem(WindowHandle, IDCANCEL), NULL, PH_ANCHOR_RIGHT | PH_ANCHOR_BOTTOM);

            PhCenterWindow(WindowHandle, SystemInformer_GetWindowHandle());
            PhRegisterDialog(WindowHandle);
            PhInitializeWindowTheme(WindowHandle, !!PhGetIntegerSetting(SETTING_ENABLE_THEME_SUPPORT));
        }
        break;
    case WM_SIZE:
        {
            PhLayoutManagerLayout(&context->LayoutManager);
        }
        break;
    case WM_COMMAND:
        {
            switch (GET_WM_COMMAND_ID(wParam, lParam))
            {
            case IDCANCEL:
                DestroyWindow(WindowHandle);
                break;
            case IDC_COPYJSON:
                {
                    PPH_STRING text = PhConvertUtf8ToUtf16Ex(context->Output->Buffer, context->Output->Length);

                    if (text)
                    {
                        PhSetClipboardString(WindowHandle, &text->sr);
                        PhDereferenceObject(text);
                    }
                }
                break;
            }
        }
        break;
    case WM_DESTROY:
        {
            PhUnregisterDialog(WindowHandle);
            PhDeleteLayoutManager(&context->LayoutManager);
            PhRemoveWindowContext(WindowHandle, PH_WINDOW_CONTEXT_DEFAULT);
            WslpFreeInspectContext(context);
        }
        break;
    }

    return FALSE;
}

/**
 * Opens the Inspect window, or reports why the inspect failed. Runs on the GUI thread.
 *
 * \param Parameter The inspect context. The window, or this function on failure, frees it.
 */
static VOID NTAPI WslpShowInspectWindow(
    _In_ PVOID Parameter
    )
{
    PWSL_INSPECT_CONTEXT context = Parameter;
    HWND windowHandle;

    if (!NT_SUCCESS(context->Status))
    {
        PhShowStatus(SystemInformer_GetWindowHandle(), L"Unable to inspect the container.", context->Status, 0);
        WslpFreeInspectContext(context);
        return;
    }

    if (windowHandle = PhCreateDialog(PluginInstance->DllBase, MAKEINTRESOURCE(IDD_INSPECT), NULL, WslpInspectDialogProc, context))
        ShowWindow(windowHandle, SW_SHOW);
    else
        WslpFreeInspectContext(context);
}

/**
 * Runs "wslc inspect", or GET /containers/<id>/json on an engine, off the GUI thread; either
 * can wait on a service.
 */
_Function_class_(USER_THREAD_START_ROUTINE)
static NTSTATUS NTAPI WslpInspectThread(
    _In_ PVOID Parameter
    )
{
    PWSL_INSPECT_CONTEXT context = Parameter;
    PPH_STRING arguments;

    if (context->PipeName && (arguments = PhFormatString(L"/containers/%s/json", context->ContainerId->Buffer)))
    {
        PPH_BYTES path = PhConvertUtf16ToUtf8Ex(arguments->Buffer, arguments->Length);
        ULONG statusCode;

        context->Status = WslEngineRequest(context->PipeName, "GET", path->Buffer, WSL_COMMAND_TIMEOUT_MS, &statusCode, &context->Output);

        if (NT_SUCCESS(context->Status) && statusCode != 200)
        {
            context->Status = statusCode == 404 ? STATUS_NOT_FOUND : STATUS_UNSUCCESSFUL;
            PhClearReference(&context->Output);
        }

        PhDereferenceObject(path);
        PhDereferenceObject(arguments);
    }
    else if (context->PipeName)
    {
        context->Status = STATUS_NO_MEMORY;
    }
    else if (arguments = PhFormatString(L"--session \"%s\" inspect %s", context->SessionName->Buffer, context->ContainerId->Buffer))
    {
        context->Status = WslRunCommand(WslGetWslcFileName(), &arguments->sr, &context->Output);
        PhDereferenceObject(arguments);
    }
    else
    {
        context->Status = STATUS_NO_MEMORY;
    }

    SystemInformer_Invoke(WslpShowInspectWindow, context);

    return STATUS_SUCCESS;
}

/**
 * Inspects a container and shows the result in the Inspect window.
 *
 * \param SessionName The session the container runs in.
 * \param ContainerId The container ID.
 * \param ContainerName The container name, for the window title.
 * \return NTSTATUS code indicating whether the inspect could be started.
 */
NTSTATUS WslShowContainerInspect(
    _In_ PPH_STRING SessionName,
    _In_ PPH_STRING ContainerId,
    _In_ PPH_STRING ContainerName
    )
{
    PWSL_INSPECT_CONTEXT context;
    NTSTATUS status;

    if (!WslGetWslcFileName())
        return STATUS_NOT_SUPPORTED;
    if (!WslIsSafeSessionName(SessionName) || !WslIsSafeContainerId(ContainerId))
        return STATUS_INVALID_PARAMETER;

    context = PhAllocateZero(sizeof(WSL_INSPECT_CONTEXT));
    PhSetReference(&context->SessionName, SessionName);
    PhSetReference(&context->ContainerId, ContainerId);
    PhSetReference(&context->ContainerName, ContainerName);

    if (!NT_SUCCESS(status = PhCreateThread2(WslpInspectThread, context)))
        WslpFreeInspectContext(context);

    return status;
}

/**
 * Inspects a container of a Docker API engine and shows the result in the Inspect window.
 *
 * \param PipeName The engine's pipe.
 * \param ContainerId The container ID.
 * \param ContainerName The container name, for the window title.
 * \return NTSTATUS code indicating whether the inspect could be started.
 */
NTSTATUS WslShowEngineContainerInspect(
    _In_ PPH_STRING PipeName,
    _In_ PPH_STRING ContainerId,
    _In_ PPH_STRING ContainerName
    )
{
    PWSL_INSPECT_CONTEXT context;
    NTSTATUS status;

    if (!WslIsSafePipeName(PipeName) || !WslIsSafeContainerId(ContainerId))
        return STATUS_INVALID_PARAMETER;

    context = PhAllocateZero(sizeof(WSL_INSPECT_CONTEXT));
    PhSetReference(&context->PipeName, PipeName);
    PhSetReference(&context->ContainerId, ContainerId);
    PhSetReference(&context->ContainerName, ContainerName);

    if (!NT_SUCCESS(status = PhCreateThread2(WslpInspectThread, context)))
        WslpFreeInspectContext(context);

    return status;
}
