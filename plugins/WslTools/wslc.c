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

// The processes of a session VM, with the cgroup of each, printed once. This is a one-shot
// snapshot per refresh rather than a loop like the distribution collector, because a loop
// started with "session run" keeps running inside the VM when wslc.exe is killed. The script
// is one double-quoted argument, so it must not contain double quotes itself.
#define WSL_SESSION_PROCESS_SCRIPT \
    L"t=$(getconf CLK_TCK 2>/dev/null || echo 100); " \
    L"p=$(getconf PAGESIZE 2>/dev/null || echo 4096); " \
    L"read -r k < /proc/sys/kernel/osrelease; " \
    L"read -r u i < /proc/uptime; " \
    L"m=0; a=0; while read -r n v r; do case $n in MemTotal:) m=$v ;; MemAvailable:) a=$v ;; esac; done < /proc/meminfo; " \
    L"echo @ $u $t $p $$ $k $m $a; " \
    L"cat /proc/[0-9]*/stat 2>/dev/null; " \
    L"for d in /proc/[0-9]*; do read -r g < $d/cgroup 2>/dev/null && echo %${d#/proc/} $g; done; " \
    L"echo @end"

typedef struct _WSL_SESSION_PARSER
{
    PPH_STRING Name;
    PWSL_FRAME_PARSER Parser; // Keeps the previous snapshot of the session, for CPU usage
    BOOLEAN Seen;
} WSL_SESSION_PARSER, *PWSL_SESSION_PARSER;

static CONST PH_STRINGREF WslpSpace = PH_STRINGREF_INIT(L" ");
static PPH_LIST WslpSessionParsers = NULL; // PWSL_SESSION_PARSER, used only by the provider thread

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
 * \return TRUE if the name is not empty, has no double quote or control character, and does not
 * end with a backslash, which would escape the closing quote.
 */
