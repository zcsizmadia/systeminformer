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

// A collector is one long-running wsl.exe per running distribution. Inside the distribution
// a POSIX shell loop prints every process's /proc/<pid>/stat line, framed by a header, at
// the refresh interval. Nothing is installed in the distribution: the loop uses only sh,
// cat and sleep, which even busybox provides. Killing wsl.exe ends the loop inside the
// distribution as well, and the job it runs in guarantees that also when System Informer exits.
//
// Every distribution has its own PID namespace in the shared VM, so /proc lists only the
// processes of the distribution the loop runs in.

// The first two frames are one second apart, so CPU usage, which needs two frames, is
// available right after the tab is shown instead of one full interval later.
//
// When the distribution hosts a container engine, the header ends with the Docker engine ID
// ("-" if there is none), and the cgroup of each process tells the container it runs in. The
// cgroup lines come before the stat lines, where a process name cannot forge them. A
// redirection that fails is reported before a "2>" on the same command applies, so the reads
// are grouped to keep those errors out of the output. The script is a PhFormatString format,
// so "%%" prints the "%" that starts a cgroup line.
#define WSL_PROCESS_SCRIPT \
    L"t=$(getconf CLK_TCK 2>/dev/null || echo 100); " \
    L"p=$(getconf PAGESIZE 2>/dev/null || echo 4096); " \
    L"read k < /proc/sys/kernel/osrelease; " \
    L"s=1; " \
    L"while :; do " \
    L"read u i < /proc/uptime; " \
    L"m=0; a=0; while read -r n v r; do case $n in MemTotal:) m=$v ;; MemAvailable:) a=$v ;; esac; done < /proc/meminfo; " \
    L"e=; { read -r e < /var/lib/docker/engine-id; } 2>/dev/null; " \
    L"echo @ $u $t $p $$ $k $m $a ${e:--}; " \
    L"for d in /proc/[0-9]*; do { read -r g < $d/cgroup; } 2>/dev/null && echo %%${d#/proc/} $g; done; " \
    L"cat /proc/[0-9]*/stat 2>/dev/null; " \
    L"echo @end; " \
    L"sleep $s; " \
    L"s=%lu; " \
    L"done"

// How long WslStopCollector waits for the reader thread before cancelling its read.
#define WSL_COLLECTOR_STOP_TIMEOUT_MS 2000

// Fields of /proc/<pid>/stat, counted from the state field that follows the name.
#define WSL_STAT_STATE 0
#define WSL_STAT_PPID 1
#define WSL_STAT_UTIME 11
#define WSL_STAT_STIME 12
#define WSL_STAT_CUTIME 13
#define WSL_STAT_CSTIME 14
#define WSL_STAT_THREADS 17
#define WSL_STAT_STARTTIME 19
#define WSL_STAT_RSS 21
#define WSL_STAT_FIELD_COUNT 22

typedef struct _WSL_CPU_SAMPLE
{
    ULONG ProcessId;
    ULONG64 StartTime;
    ULONG64 Ticks; // utime + stime
} WSL_CPU_SAMPLE, *PWSL_CPU_SAMPLE;

// A /proc/<pid>/stat line as parsed, before the collector's own processes are removed.
typedef struct _WSL_STAT_ENTRY
{
    WSL_LINUX_PROCESS Process;
    ULONG64 Ticks; // utime + stime
    ULONG64 ChildTicks; // cutime + cstime: exited children that the process has waited for
} WSL_STAT_ENTRY, *PWSL_STAT_ENTRY;

// Turns lines of frame output into frames, and keeps the previous frame's CPU samples.
typedef struct _WSL_FRAME_PARSER
{
    PPH_HASHTABLE PreviousSamples; // WSL_CPU_SAMPLE
    ULONG64 PreviousTotalTicks;
    DOUBLE PreviousUptime;
    BOOLEAN HavePrevious;
    PPH_LIST Entries; // PWSL_STAT_ENTRY of the frame being read
    DOUBLE Uptime;
    ULONG64 TicksPerSecond;
    ULONG64 PageSize;
    ULONG SelfProcessId;
    PPH_STRING KernelRelease;
    ULONG64 MemoryTotal;
    ULONG64 MemoryAvailable;
    PPH_STRING EngineId; // Docker engine ID of the distribution, from the header, or NULL
    // Simple hashtable of the frame being read: process ID -> container ID (PPH_STRING). The
    // cgroup lines come before the stat lines and are applied when the frame completes.
    PPH_HASHTABLE Cgroups;
    BOOLEAN InFrame;
    BOOLEAN InStats; // A stat line of the frame was read; later cgroup lines are ignored
} WSL_FRAME_PARSER, *PWSL_FRAME_PARSER;

