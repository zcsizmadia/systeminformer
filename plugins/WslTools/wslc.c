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

// WSL container (WSLC) sessions and their containers, read with the wslc CLI.
//
// "wslc system session list" lists only running sessions and does not start one. Containers
// are only ever listed for a session it reported, and always with --session, because
// "wslc list" without a session could create or start the default session.

static CONST PH_STRINGREF WslpSpace = PH_STRINGREF_INIT(L" ");

typedef struct _WSL_CONTAINER_STATS
{
    PPH_STRING Id; // Full ID
    FLOAT CpuPercent; // Docker style: 100 per busy vCPU
    ULONG64 MemoryBytes;
    ULONG NumberOfProcesses;
} WSL_CONTAINER_STATS, *PWSL_CONTAINER_STATS;

/**
 * Determines whether a session name can be passed to wslc.exe inside double quotes.
 *
 * \param Name The session display name. It can contain spaces, e.g. from the user name.
 * \return TRUE if the name is not empty and has no double quote or control character.
 */
BOOLEAN WslIsSafeSessionName(
    _In_ PPH_STRING Name
    )
{
    if (Name->Length == 0)
        return FALSE;

    for (SIZE_T i = 0; i < Name->Length / sizeof(WCHAR); i++)
    {
        if (Name->Buffer[i] == L'"' || Name->Buffer[i] < L' ')
            return FALSE;
    }

    return TRUE;
}

/**
 * Determines whether a container ID can be passed to wslc.exe unquoted.
 *
 * \param Id The container ID.
 * \return TRUE if the ID is not empty and only has hexadecimal digits.
 */
BOOLEAN WslIsSafeContainerId(
    _In_ PPH_STRING Id
    )
{
    if (Id->Length == 0)
        return FALSE;

    for (SIZE_T i = 0; i < Id->Length / sizeof(WCHAR); i++)
    {
        WCHAR c = Id->Buffer[i];

        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f')))
            return FALSE;
    }

    return TRUE;
}

/**
 * Frees a container.
 */
static VOID WslpFreeContainer(
    _In_ PWSL_CONTAINER Container
    )
{
    PhClearReference(&Container->Id);
    PhClearReference(&Container->Name);
    PhClearReference(&Container->Image);
    PhClearReference(&Container->State);
    PhClearReference(&Container->Status);
    PhClearReference(&Container->Ports);
    PhFree(Container);
}

/**
 * Frees a session list returned by WslQuerySessions.
 *
 * \param Sessions The sessions.
 */
VOID WslFreeSessions(
    _In_ PPH_LIST Sessions
    )
{
    for (ULONG i = 0; i < Sessions->Count; i++)
    {
        PWSL_SESSION session = Sessions->Items[i];

        for (ULONG j = 0; j < session->Containers->Count; j++)
            WslpFreeContainer(session->Containers->Items[j]);

        PhDereferenceObject(session->Containers);
        PhClearReference(&session->Name);
        PhFree(session);
    }

    PhDereferenceObject(Sessions);
}

/**
 * Calls a function for every non-empty line of UTF-8 output.
 */
static VOID WslpForEachLine(
    _In_ PPH_BYTES Output,
    _In_ VOID (*Callback)(_In_ PPH_BYTES Line, _In_ PVOID Context),
    _In_ PVOID Context
    )
{
    SIZE_T lineStart = 0;

    for (SIZE_T i = 0; i <= Output->Length; i++)
    {
        if (i == Output->Length || Output->Buffer[i] == '\n')
        {
            SIZE_T length = i - lineStart;

            if (length != 0 && Output->Buffer[lineStart + length - 1] == '\r')
                length--;

            if (length != 0)
            {
                PPH_BYTES line = PhCreateBytesEx(Output->Buffer + lineStart, length);

                Callback(line, Context);
                PhDereferenceObject(line);
            }

            lineStart = i + 1;
        }
    }
}

/**
 * Parses the table printed by "wslc system session list":
 *
 *     ID   Creator PID   Display Name
 *     1    14132         wslc-cli-...
 *
 * \remarks The display name can contain spaces, so it is taken from the column where its
 * header starts to the end of the line, rather than by splitting on spaces.
 */