BOOLEAN WslIsSafeSessionName(
    _In_ PPH_STRING Name
    )
{
    if (Name->Length == 0 || Name->Buffer[Name->Length / sizeof(WCHAR) - 1] == L'\\')
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
        PhClearReference(&session->Processes);
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
 * Finds where the third column starts in the header of "wslc system session list".
 *
 * \param Header The header line, e.g. "ID   Creator PID   Display Name".
 * \return The character index of the column, or SIZE_MAX if the header has fewer than three
 * columns separated by two or more spaces.
 */
static ULONG_PTR WslpGetSessionNameColumn(
    _In_ PCPH_STRINGREF Header
    )
{
    SIZE_T count = Header->Length / sizeof(WCHAR);
    ULONG separators = 0;

    for (SIZE_T i = 0; i + 1 < count; i++)
    {
        if (Header->Buffer[i] != L' ' || Header->Buffer[i + 1] != L' ')
            continue;

        // Skip the whole run of spaces; the next column starts after it.
        while (i < count && Header->Buffer[i] == L' ')
            i++;

        if (++separators == 2)
            return i < count ? i : SIZE_MAX;
    }

    return SIZE_MAX;
}

/**
 * Parses the table printed by "wslc system session list":
 *
 *     ID   Creator PID   Display Name
 *     1    14132         wslc-cli-...
 *
 * \remarks The display name can contain spaces, so it is taken from the column where its
 * header starts to the end of the line, rather than by splitting on spaces. The command has no
 * JSON format. The header text can be localized, so the column is found from the layout instead:
 * columns are separated by two or more spaces, and the name is the third column. Nothing
 * follows the name on a line, so it is not trimmed; a trailing space is part of the name.
 */
static PPH_LIST WslpParseSessionList(
    _In_ PPH_BYTES Output
    )
{
    PPH_LIST sessions;
    PPH_STRING text;
    PH_STRINGREF remaining;
    PH_STRINGREF line;
    ULONG_PTR nameColumn = SIZE_MAX;

    sessions = PhCreateList(2);

    if (!(text = PhConvertUtf8ToUtf16Ex(Output->Buffer, Output->Length)))
        return sessions;

    remaining = text->sr;

    while (remaining.Length != 0)
    {
        PhSplitStringRefAtChar(&remaining, L'\n', &line, &remaining);

        if (line.Length != 0 && line.Buffer[line.Length / sizeof(WCHAR) - 1] == L'\r')
            line.Length -= sizeof(WCHAR);

        if (nameColumn == SIZE_MAX)
        {
            nameColumn = WslpGetSessionNameColumn(&line);

            // Without a recognizable header no line can be parsed.
            if (nameColumn == SIZE_MAX)
                break;

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
    if (!(arguments = PhFormatString(L"--session \"%s\" list --all --format json", Session->Name->Buffer)))
    {
        Session->QueryStatus = STATUS_NO_MEMORY;
        return;
    }

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

    if (!(arguments = PhFormatString(L"--session \"%s\" stats --format json", Session->Name->Buffer)))
        return;

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

    if (!commandLine)
        return STATUS_NO_MEMORY;

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
 * Gets the frame parser of a session, and marks it as still in use.
 */
static PWSL_FRAME_PARSER WslpGetSessionParser(
    _In_ PPH_STRING Name
    )
{
    PWSL_SESSION_PARSER entry;

    if (!WslpSessionParsers)
        WslpSessionParsers = PhCreateList(2);

    for (ULONG i = 0; i < WslpSessionParsers->Count; i++)
    {
        entry = WslpSessionParsers->Items[i];

        if (PhEqualString(entry->Name, Name, FALSE))
        {
            entry->Seen = TRUE;
            return entry->Parser;
        }
    }

    entry = PhAllocateZero(sizeof(WSL_SESSION_PARSER));
    PhSetReference(&entry->Name, Name);
    entry->Parser = WslCreateFrameParser();
    entry->Seen = TRUE;
    PhAddItemList(WslpSessionParsers, entry);

    return entry->Parser;
}

/**
 * Frees the parsers of sessions that were not seen since the last call.
 */
static VOID WslpPruneSessionParsers(
    VOID
    )
{
    if (!WslpSessionParsers)
        return;

    for (ULONG i = WslpSessionParsers->Count; i != 0; i--)
    {
        PWSL_SESSION_PARSER entry = WslpSessionParsers->Items[i - 1];

        if (!entry->Seen)
        {
            PhRemoveItemList(WslpSessionParsers, i - 1);
            WslDestroyFrameParser(entry->Parser);
            PhDereferenceObject(entry->Name);
            PhFree(entry);
        }
        else
        {
            entry->Seen = FALSE;
        }
    }
}

/**
 * Reads the processes of a running session VM, with the container each one runs in.
 *
 * \remarks Runs a short shell in the session VM, only for a session that "wslc system session
 * list" just reported and only when a container is running.
 */
static VOID WslpQuerySessionProcesses(
    _In_ PPH_STRING FileName,
    _In_ PWSL_SESSION Session
    )
{
    PPH_STRING arguments;
    PPH_BYTES output;
    BOOLEAN anyRunning = FALSE;

    for (ULONG i = 0; i < Session->Containers->Count; i++)
        anyRunning |= ((PWSL_CONTAINER)Session->Containers->Items[i])->Running;

    if (!anyRunning || !WslIsSafeSessionName(Session->Name))
        return;

    if (!(arguments = PhFormatString(L"--session \"%s\" system session run /bin/sh -c \"%s\"", Session->Name->Buffer, WSL_SESSION_PROCESS_SCRIPT)))
        return;

    if (NT_SUCCESS(WslRunCommand(FileName, &arguments->sr, &output)))
    {
        Session->Processes = WslParseFrameOutput(WslpGetSessionParser(Session->Name), output);
        PhDereferenceObject(output);
    }

    PhDereferenceObject(arguments);
}

/**
 * Stops tracking all sessions, e.g. when the tab is hidden, so CPU usage starts fresh.
 */
VOID WslResetSessionProcesses(
    VOID
    )
{
    if (!WslpSessionParsers)
        return;

    for (ULONG i = 0; i < WslpSessionParsers->Count; i++)
    {
        PWSL_SESSION_PARSER entry = WslpSessionParsers->Items[i];

        WslDestroyFrameParser(entry->Parser);
        PhDereferenceObject(entry->Name);
        PhFree(entry);
    }

    PhClearList(WslpSessionParsers);
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
    {
        WslpQuerySessionContainers(fileName, sessions->Items[i]);
        WslpQuerySessionProcesses(fileName, sessions->Items[i]);
    }

    WslpPruneSessionParsers();

    return sessions;
}