typedef struct _WSL_COLLECTOR
{
    PPH_STRING DistroName;
    HANDLE ProcessHandle;
    HANDLE ReadHandle;
    HANDLE JobHandle; // Kill-on-close job with wsl.exe and its helper processes
    HANDLE ThreadHandle;
    LONG Running;
    LONG Stopping; // Set by WslStopCollector; the reader thread checks it before each read

    PH_QUEUED_LOCK FrameLock;
    PWSL_PROCESS_FRAME Frame; // Latest complete frame, protected by FrameLock

    // Used only by the reader thread.
    PWSL_FRAME_PARSER Parser;
    ULONG FrameCount; // Complete frames so far, counted up to 2
} WSL_COLLECTOR, *PWSL_COLLECTOR;

static PPH_OBJECT_TYPE WslpProcessFrameType = NULL;

/**
 * Frees the processes owned by a frame.
 */
_Function_class_(PH_TYPE_DELETE_PROCEDURE)
static VOID NTAPI WslpProcessFrameDeleteProcedure(
    _In_ PVOID Object,
    _In_ ULONG Flags
    )
{
    PWSL_PROCESS_FRAME frame = Object;

    for (ULONG i = 0; i < frame->Processes->Count; i++)
    {
        PWSL_LINUX_PROCESS process = frame->Processes->Items[i];

        PhClearReference(&process->Name);
        PhClearReference(&process->ContainerId);
        PhFree(process);
    }

    PhDereferenceObject(frame->Processes);
    PhClearReference(&frame->KernelRelease);
    PhClearReference(&frame->EngineId);
}

/**
 * Creates the object type used for process frames. Must be called once before WslStartCollector.
 */
VOID WslInitializeProcessFrameType(
    VOID
    )
{
    WslpProcessFrameType = PhCreateObjectType(L"WslProcessFrame", 0, WslpProcessFrameDeleteProcedure);
}

/**
 * Gets the number of host logical processors across all processor groups.
 *
 * \remarks This is the count the process provider divides CPU time by
 * (PhSystemProcessorInformation, which is not exported), so WSL rows use the same unit as
 * the VM row. PhSystemBasicInformation only covers the current processor group.
 */
ULONG WslGetHostProcessorCount(
    VOID
    )
{
    static ULONG processorCount = 0;

    if (processorCount == 0)
        processorCount = max(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), 1);

    return processorCount;
}

_Function_class_(PH_HASHTABLE_EQUAL_FUNCTION)
static BOOLEAN NTAPI WslpCpuSampleEqualFunction(
    _In_ PVOID Entry1,
    _In_ PVOID Entry2
    )
{
    return ((PWSL_CPU_SAMPLE)Entry1)->ProcessId == ((PWSL_CPU_SAMPLE)Entry2)->ProcessId;
}

_Function_class_(PH_HASHTABLE_HASH_FUNCTION)
static ULONG NTAPI WslpCpuSampleHashFunction(
    _In_ PVOID Entry
    )
{
    return PhHashInt32(((PWSL_CPU_SAMPLE)Entry)->ProcessId);
}

/**
 * Gets the display text for a Linux process state letter.
 *
 * \param State The state letter from /proc/<pid>/stat.
 * \return The display text.
 */
PCPH_STRINGREF WslGetLinuxProcessStateText(
    _In_ WCHAR State
    )
{
    static CONST PH_STRINGREF runningText = PH_STRINGREF_INIT(L"Running");
    static CONST PH_STRINGREF sleepingText = PH_STRINGREF_INIT(L"Sleeping");
    static CONST PH_STRINGREF diskSleepText = PH_STRINGREF_INIT(L"Waiting (disk)");
    static CONST PH_STRINGREF zombieText = PH_STRINGREF_INIT(L"Zombie");
    static CONST PH_STRINGREF stoppedText = PH_STRINGREF_INIT(L"Stopped");
    static CONST PH_STRINGREF idleText = PH_STRINGREF_INIT(L"Idle");
    static CONST PH_STRINGREF otherText = PH_STRINGREF_INIT(L"Other");

    switch (State)
    {
    case L'R':
        return &runningText;
    case L'S':
        return &sleepingText;
    case L'D':
        return &diskSleepText;
    case L'Z':
        return &zombieText;
    case L'T':
    case L't':
        return &stoppedText;
    case L'I':
        return &idleText;
    default:
        return &otherText;
    }
}

/**
 * Determines whether a header field is a Docker engine ID, e.g. a UUID.
 *
 * \return TRUE if the field has 1 to 128 letters, digits, "-" or ":".
 */
static BOOLEAN WslpIsEngineId(
    _In_ PCPH_STRINGREF Text
    )
{
    SIZE_T count = Text->Length / sizeof(WCHAR);

    if (count == 0 || count > 128)
        return FALSE;

    for (SIZE_T i = 0; i < count; i++)
    {
        WCHAR c = Text->Buffer[i];

        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'-' || c == L':'))
            return FALSE;
    }

    return TRUE;
}