static PPH_LIST WslpParseSessionList(
    _In_ PPH_BYTES Output
    )
{
    static CONST PH_STRINGREF nameHeader = PH_STRINGREF_INIT(L"Display Name");
    PPH_LIST sessions;
    PPH_STRING text;
    PH_STRINGREF remaining;
    PH_STRINGREF line;
    ULONG_PTR nameColumn = SIZE_MAX;

    sessions = PhCreateList(2);
    text = PhConvertUtf8ToUtf16Ex(Output->Buffer, Output->Length);
    remaining = text->sr;

    while (remaining.Length != 0)
    {
        PhSplitStringRefAtChar(&remaining, L'\n', &line, &remaining);

        if (line.Length != 0 && line.Buffer[line.Length / sizeof(WCHAR) - 1] == L'\r')
            line.Length -= sizeof(WCHAR);

        if (nameColumn == SIZE_MAX)
        {
            nameColumn = PhFindStringInStringRef(&line, &nameHeader, FALSE);
            continue;
        }

        if (line.Length / sizeof(WCHAR) > nameColumn)
        {
            PH_STRINGREF idPart;
            PH_STRINGREF namePart;
            PH_STRINGREF rest;
            ULONG64 id;

            PhSplitStringRefAtChar(&line, L' ', &idPart, &rest);
            namePart.Buffer = line.Buffer + nameColumn;
            namePart.Length = line.Length - nameColumn * sizeof(WCHAR);
            PhTrimStringRef(&namePart, &WslpSpace, 0);

            if (PhStringToUInt64(&idPart, 10, &id) && namePart.Length != 0)
            {
                PWSL_SESSION session = PhAllocateZero(sizeof(WSL_SESSION));

                session->Id = (ULONG)id;
                session->Name = PhCreateString2(&namePart);
                session->Containers = PhCreateList(4);
                PhAddItemList(sessions, session);
            }
        }
    }

    PhDereferenceObject(text);

    return sessions;
}

/**
 * Parses one line of "wslc list --format json".
 */
static VOID WslpParseContainerLine(
    _In_ PPH_BYTES Line,
    _In_ PVOID Context
    )
{
    PWSL_SESSION session = Context;
    PVOID object;
    PWSL_CONTAINER container;

    if (!NT_SUCCESS(PhCreateJsonParserEx(&object, Line, FALSE)) || !object)
        return;

    if (PhGetJsonObjectType(object) == PH_JSON_OBJECT_TYPE_OBJECT)
    {
        container = PhAllocateZero(sizeof(WSL_CONTAINER));
        container->Id = PhGetJsonValueAsString(object, "ID");
        container->Name = PhGetJsonValueAsString(object, "Names");
        container->Image = PhGetJsonValueAsString(object, "Image");
        container->State = PhGetJsonValueAsString(object, "State");
        container->Status = PhGetJsonValueAsString(object, "Status");
        container->Ports = PhGetJsonValueAsString(object, "Ports");
        container->Running = container->State && PhEqualString2(container->State, L"running", TRUE);

        // wslc prints states in lower case; show them like the other rows. The string is new and
        // not shared yet, so it can still be changed.
        if (!PhIsNullOrEmptyString(container->State))
            container->State->Buffer[0] = RtlUpcaseUnicodeChar(container->State->Buffer[0]);

        if (container->Id && container->Name)
            PhAddItemList(session->Containers, container);
        else
            WslpFreeContainer(container);
    }

    PhFreeJsonObject(object);
}

/**
 * Parses a size as wslc prints it, e.g. "21.16MiB" or "1.49MB".
 *
 * \return The size in bytes, or 0 if it cannot be parsed.
 */