/**
 * Parses a frame header, "@ <uptime> <ticks per second> <page size> <shell pid> <kernel release>
 * <MemTotal kB> <MemAvailable kB> [<engine id>]".
 *
 * \remarks A process name can contain a newline, so a line that only starts like a header
 * can be the tail of a stat line. The rest of that stat line always follows the name on the
 * same line, so requiring exactly these fields and nothing after them rejects it. A name has at
 * most 15 bytes, too short to fake a header.
 */
static BOOLEAN WslpParseHeader(
    _In_ PWSL_FRAME_PARSER Parser,
    _In_ PH_STRINGREF Line
    )
{
    PH_STRINGREF part;
    ULONG64 value;

    PhSplitStringRefAtChar(&Line, L' ', &part, &Line); // "@"
    PhSplitStringRefAtChar(&Line, L' ', &part, &Line);

    if (!PhStringToDouble(&part, 0, &Parser->Uptime))
        return FALSE;

    PhSplitStringRefAtChar(&Line, L' ', &part, &Line);

    if (!PhStringToUInt64(&part, 10, &Parser->TicksPerSecond) || Parser->TicksPerSecond == 0)
        return FALSE;

    PhSplitStringRefAtChar(&Line, L' ', &part, &Line);

    if (!PhStringToUInt64(&part, 10, &Parser->PageSize))
        return FALSE;

    PhSplitStringRefAtChar(&Line, L' ', &part, &Line);

    if (!PhStringToUInt64(&part, 10, &value))
        return FALSE;

    Parser->SelfProcessId = (ULONG)value;

    PhSplitStringRefAtChar(&Line, L' ', &part, &Line);

    if (part.Length == 0)
        return FALSE;

    if (!Parser->KernelRelease || !PhEqualStringRef(&Parser->KernelRelease->sr, &part, FALSE))
        PhMoveReference(&Parser->KernelRelease, PhCreateString2(&part));

    // /proc/meminfo is in kB.
    PhSplitStringRefAtChar(&Line, L' ', &part, &Line);

    if (!PhStringToUInt64(&part, 10, &Parser->MemoryTotal))
        return FALSE;

    PhSplitStringRefAtChar(&Line, L' ', &part, &Line);

    if (!PhStringToUInt64(&part, 10, &Parser->MemoryAvailable))
        return FALSE;

    Parser->MemoryTotal *= 1024;
    Parser->MemoryAvailable *= 1024;

    // The collector adds the Docker engine ID of the distribution, from
    // /var/lib/docker/engine-id, or "-"; the session snapshot has no such field.
    PhSplitStringRefAtChar(&Line, L' ', &part, &Line);

    if (WslpIsEngineId(&part))
    {
        if (!Parser->EngineId || !PhEqualStringRef(&Parser->EngineId->sr, &part, FALSE))
            PhMoveReference(&Parser->EngineId, PhCreateString2(&part));
    }
    else
    {
        PhClearReference(&Parser->EngineId);
    }

    return Line.Length == 0;
}

/**
 * Parses a /proc/<pid>/stat line.
 *
 * \return A new entry, or NULL if the line is malformed.
 * \remarks The name is between the first "(" and the last ")"; it can itself contain
 * spaces and parentheses, e.g. "init(skrog-engi".
 */
static PWSL_STAT_ENTRY WslpParseStatLine(
    _In_ PWSL_FRAME_PARSER Parser,
    _In_ PH_STRINGREF Line
    )
{
    PWSL_STAT_ENTRY entry;
    PH_STRINGREF pidPart;
    PH_STRINGREF remaining;
    PH_STRINGREF name;
    ULONG64 fields[WSL_STAT_FIELD_COUNT] = { 0 };
    WCHAR state = 0;
    ULONG_PTR open;
    ULONG_PTR close;
    ULONG64 pid;

    open = PhFindCharInStringRef(&Line, L'(', FALSE);
    close = PhFindLastCharInStringRef(&Line, L')', FALSE);

    if (open == SIZE_MAX || close == SIZE_MAX || close < open || open == 0)
        return NULL;

    pidPart.Buffer = Line.Buffer;
    pidPart.Length = (open - 1) * sizeof(WCHAR);
    name.Buffer = Line.Buffer + open + 1;
    name.Length = (close - open - 1) * sizeof(WCHAR);

    if (!PhStringToUInt64(&pidPart, 10, &pid))
        return NULL;

    // Skip ") " to reach the state field.
    if (Line.Length < (close + 2) * sizeof(WCHAR))
        return NULL;

    remaining.Buffer = Line.Buffer + close + 2;
    remaining.Length = Line.Length - (close + 2) * sizeof(WCHAR);

    for (ULONG i = 0; i < WSL_STAT_FIELD_COUNT && remaining.Length != 0; i++)
    {
        PH_STRINGREF field;

        PhSplitStringRefAtChar(&remaining, L' ', &field, &remaining);

        if (i == WSL_STAT_STATE)
            state = field.Length != 0 ? field.Buffer[0] : 0;
        else if (!PhStringToUInt64(&field, 10, &fields[i]))
            return NULL;
    }

    entry = PhAllocateZero(sizeof(WSL_STAT_ENTRY));
    entry->Process.ProcessId = (ULONG)pid;
    entry->Process.ParentProcessId = (ULONG)fields[WSL_STAT_PPID];
    entry->Process.StartTime = fields[WSL_STAT_STARTTIME];
    entry->Process.Name = PhCreateString2(&name);
    entry->Process.State = state;
    entry->Process.NumberOfThreads = (ULONG)fields[WSL_STAT_THREADS];
    entry->Process.ResidentBytes = fields[WSL_STAT_RSS] * Parser->PageSize;
    entry->Ticks = fields[WSL_STAT_UTIME] + fields[WSL_STAT_STIME];
    entry->ChildTicks = fields[WSL_STAT_CUTIME] + fields[WSL_STAT_CSTIME];

    return entry;
}

/**
 * Finds a parsed entry by process ID.
 */
static PWSL_STAT_ENTRY WslpFindEntry(
    _In_ PPH_LIST Entries,
    _In_ ULONG ProcessId
    )
{
    for (ULONG i = 0; i < Entries->Count; i++)
    {
        PWSL_STAT_ENTRY entry = Entries->Items[i];

        if (entry->Process.ProcessId == ProcessId)
            return entry;
    }

    return NULL;
}

/**
 * Determines whether an entry belongs to the collector itself: its shell, the shell's
 * children (cat, sleep), and the Relay and SessionLeader processes WSL creates for the
 * collector's wsl.exe session.
 */
static BOOLEAN WslpIsCollectorEntry(
    _In_ PWSL_FRAME_PARSER Parser,
    _In_ PWSL_STAT_ENTRY Entry,
    _In_ ULONG RelayProcessId,
    _In_ ULONG SessionLeaderProcessId
    )
{
    ULONG pid = Entry->Process.ProcessId;

    return pid == Parser->SelfProcessId ||
        Entry->Process.ParentProcessId == Parser->SelfProcessId ||
        (RelayProcessId && pid == RelayProcessId) ||
        (SessionLeaderProcessId && pid == SessionLeaderProcessId);
}

/**
 * Turns the parsed entries of a complete frame into a frame with CPU usage.
 *
 * \remarks CPU usage is expressed as a fraction of all host processors, the unit of the
 * VM row and of every Windows process, so the numbers can be compared. A vCPU second is
 * counted as a host CPU second, which is close but not exact. The distribution total is
 * the change of the sum of utime + stime + cutime + cstime over live processes: when a
 * process exits, its time moves into its parent's cutime, so exited processes stay counted.
 * \return The frame. The caller owns the reference.
 */