static ULONG64 WslpParseSize(
    _In_ PH_STRINGREF Text
    )
{
    static CONST struct { PCWSTR Suffix; DOUBLE Factor; } units[] =
    {
        // Binary units first, so "KiB" is not read as "B" with a leftover "Ki".
        { L"KiB", 1024.0 }, { L"MiB", 1024.0 * 1024 }, { L"GiB", 1024.0 * 1024 * 1024 }, { L"TiB", 1024.0 * 1024 * 1024 * 1024 },
        { L"kB", 1000.0 }, { L"MB", 1000.0 * 1000 }, { L"GB", 1000.0 * 1000 * 1000 }, { L"TB", 1000.0 * 1000 * 1000 * 1000 },
        { L"B", 1.0 },
    };

    PhTrimStringRef(&Text, &WslpSpace, 0);

    for (ULONG i = 0; i < RTL_NUMBER_OF(units); i++)
    {
        PH_STRINGREF suffix;
        PH_STRINGREF number;
        DOUBLE value;

        PhInitializeStringRef(&suffix, (PWSTR)units[i].Suffix);

        if (PhEndsWithStringRef(&Text, &suffix, FALSE))
        {
            number.Buffer = Text.Buffer;
            number.Length = Text.Length - suffix.Length;

            if (PhStringToDouble(&number, 0, &value) && value >= 0)
                return (ULONG64)(value * units[i].Factor);

            return 0;
        }
    }

    return 0;
}

/**
 * Parses one line of "wslc stats --format json".
 */
static VOID WslpParseStatsLine(
    _In_ PPH_BYTES Line,
    _In_ PVOID Context
    )
{
    PPH_LIST statsList = Context;
    PVOID object;

    if (!NT_SUCCESS(PhCreateJsonParserEx(&object, Line, FALSE)) || !object)
        return;

    if (PhGetJsonObjectType(object) == PH_JSON_OBJECT_TYPE_OBJECT)
    {
        PWSL_CONTAINER_STATS stats;
        PPH_STRING cpu;
        PPH_STRING memory;

        stats = PhAllocateZero(sizeof(WSL_CONTAINER_STATS));
        stats->Id = PhGetJsonValueAsString(object, "ID");
        stats->NumberOfProcesses = (ULONG)PhGetJsonValueAsUInt64(object, "PIDs");

        // "CPUPerc": "12.34%"
        if (cpu = PhGetJsonValueAsString(object, "CPUPerc"))
        {
            PH_STRINGREF number = cpu->sr;
            DOUBLE value;

            if (PhEndsWithStringRef2(&number, L"%", FALSE))
                number.Length -= sizeof(WCHAR);

            if (PhStringToDouble(&number, 0, &value) && value >= 0)
                stats->CpuPercent = (FLOAT)value;

            PhDereferenceObject(cpu);
        }

        // "MemUsage": "21.16MiB / 7.611GiB"; the part before the slash is the usage.
        if (memory = PhGetJsonValueAsString(object, "MemUsage"))
        {
            PH_STRINGREF usage;
            PH_STRINGREF limit;

            PhSplitStringRefAtChar(&memory->sr, L'/', &usage, &limit);
            stats->MemoryBytes = WslpParseSize(usage);
            PhDereferenceObject(memory);
        }

        if (stats->Id)
            PhAddItemList(statsList, stats);
        else
            PhFree(stats);
    }

    PhFreeJsonObject(object);
}

/**
 * Lists the containers of a running session and adds their resource usage.
 *
 * \remarks "wslc stats" samples CPU usage over about two seconds, so this blocks for that
 * long. It only runs on the provider thread, and only when a container is running.
 */