static PWSL_PROCESS_FRAME WslpCompleteFrame(
    _In_ PWSL_FRAME_PARSER Parser
    )
{
    PWSL_PROCESS_FRAME frame;
    PPH_HASHTABLE samples;
    PWSL_STAT_ENTRY self;
    PWSL_STAT_ENTRY relay;
    ULONG relayProcessId = 0;
    ULONG sessionLeaderProcessId = 0;
    ULONG64 totalTicks = 0;
    DOUBLE capacity = 0; // Host CPU ticks available since the previous frame

    // The shell's parent is its Relay, and the Relay's parent is the session's SessionLeader.
    // Both are checked by name, so an unexpected tree never hides an unrelated process.
    if (self = WslpFindEntry(Parser->Entries, Parser->SelfProcessId))
    {
        static CONST PH_STRINGREF relayPrefix = PH_STRINGREF_INIT(L"Relay(");
        static CONST PH_STRINGREF sessionLeaderName = PH_STRINGREF_INIT(L"SessionLeader");
        PWSL_STAT_ENTRY sessionLeader;

        if ((relay = WslpFindEntry(Parser->Entries, self->Process.ParentProcessId)) &&
            PhStartsWithStringRef(&relay->Process.Name->sr, &relayPrefix, FALSE))
        {
            relayProcessId = relay->Process.ProcessId;

            if ((sessionLeader = WslpFindEntry(Parser->Entries, relay->Process.ParentProcessId)) &&
                PhEqualStringRef(&sessionLeader->Process.Name->sr, &sessionLeaderName, FALSE))
            {
                sessionLeaderProcessId = sessionLeader->Process.ProcessId;
            }
        }
    }

    if (Parser->HavePrevious && Parser->Uptime > Parser->PreviousUptime)
    {
        capacity = (Parser->Uptime - Parser->PreviousUptime) *
            (DOUBLE)Parser->TicksPerSecond * WslGetHostProcessorCount();
    }

    // PhCreateObject does not zero the object.
    frame = PhCreateObject(sizeof(WSL_PROCESS_FRAME), WslpProcessFrameType);
    memset(frame, 0, sizeof(WSL_PROCESS_FRAME));
    frame->Processes = PhCreateList(Parser->Entries->Count);
    frame->Uptime = Parser->Uptime;
    frame->TicksPerSecond = Parser->TicksPerSecond;
    frame->MemoryTotal = Parser->MemoryTotal;
    frame->MemoryAvailable = Parser->MemoryAvailable;
    PhSetReference(&frame->KernelRelease, Parser->KernelRelease);
    PhSetReference(&frame->EngineId, Parser->EngineId);
    samples = PhCreateHashtable(sizeof(WSL_CPU_SAMPLE), WslpCpuSampleEqualFunction, WslpCpuSampleHashFunction, Parser->Entries->Count);

    for (ULONG i = 0; i < Parser->Entries->Count; i++)
    {
        PWSL_STAT_ENTRY entry = Parser->Entries->Items[i];
        PWSL_LINUX_PROCESS process;
        WSL_CPU_SAMPLE sample;
        PPH_STRING containerId;

        if (WslpIsCollectorEntry(Parser, entry, relayProcessId, sessionLeaderProcessId))
        {
            PhClearReference(&entry->Process.Name);
            PhClearReference(&entry->Process.ContainerId);
            continue;
        }

        totalTicks += entry->Ticks + entry->ChildTicks;

        if (capacity > 0)
        {
            PWSL_CPU_SAMPLE previous;
            WSL_CPU_SAMPLE lookup;

            lookup.ProcessId = entry->Process.ProcessId;
            previous = PhFindEntryHashtable(Parser->PreviousSamples, &lookup);

            if (previous && previous->StartTime == entry->Process.StartTime && entry->Ticks >= previous->Ticks)
            {
                entry->Process.CpuUsage = (FLOAT)((entry->Ticks - previous->Ticks) / capacity);
                entry->Process.HaveCpuUsage = TRUE;
            }
            else if (entry->Process.StartTime >= Parser->PreviousUptime * Parser->TicksPerSecond)
            {
                // Started since the previous frame, so all of its time is new.
                entry->Process.CpuUsage = (FLOAT)(entry->Ticks / capacity);
                entry->Process.HaveCpuUsage = TRUE;
            }
        }

        sample.ProcessId = entry->Process.ProcessId;
        sample.StartTime = entry->Process.StartTime;
        sample.Ticks = entry->Ticks;
        PhAddEntryHashtable(samples, &sample);

        // The container of the process, from the cgroup lines read before the stat lines.
        if (containerId = PhFindItemSimpleHashtable2(Parser->Cgroups, UlongToPtr(entry->Process.ProcessId)))
            PhSetReference(&entry->Process.ContainerId, containerId);

        process = PhAllocateCopy(&entry->Process, sizeof(WSL_LINUX_PROCESS));
        PhAddItemList(frame->Processes, process);
        frame->ResidentBytes += process->ResidentBytes;
    }

    // The total can drop when a process exits without being waited for; that time is lost.
    if (capacity > 0)
    {
        frame->CpuUsage = totalTicks > Parser->PreviousTotalTicks ? (FLOAT)((totalTicks - Parser->PreviousTotalTicks) / capacity) : 0;
        frame->CpuUsage = min(frame->CpuUsage, 1.0f);
        frame->HaveCpuUsage = TRUE;
    }

    PhDereferenceObject(Parser->PreviousSamples);
    Parser->PreviousSamples = samples;
    Parser->PreviousTotalTicks = totalTicks;
    Parser->PreviousUptime = Parser->Uptime;
    Parser->HavePrevious = TRUE;

    // Names and container IDs moved into the frame's processes; the entries are no longer needed.
    for (ULONG i = 0; i < Parser->Entries->Count; i++)
        PhFree(Parser->Entries->Items[i]);

    PhClearList(Parser->Entries);

    return frame;
}

/**
 * Discards the entries of an incomplete frame.
 */
static VOID WslpDiscardEntries(
    _In_ PWSL_FRAME_PARSER Parser
    )
{
    for (ULONG i = 0; i < Parser->Entries->Count; i++)
    {
        PWSL_STAT_ENTRY entry = Parser->Entries->Items[i];

        PhClearReference(&entry->Process.Name);
        PhClearReference(&entry->Process.ContainerId);
        PhFree(entry);
    }

    PhClearList(Parser->Entries);

    // The cgroup lines belong to the frame being read too.
    {
        PH_HASHTABLE_ENUM_CONTEXT enumContext;
        PPH_KEY_VALUE_PAIR pair;

        PhBeginEnumHashtable(Parser->Cgroups, &enumContext);

        while (pair = PhNextEnumHashtable(&enumContext))
            PhDereferenceObject(pair->Value);

        PhClearHashtable(Parser->Cgroups);
    }

    Parser->InStats = FALSE;
}

/**
 * Gets the container ID from a cgroup path.
 *
 * \return The ID, or NULL if the process is not in a container.
 * \remarks The ID is 64 hexadecimal digits after "/docker/" (WSLC, and Docker with the
 * cgroupfs driver), "/docker-" (Docker with the systemd driver, "docker-<id>.scope") or
 * "/libpod-" (Podman, "libpod-<id>.scope").
 */
static PPH_STRING WslpGetCgroupContainerId(
    _In_ PH_STRINGREF Cgroup
    )
{
    static CONST PH_STRINGREF prefixes[] =
    {
        PH_STRINGREF_INIT(L"/docker/"),
        PH_STRINGREF_INIT(L"/docker-"),
        PH_STRINGREF_INIT(L"/libpod-"),
    };

    for (ULONG p = 0; p < RTL_NUMBER_OF(prefixes); p++)
    {
        ULONG_PTR index;
        PH_STRINGREF id;
        BOOLEAN valid = TRUE;

        if ((index = PhFindStringInStringRef(&Cgroup, &prefixes[p], FALSE)) == SIZE_MAX)
            continue;

        id.Buffer = Cgroup.Buffer + index + prefixes[p].Length / sizeof(WCHAR);
        id.Length = Cgroup.Length - index * sizeof(WCHAR) - prefixes[p].Length;

        // Anything after the 64 digits is a nested cgroup or ".scope".
        if (id.Length < 64 * sizeof(WCHAR))
            continue;

        id.Length = 64 * sizeof(WCHAR);

        for (ULONG i = 0; i < 64 && valid; i++)
        {
            WCHAR c = id.Buffer[i];

            valid = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
        }

        if (valid)
            return PhCreateString2(&id);
    }

    return NULL;
}

/**
 * Handles one output line of the collector loop or of a snapshot.
 *
 * \return The frame that the line completed, or NULL. The caller owns the reference.
 * \remarks A "%<pid> <cgroup>" line gives the cgroup of a process of the frame. Both scripts
 * print these lines after all stat lines.
 */
static PWSL_PROCESS_FRAME WslpProcessLine(
    _In_ PWSL_FRAME_PARSER Parser,
    _In_ PSTR Buffer,
    _In_ SIZE_T Length
    )
{
    PWSL_PROCESS_FRAME frame = NULL;
    PPH_STRING line;
    PH_STRINGREF lineRef;

    if (Length != 0 && Buffer[Length - 1] == '\r')
        Length--;

    // A line that does not convert is skipped like any other line that does not parse.
    if (!(line = PhConvertUtf8ToUtf16Ex(Buffer, Length)))
        return NULL;

    lineRef = line->sr;

    if (PhEqualStringRef2(&lineRef, L"@end", FALSE))
    {
        if (Parser->InFrame)
            frame = WslpCompleteFrame(Parser);

        Parser->InFrame = FALSE;
    }
    else if (Parser->InFrame && lineRef.Length != 0 && lineRef.Buffer[0] == L'%')
    {
        PH_STRINGREF pidPart;
        PH_STRINGREF cgroup;
        ULONG64 pid;
        PPH_STRING containerId;

        // A process name can contain a line break, so only the lines before the first stat
        // line are cgroup lines; one inside the stat lines could be forged by a name.
        if (!Parser->InStats)
        {
            PhSkipStringRef(&lineRef, sizeof(WCHAR));
            PhSplitStringRefAtChar(&lineRef, L' ', &pidPart, &cgroup);

            if (PhStringToUInt64(&pidPart, 10, &pid) && pid != 0 && pid <= MAXULONG && (containerId = WslpGetCgroupContainerId(cgroup)))
            {
                PVOID *value;

                if (value = PhFindItemSimpleHashtable(Parser->Cgroups, UlongToPtr((ULONG)pid)))
                    PhMoveReference(value, containerId);
                else
                    PhAddItemSimpleHashtable(Parser->Cgroups, UlongToPtr((ULONG)pid), containerId);
            }
        }
    }
    else if (lineRef.Length >= 2 * sizeof(WCHAR) && lineRef.Buffer[0] == L'@' && lineRef.Buffer[1] == L' ')
    {
        WslpDiscardEntries(Parser);
        Parser->InFrame = WslpParseHeader(Parser, lineRef);
    }
    else if (Parser->InFrame)
    {
        PWSL_STAT_ENTRY entry;

        Parser->InStats = TRUE;

        if (entry = WslpParseStatLine(Parser, lineRef))
            PhAddItemList(Parser->Entries, entry);
    }

    PhDereferenceObject(line);

    return frame;
}