static VOID WslpQuerySessionContainers(
    _In_ PPH_STRING FileName,
    _In_ PWSL_SESSION Session
    )
{
    PPH_STRING arguments;
    PPH_BYTES output;
    PPH_LIST statsList;
    BOOLEAN anyRunning = FALSE;

    if (!WslIsSafeSessionName(Session->Name))
    {
        Session->QueryStatus = STATUS_INVALID_PARAMETER;
        return;
    }

    // --all also lists stopped containers; they are shown greyed out.
    arguments = PhFormatString(L"--session \"%s\" list --all --format json", Session->Name->Buffer);
    Session->QueryStatus = WslRunCommand(FileName, &arguments->sr, &output);
    PhDereferenceObject(arguments);

    if (!NT_SUCCESS(Session->QueryStatus))
        return;

    WslpForEachLine(output, WslpParseContainerLine, Session);
    PhDereferenceObject(output);

    for (ULONG i = 0; i < Session->Containers->Count; i++)
        anyRunning |= ((PWSL_CONTAINER)Session->Containers->Items[i])->Running;

    if (!anyRunning)
        return;

    arguments = PhFormatString(L"--session \"%s\" stats --format json", Session->Name->Buffer);

    if (!NT_SUCCESS(WslRunCommand(FileName, &arguments->sr, &output)))
    {
        PhDereferenceObject(arguments);
        return;
    }

    PhDereferenceObject(arguments);
    statsList = PhCreateList(Session->Containers->Count);
    WslpForEachLine(output, WslpParseStatsLine, statsList);
    PhDereferenceObject(output);

    Session->HaveStats = TRUE;

    for (ULONG i = 0; i < statsList->Count; i++)
    {
        PWSL_CONTAINER_STATS stats = statsList->Items[i];

        for (ULONG j = 0; j < Session->Containers->Count; j++)
        {
            PWSL_CONTAINER container = Session->Containers->Items[j];

            // "wslc list" prints the short ID, "wslc stats" the full one.
            if (PhStartsWithString(stats->Id, container->Id, TRUE))
            {
                // CPUPerc counts 100 per busy vCPU; the tab shows a fraction of all host processors.
                container->CpuUsage = stats->CpuPercent / 100.0f / WslGetHostProcessorCount();
                container->MemoryBytes = stats->MemoryBytes;
                container->NumberOfProcesses = stats->NumberOfProcesses;
                container->HaveStats = TRUE;

                Session->CpuUsage += container->CpuUsage;
                Session->MemoryBytes += container->MemoryBytes;
                break;
            }
        }

        PhDereferenceObject(stats->Id);
        PhFree(stats);
    }

    PhDereferenceObject(statsList);
}

/**
 * Opens a console window with a shell in a container, or with its log output.
 *
 * \param SessionName The session the container runs in.
 * \param ContainerId The container ID.
 * \param Logs TRUE to follow the container's logs, FALSE to open a shell.
 * \return NTSTATUS code indicating success or failure.
 * \remarks The shell is sh, which almost every image has; the window stays open while it runs.
 */
NTSTATUS WslStartContainerConsole(
    _In_ PPH_STRING SessionName,
    _In_ PPH_STRING ContainerId,
    _In_ BOOLEAN Logs
    )
{
    NTSTATUS status;
    PPH_STRING fileName;
    PPH_STRING commandLine;

    if (!(fileName = WslGetWslcFileName()))
        return STATUS_NOT_SUPPORTED;
    if (!WslIsSafeSessionName(SessionName) || !WslIsSafeContainerId(ContainerId))
        return STATUS_INVALID_PARAMETER;

    commandLine = PhFormatString(
        Logs ? L"\"%s\" --session \"%s\" logs --follow %s" : L"\"%s\" --session \"%s\" exec --interactive --tty %s sh",
        fileName->Buffer,
        SessionName->Buffer,
        ContainerId->Buffer
        );

    status = PhCreateProcessWin32Ex(
        fileName->Buffer,
        commandLine->Buffer,
        NULL,
        NULL,
        NULL,
        PH_CREATE_PROCESS_NEW_CONSOLE,
        NULL,
        NULL,
        NULL,
        NULL
        );

    PhDereferenceObject(commandLine);

    return status;
}

/**
 * Lists the running WSLC sessions and their containers.
 *
 * \return The sessions, or NULL if this WSL version has no wslc.exe or the session list
 * could not be read. Free the list with WslFreeSessions.
 */
PPH_LIST WslQuerySessions(
    VOID
    )
{
    static CONST PH_STRINGREF sessionListArguments = PH_STRINGREF_INIT(L"system session list");
    PPH_STRING fileName;
    PPH_BYTES output;
    PPH_LIST sessions;

    if (!(fileName = WslGetWslcFileName()))
        return NULL;

    if (!NT_SUCCESS(WslRunCommand(fileName, &sessionListArguments, &output)))
        return NULL;

    sessions = WslpParseSessionList(output);
    PhDereferenceObject(output);

    for (ULONG i = 0; i < sessions->Count; i++)
        WslpQuerySessionContainers(fileName, sessions->Items[i]);

    return sessions;
}