/**
 * Creates a frame parser.
 *
 * \return The parser. Free it with WslDestroyFrameParser.
 */
PWSL_FRAME_PARSER WslCreateFrameParser(
    VOID
    )
{
    PWSL_FRAME_PARSER parser;

    parser = PhAllocateZero(sizeof(WSL_FRAME_PARSER));
    parser->PreviousSamples = PhCreateHashtable(sizeof(WSL_CPU_SAMPLE), WslpCpuSampleEqualFunction, WslpCpuSampleHashFunction, 64);
    parser->Entries = PhCreateList(64);
    parser->Cgroups = PhCreateSimpleHashtable(64);

    return parser;
}

/**
 * Frees a frame parser.
 */
VOID WslDestroyFrameParser(
    _In_ PWSL_FRAME_PARSER Parser
    )
{
    WslpDiscardEntries(Parser);
    PhDereferenceObject(Parser->Entries);
    PhDereferenceObject(Parser->Cgroups);
    PhDereferenceObject(Parser->PreviousSamples);
    PhClearReference(&Parser->KernelRelease);
    PhClearReference(&Parser->EngineId);
    PhFree(Parser);
}

/**
 * Parses the complete output of a one-shot snapshot.
 *
 * \param Parser The parser, which keeps the previous snapshot for CPU usage.
 * \param Output The output, one or more complete frames.
 * \return The last complete frame, or NULL. The caller owns the reference.
 */
PWSL_PROCESS_FRAME WslParseFrameOutput(
    _In_ PWSL_FRAME_PARSER Parser,
    _In_ PPH_BYTES Output
    )
{
    PWSL_PROCESS_FRAME lastFrame = NULL;
    SIZE_T lineStart = 0;

    for (SIZE_T i = 0; i <= Output->Length; i++)
    {
        if (i == Output->Length || Output->Buffer[i] == '\n')
        {
            PWSL_PROCESS_FRAME frame;

            if (i > lineStart && (frame = WslpProcessLine(Parser, Output->Buffer + lineStart, i - lineStart)))
                PhMoveReference(&lastFrame, frame);

            lineStart = i + 1;
        }
    }

    return lastFrame;
}

/**
 * Makes a frame the collector's latest.
 *
 * \param Frame The frame. This function takes ownership of the reference.
 */
static VOID WslpPublishFrame(
    _In_ PWSL_COLLECTOR Collector,
    _In_ PWSL_PROCESS_FRAME Frame
    )
{
    PhAcquireQueuedLockExclusive(&Collector->FrameLock);
    PhMoveReference(&Collector->Frame, Frame);
    PhReleaseQueuedLockExclusive(&Collector->FrameLock);

    // Show the first frame and the first CPU usage as soon as they exist; later frames are
    // picked up by the regular refresh.
    if (Collector->FrameCount < 2)
    {
        Collector->FrameCount++;
        WslRefreshProvider();
    }
}

/**
 * Reader thread: reads the collector output until wsl.exe exits.
 */
_Function_class_(USER_THREAD_START_ROUTINE)
static NTSTATUS NTAPI WslpCollectorThread(
    _In_ PVOID Parameter
    )
{
    PWSL_COLLECTOR collector = Parameter;
    PSTR buffer;
    SIZE_T allocatedLength = PAGE_SIZE * 4;
    SIZE_T usedLength = 0;

    buffer = PhAllocate(allocatedLength);

    while (TRUE)
    {
        ULONG bytesRead;
        SIZE_T lineStart = 0;

        if (allocatedLength - usedLength < PAGE_SIZE)
        {
            allocatedLength *= 2;
            buffer = PhReAllocate(buffer, allocatedLength);
        }

        if (ReadAcquire(&collector->Stopping))
            break;

        // Blocks until output arrives; fails once wsl.exe has exited and the pipe is closed,
        // or when WslStopCollector cancels the read.
        if (!NT_SUCCESS(PhReadFile(collector->ReadHandle, buffer + usedLength, PAGE_SIZE, NULL, &bytesRead)) || bytesRead == 0)
            break;

        usedLength += bytesRead;

        for (SIZE_T i = 0; i < usedLength; i++)
        {
            if (buffer[i] == '\n')
            {
                PWSL_PROCESS_FRAME frame;

                if (frame = WslpProcessLine(collector->Parser, buffer + lineStart, i - lineStart))
                    WslpPublishFrame(collector, frame);

                lineStart = i + 1;
            }
        }

        // Keep an incomplete last line for the next read.
        memmove(buffer, buffer + lineStart, usedLength - lineStart);
        usedLength -= lineStart;
    }

    PhFree(buffer);
    WriteRelease(&collector->Running, FALSE);

    return STATUS_SUCCESS;
}

/**
 * Starts collecting the processes of a running distribution.
 *
 * \param DistroName The distribution name.
 * \return The collector, or NULL if wsl.exe could not be started.
 * \remarks This runs wsl.exe with --distribution, which starts the distribution if it is
 * stopped. Callers must only start a collector for a distribution they just saw running.
 * While the collector runs, its wsl.exe session also keeps the distribution from
 * stopping on idle.
 */
PWSL_COLLECTOR WslStartCollector(
    _In_ PPH_STRING DistroName
    )
{
    PWSL_COLLECTOR collector;
    PPH_STRING script;
    PPH_STRING arguments;
    NTSTATUS status;

    if (!WslIsSafeDistroName(DistroName))
        return NULL;

    // The script is one double-quoted argument, so it must not contain double quotes itself.
    // --cd / keeps the shell off the Windows drives that are mounted in the distribution.
    if (!(script = PhFormatString(WSL_PROCESS_SCRIPT, WSL_REFRESH_INTERVAL_MS / 1000)))
        return NULL;

    arguments = PhFormatString(L"--distribution %s --cd / --exec /bin/sh -c \"%s\"", DistroName->Buffer, script->Buffer);
    PhDereferenceObject(script);

    if (!arguments)
        return NULL;

    collector = PhAllocateZero(sizeof(WSL_COLLECTOR));
    PhSetReference(&collector->DistroName, DistroName);
    PhInitializeQueuedLock(&collector->FrameLock);
    collector->Parser = WslCreateFrameParser();
    collector->Running = TRUE;

    status = WslCreateProcess(WslGetWslFileName(), &arguments->sr, &collector->ProcessHandle, &collector->ReadHandle, &collector->JobHandle);
    PhDereferenceObject(arguments);

    if (NT_SUCCESS(status))
        status = PhCreateThreadEx(&collector->ThreadHandle, WslpCollectorThread, collector);

    if (!NT_SUCCESS(status))
    {
        collector->Running = FALSE;
        WslStopCollector(collector);
        return NULL;
    }

    return collector;
}

/**
 * Stops a collector and frees it.
 *
 * \param Collector The collector.
 */
VOID WslStopCollector(
    _In_ PWSL_COLLECTOR Collector
    )
{
    WriteRelease(&Collector->Stopping, TRUE);

    // Ending the job kills wsl.exe and its helpers, which closes every copy of the pipe's write
    // end and so ends the reader thread's blocking read.
    if (Collector->JobHandle)
        NtTerminateJobObject(Collector->JobHandle, STATUS_SUCCESS);

    // Should a copy of the write end survive outside the job, the read would never end on its
    // own, so cancel it. The cancel is repeated because it only affects a read that is already
    // pending; once Stopping is set, the thread does not start another one.
    if (Collector->ThreadHandle)
    {
        LARGE_INTEGER timeout;
        IO_STATUS_BLOCK isb;

        PhTimeoutFromMilliseconds(&timeout, WSL_COLLECTOR_STOP_TIMEOUT_MS);

        while (NtWaitForSingleObject(Collector->ThreadHandle, FALSE, &timeout) != STATUS_WAIT_0)
            NtCancelSynchronousIoFile(Collector->ThreadHandle, NULL, &isb);

        NtClose(Collector->ThreadHandle);
    }

    if (Collector->ProcessHandle)
        NtClose(Collector->ProcessHandle);
    if (Collector->ReadHandle)
        NtClose(Collector->ReadHandle);
    if (Collector->JobHandle)
        NtClose(Collector->JobHandle);

    PhClearReference(&Collector->Frame);
    WslDestroyFrameParser(Collector->Parser);
    PhClearReference(&Collector->DistroName);
    PhFree(Collector);
}

/**
 * Determines whether a collector's wsl.exe is still running.
 *
 * \param Collector The collector.
 * \return FALSE once wsl.exe has exited, e.g. because the distribution was terminated.
 */
BOOLEAN WslIsCollectorRunning(
    _In_ PWSL_COLLECTOR Collector
    )
{
    return !!ReadAcquire(&Collector->Running);
}

/**
 * Gets the latest complete frame of a collector.
 *
 * \param Collector The collector.
 * \return The frame, or NULL before the first frame. The caller owns the reference.
 */
PWSL_PROCESS_FRAME WslReferenceCollectorFrame(
    _In_ PWSL_COLLECTOR Collector
    )
{
    PWSL_PROCESS_FRAME frame;

    PhAcquireQueuedLockShared(&Collector->FrameLock);

    if (frame = Collector->Frame)
        PhReferenceObject(frame);

    PhReleaseQueuedLockShared(&Collector->FrameLock);

    return frame;
}
