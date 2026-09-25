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

static CONST PH_STRINGREF WslpLxssKeyName = PH_STRINGREF_INIT(L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss");
static PPH_OBJECT_TYPE WslpSnapshotType = NULL;
static PPH_OBJECT_TYPE WslpContainerDetailsType = NULL;

// The inspect details of containers, by container, while they keep their state. Only the
// provider thread uses it.
typedef struct _WSL_CONTAINER_DETAILS_ENTRY
{
    PPH_STRING Key;
    PPH_STRING State;
    PWSL_CONTAINER_DETAILS Details;
    ULONG64 LastUsedTime;
} WSL_CONTAINER_DETAILS_ENTRY, *PWSL_CONTAINER_DETAILS_ENTRY;

// A container not seen for this long is forgotten, e.g. after it was removed.
#define WSL_CONTAINER_DETAILS_LIFETIME_MS 60000

static PPH_LIST WslpContainerDetailsCache = NULL; // PWSL_CONTAINER_DETAILS_ENTRY
volatile LONG WslContainerDetailsWanted = FALSE;

_Function_class_(PH_TYPE_DELETE_PROCEDURE)
static VOID NTAPI WslpContainerDetailsDeleteProcedure(
    _In_ PVOID Object,
    _In_ ULONG Flags
    );

typedef struct _WSL_ENUM_DISTRO_CONTEXT
{
    PPH_LIST Distributions;
    PPH_STRING DefaultId;
} WSL_ENUM_DISTRO_CONTEXT, *PWSL_ENUM_DISTRO_CONTEXT;

/**
 * Determines whether the current user has WSL registered.
 *
 * \return TRUE if the per-user Lxss registry key exists, FALSE otherwise.
 * \remarks Only the registry is read, so this never starts the WSL service or its virtual machine.
 */
BOOLEAN WslIsInstalled(
    VOID
    )
{
    HANDLE keyHandle;

    if (NT_SUCCESS(PhOpenKey(&keyHandle, KEY_READ, PH_KEY_CURRENT_USER, &WslpLxssKeyName, 0)))
    {
        NtClose(keyHandle);
        return TRUE;
    }

    return FALSE;
}

/**
 * Frees the distributions owned by a snapshot.
 *
 * \param Object The snapshot object.
 * \param Flags Unused.
 */
_Function_class_(PH_TYPE_DELETE_PROCEDURE)
VOID NTAPI WslpSnapshotDeleteProcedure(
    _In_ PVOID Object,
    _In_ ULONG Flags
    )
{
    PWSL_SNAPSHOT snapshot = Object;

    for (ULONG i = 0; i < snapshot->Distributions->Count; i++)
    {
        PWSL_DISTRO_ITEM distro = snapshot->Distributions->Items[i];

        PhClearReference(&distro->Id);
        PhClearReference(&distro->Name);
        PhClearReference(&distro->BasePath);
        PhClearReference(&distro->VhdFileName);
        PhClearReference(&distro->OsName);
        PhClearReference(&distro->Processes);
        PhFree(distro);
    }

    PhDereferenceObject(snapshot->Distributions);

    if (snapshot->Sessions)
        WslFreeSessions(snapshot->Sessions);
    if (snapshot->Engines)
        WslFreeEngines(snapshot->Engines);
}

/**
 * Creates the object type used for snapshots. Must be called once before WslQuerySnapshot.
 */
VOID WslInitializeSnapshotType(
    VOID
    )
{
    WslpSnapshotType = PhCreateObjectType(L"WslSnapshot", 0, WslpSnapshotDeleteProcedure);
    WslpContainerDetailsType = PhCreateObjectType(L"WslContainerDetails", 0, WslpContainerDetailsDeleteProcedure);
}

/**
 * Converts a registry BasePath value to a Win32 path.
 *
 * \param BasePath The BasePath value, which WSL stores with a "\\?\" prefix.
 * \return The path without the prefix. The caller owns the returned string.
 */
static PPH_STRING WslpNormalizeBasePath(
    _In_ PPH_STRING BasePath
    )
{
    static CONST PH_STRINGREF prefix = PH_STRINGREF_INIT(L"\\\\?\\");
    PH_STRINGREF remaining = BasePath->sr;

    // Only strip the prefix from drive paths; "\\?\UNC\" has no plain Win32 form here.
    if (PhStartsWithStringRef(&remaining, &prefix, FALSE) && remaining.Length >= prefix.Length + 2 * sizeof(WCHAR))
    {
        PhSkipStringRef(&remaining, prefix.Length);

        if (remaining.Buffer[1] == L':')
            return PhCreateString2(&remaining);
    }

    return PhReferenceObject(BasePath);
}

/**
 * Reads one distribution subkey of the Lxss key into a distribution item.
 */
_Function_class_(PH_ENUM_KEY_CALLBACK)
static BOOLEAN NTAPI WslpEnumerateDistroCallback(
    _In_ HANDLE RootDirectory,
    _In_ PVOID Information,
    _In_opt_ PVOID Context
    )
{
    PWSL_ENUM_DISTRO_CONTEXT context = Context;
    PKEY_BASIC_INFORMATION information = Information;
    PH_STRINGREF keyName;
    HANDLE keyHandle;
    PPH_STRING name;
    PPH_STRING basePath;
    PWSL_DISTRO_ITEM distro;

    if (!context)
        return FALSE;

    keyName.Buffer = information->Name;
    keyName.Length = information->NameLength;

    // Skip a subkey we cannot read instead of failing the whole enumeration.
    if (!NT_SUCCESS(PhOpenKey(&keyHandle, KEY_QUERY_VALUE, RootDirectory, &keyName, 0)))
        return TRUE;

    name = PhQueryRegistryStringZ(keyHandle, L"DistributionName");

    // A subkey without a name is an interrupted registration, not a usable distribution.
    if (PhIsNullOrEmptyString(name))
    {
        PhClearReference(&name);
        NtClose(keyHandle);
        return TRUE;
    }

    distro = PhAllocateZero(sizeof(WSL_DISTRO_ITEM));
    distro->Id = PhCreateString2(&keyName);
    distro->Name = name;
    distro->Version = PhQueryRegistryUlongZ(keyHandle, L"Version");
    distro->Default = context->DefaultId && PhEqualStringRef(&context->DefaultId->sr, &keyName, TRUE);
    distro->State = WslDistroStateUnknown;

    // Flavor is lower case, e.g. "ubuntu"; show it the way the distribution names itself.
    {
        PPH_STRING flavor = PhQueryRegistryStringZ(keyHandle, L"Flavor");
        PPH_STRING osVersion = PhQueryRegistryStringZ(keyHandle, L"OsVersion");

        if (!PhIsNullOrEmptyString(flavor))
        {
            static CONST PH_STRINGREF space = PH_STRINGREF_INIT(L" ");

            distro->OsName = PhIsNullOrEmptyString(osVersion) ? PhDuplicateString(flavor) : PhConcatStringRef3(&flavor->sr, &space, &osVersion->sr);
            distro->OsName->Buffer[0] = RtlUpcaseUnicodeChar(distro->OsName->Buffer[0]);
        }

        PhClearReference(&flavor);
        PhClearReference(&osVersion);
    }

    if (basePath = PhQueryRegistryStringZ(keyHandle, L"BasePath"))
    {
        PhMoveReference(&distro->BasePath, WslpNormalizeBasePath(basePath));
        PhDereferenceObject(basePath);
    }

    if (distro->Version == 2 && distro->BasePath)
    {
        static CONST PH_STRINGREF separator = PH_STRINGREF_INIT(L"\\");
        PPH_STRING vhdName;

        if (!PhIsNullOrEmptyString(vhdName = PhQueryRegistryStringZ(keyHandle, L"VhdFileName")))
        {
            FILE_NETWORK_OPEN_INFORMATION fileInformation;

            distro->VhdFileName = PhConcatStringRef3(&distro->BasePath->sr, &separator, &vhdName->sr);

            // Reading file attributes does not open the disk for I/O, so this is safe while the VM has it attached.
            if (NT_SUCCESS(PhQueryFullAttributesFileWin32(distro->VhdFileName->Buffer, &fileInformation)))
                distro->VhdSize = fileInformation.EndOfFile.QuadPart;
        }

        PhClearReference(&vhdName);
    }

    NtClose(keyHandle);

    PhAddItemList(context->Distributions, distro);

    return TRUE;
}

/**
 * Marks each distribution as running or stopped from the output of "wsl.exe --list --running --quiet".
 *
 * \param Distributions The distributions to update.
 * \param Output One distribution name per line.
 */
static VOID WslpApplyRunningList(
    _In_ PPH_LIST Distributions,
    _In_ PPH_STRING Output
    )
{
    static CONST PH_STRINGREF whitespace = PH_STRINGREF_INIT(L" \t\r");
    PH_STRINGREF remaining = Output->sr;

    for (ULONG i = 0; i < Distributions->Count; i++)
        ((PWSL_DISTRO_ITEM)Distributions->Items[i])->State = WslDistroStateStopped;

    while (remaining.Length != 0)
    {
        PH_STRINGREF line;

        PhSplitStringRefAtChar(&remaining, L'\n', &line, &remaining);
        PhTrimStringRef(&line, &whitespace, 0);

        if (line.Length == 0)
            continue;

        for (ULONG i = 0; i < Distributions->Count; i++)
        {
            PWSL_DISTRO_ITEM distro = Distributions->Items[i];

            // WSL treats distribution names case-insensitively.
            if (PhEqualStringRef(&distro->Name->sr, &line, TRUE))
            {
                distro->State = WslDistroStateRunning;
                break;
            }
        }
    }
}

/**
 * Builds a snapshot of the registered distributions and their state.
 *
 * \param VmRunning TRUE if a WSL virtual machine process exists.
 * \return A new snapshot. The caller owns the reference.
 * \remarks The distribution list comes from the registry. wsl.exe is asked for running
 * distributions only when something can be running: a WSL 2 distribution needs the VM,
 * and a WSL 1 distribution needs no VM at all. In every other case each distribution is
 * reported as stopped without starting anything.
 */
PWSL_SNAPSHOT WslQuerySnapshot(
    _In_ BOOLEAN VmRunning
    )
{
    static CONST PH_STRINGREF runningArguments = PH_STRINGREF_INIT(L"--list --running --quiet");
    PWSL_SNAPSHOT snapshot;
    WSL_ENUM_DISTRO_CONTEXT context;
    HANDLE keyHandle;
    BOOLEAN queryRunning;

    snapshot = PhCreateObject(sizeof(WSL_SNAPSHOT), WslpSnapshotType);
    snapshot->Distributions = PhCreateList(4);
    snapshot->RunningQueryStatus = STATUS_SUCCESS;
    snapshot->Sessions = NULL;
    snapshot->Engines = NULL;

    context.Distributions = snapshot->Distributions;
    context.DefaultId = NULL;

    if (NT_SUCCESS(PhOpenKey(&keyHandle, KEY_READ, PH_KEY_CURRENT_USER, &WslpLxssKeyName, 0)))
    {
        context.DefaultId = PhQueryRegistryStringZ(keyHandle, L"DefaultDistribution");
        PhEnumerateKey(keyHandle, KeyBasicInformation, WslpEnumerateDistroCallback, &context);
        NtClose(keyHandle);
    }

    PhClearReference(&context.DefaultId);

    queryRunning = VmRunning;

    for (ULONG i = 0; i < snapshot->Distributions->Count && !queryRunning; i++)
    {
        if (((PWSL_DISTRO_ITEM)snapshot->Distributions->Items[i])->Version == 1)
            queryRunning = TRUE;
    }

    if (queryRunning)
    {
        PPH_BYTES output;

        snapshot->RunningQueryStatus = WslRunCommand(WslGetWslFileName(), &runningArguments, &output);

        if (NT_SUCCESS(snapshot->RunningQueryStatus))
        {
            PPH_STRING text = PhConvertUtf8ToUtf16Ex(output->Buffer, output->Length);

            if (text)
            {
                WslpApplyRunningList(snapshot->Distributions, text);
                PhDereferenceObject(text);
            }

            PhDereferenceObject(output);
        }
    }
    else
    {
        for (ULONG i = 0; i < snapshot->Distributions->Count; i++)
            ((PWSL_DISTRO_ITEM)snapshot->Distributions->Items[i])->State = WslDistroStateStopped;
    }

    return snapshot;
}

/**
 * Gets the display text for a distribution state.
 *
 * \param State The distribution state.
 * \return The display text.
 */
PCPH_STRINGREF WslGetDistroStateText(
    _In_ WSL_DISTRO_STATE State
    )
{
    static CONST PH_STRINGREF unknownText = PH_STRINGREF_INIT(L"Unknown");
    static CONST PH_STRINGREF stoppedText = PH_STRINGREF_INIT(L"Stopped");
    static CONST PH_STRINGREF runningText = PH_STRINGREF_INIT(L"Running");

    switch (State)
    {
    case WslDistroStateStopped:
        return &stoppedText;
    case WslDistroStateRunning:
        return &runningText;
    default:
        return &unknownText;
    }
}

/**
 * Formats a file version for display, e.g. "2.9.12" for "2.9.12.0".
 *
 * \param Version The file version.
 * \return The version without a zero fourth part; "2.9.12.1" stays as it is. The caller owns
 * the string.
 */
PPH_STRING WslFormatDisplayVersion(
    _In_ PCPH_STRINGREF Version
    )
{
    static CONST PH_STRINGREF zeroRevision = PH_STRINGREF_INIT(L".0");
    PH_STRINGREF text = *Version;
    ULONG dots = 0;

    for (SIZE_T i = 0; i < text.Length / sizeof(WCHAR); i++)
        dots += text.Buffer[i] == L'.';

    if (dots == 3 && PhEndsWithStringRef(&text, &zeroRevision, FALSE))
        text.Length -= zeroRevision.Length;

    return PhCreateString2(&text);
}

/**
 * Gets the version of a file for display, e.g. "2.9.12" for a file version of "2.9.12.0".
 *
 * \param FileName The file, with environment variables.
 * \return The version, or NULL if the file has none. The caller owns the string.
 */
static PPH_STRING WslpGetFileDisplayVersion(
    _In_ PCPH_STRINGREF FileName
    )
{
    PPH_STRING fileName;
    PPH_STRING version = NULL;
    PH_IMAGE_VERSION_INFO versionInfo;

    if (!(fileName = PhExpandEnvironmentStrings(FileName)))
        return NULL;

    if (NT_SUCCESS(PhInitializeImageVersionInfo(&versionInfo, fileName->Buffer)))
    {
        if (!PhIsNullOrEmptyString(versionInfo.FileVersion))
            version = WslFormatDisplayVersion(&versionInfo.FileVersion->sr);

        PhDeleteImageVersionInfo(&versionInfo);
    }

    PhDereferenceObject(fileName);

    return version;
}

/**
 * Gets the version of WSL, from wslservice.exe, e.g. "2.9.12".
 *
 * \return The version, or NULL if it cannot be read. The string is cached; do not free it.
 * \remarks The in-box wsl.exe of Windows 10 has the Windows version, not the WSL version.
 */
PPH_STRING WslGetWslVersion(
    VOID
    )
{
    static PH_INITONCE initOnce = PH_INITONCE_INIT;
    static PPH_STRING version = NULL;

    if (PhBeginInitOnce(&initOnce))
    {
        static CONST PH_STRINGREF path = PH_STRINGREF_INIT(L"%ProgramW6432%\\WSL\\wslservice.exe");

        version = WslpGetFileDisplayVersion(&path);
        PhEndInitOnce(&initOnce);
    }

    return version;
}

/**
 * Gets the version of wslc.exe, e.g. "2.9.12".
 *
 * \return The version, or NULL if wslc.exe is missing. The string is cached; do not free it.
 */
PPH_STRING WslGetWslcVersion(
    VOID
    )
{
    static PH_INITONCE initOnce = PH_INITONCE_INIT;
    static PPH_STRING version = NULL;

    if (PhBeginInitOnce(&initOnce))
    {
        static CONST PH_STRINGREF path = PH_STRINGREF_INIT(L"%ProgramW6432%\\WSL\\wslc.exe");

        version = WslpGetFileDisplayVersion(&path);
        PhEndInitOnce(&initOnce);
    }

    return version;
}

/**
 * Gets the full path of wsl.exe.
 *
 * \return The path. The string is cached for the lifetime of the process.
 */
PPH_STRING WslGetWslFileName(
    VOID
    )
{
    static PH_INITONCE initOnce = PH_INITONCE_INIT;
    static PPH_STRING fileName = NULL;

    if (PhBeginInitOnce(&initOnce))
    {
        static CONST PH_STRINGREF system32Path = PH_STRINGREF_INIT(L"\\System32\\wsl.exe");
        static CONST PH_STRINGREF sysnativePath = PH_STRINGREF_INIT(L"\\Sysnative\\wsl.exe");
        PH_STRINGREF systemRoot;

        PhGetSystemRoot(&systemRoot);

        // wsl.exe is 64-bit only; a 32-bit build has to bypass the System32 redirection.
        fileName = PhConcatStringRef2(&systemRoot, PhIsExecutingInWow64() ? &sysnativePath : &system32Path);

        PhEndInitOnce(&initOnce);
    }

    return fileName;
}

/**
 * Gets the full path of wslc.exe, the WSL container CLI.
 *
 * \return The path, or NULL if this WSL version has no wslc.exe. The result is cached for
 * the lifetime of the process.
 */
PPH_STRING WslGetWslcFileName(
    VOID
    )
{
    static PH_INITONCE initOnce = PH_INITONCE_INIT;
    static PPH_STRING fileName = NULL;

    if (PhBeginInitOnce(&initOnce))
    {
        // wslc.exe is 64-bit only and lives in the native Program Files, which a 32-bit
        // build can only name through ProgramW6432.
        static CONST PH_STRINGREF path = PH_STRINGREF_INIT(L"%ProgramW6432%\\WSL\\wslc.exe");
        PPH_STRING expanded;

        if (expanded = PhExpandEnvironmentStrings(&path))
        {
            if (PhDoesFileExistWin32(expanded->Buffer))
                fileName = expanded;
            else
                PhDereferenceObject(expanded);
        }

        PhEndInitOnce(&initOnce);
    }

    return fileName;
}

/**
 * Starts wsl.exe or wslc.exe hidden, with its stdout and stderr connected to a pipe.
 *
 * \param FileName The executable, from WslGetWslFileName or WslGetWslcFileName.
 * \param Arguments The command line arguments, without the executable name.
 * \param ProcessHandle Receives a handle to the wsl.exe process.
 * \param ReadHandle Receives the read end of the output pipe. It reports end-of-file once
 * wsl.exe exits.
 * \param JobHandle Receives a kill-on-close job that contains wsl.exe and the helper processes
 * it starts. Terminating or closing the job ends all of them, also if System Informer exits.
 * \return NTSTATUS code indicating success or failure.
 */
NTSTATUS WslCreateProcess(
    _In_ PPH_STRING FileName,
    _In_ PCPH_STRINGREF Arguments,
    _Out_ PHANDLE ProcessHandle,
    _Out_ PHANDLE ReadHandle,
    _Out_ PHANDLE JobHandle
    )
{
    static CONST PH_STRINGREF quote = PH_STRINGREF_INIT(L"\"");
    static CONST PH_STRINGREF quoteSpace = PH_STRINGREF_INIT(L"\" ");
    static UNICODE_STRING utf8Name = RTL_CONSTANT_STRING(L"WSL_UTF8");
    static UNICODE_STRING utf8Value = RTL_CONSTANT_STRING(L"1");
    NTSTATUS status;
    PPH_STRING commandLine;
    PVOID environment = NULL;
    HANDLE readHandle = NULL;
    HANDLE writeHandle = NULL;
    HANDLE inputHandle = NULL;
    HANDLE inputWriteHandle = NULL;
    HANDLE handleList[2];
    HANDLE processHandle = NULL;
    HANDLE threadHandle = NULL;
    HANDLE jobHandle = NULL;
    PPROC_THREAD_ATTRIBUTE_LIST attributeList = NULL;
    STARTUPINFOEX startupInfo;
    OBJECT_HANDLE_FLAG_INFORMATION handleFlags;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobLimits;

    commandLine = PhConcatStringRef3(&quote, &FileName->sr, &quoteSpace);
    PhMoveReference(&commandLine, PhConcatStringRef2(&commandLine->sr, Arguments));

    // Without WSL_UTF8 wsl.exe writes UTF-16 to a pipe, unless the user has set it globally.
    // Setting it explicitly makes the output encoding predictable.
    if (!NT_SUCCESS(status = RtlCreateEnvironment(TRUE, &environment)))
        goto CleanupExit;
    if (!NT_SUCCESS(status = RtlSetEnvironmentVariable(&environment, &utf8Name, &utf8Value)))
        goto CleanupExit;

    // wsl.exe starts a second wsl.exe and a wslhost.exe; the job ends them together.
    if (!NT_SUCCESS(status = PhCreateJobObject(&jobHandle, JOB_OBJECT_ALL_ACCESS, NULL, NULL)))
        goto CleanupExit;

    memset(&jobLimits, 0, sizeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION));
    jobLimits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

    if (!NT_SUCCESS(status = NtSetInformationJobObject(jobHandle, JobObjectExtendedLimitInformation, &jobLimits, sizeof(jobLimits))))
        goto CleanupExit;

    if (!NT_SUCCESS(status = PhCreatePipe(&readHandle, &writeHandle)))
        goto CleanupExit;

    // Only the pipe's write end and the input handle are inherited, and the handle list keeps
    // any other inheritable handle in this process out of the child.
    handleFlags.Inherit = TRUE;
    handleFlags.ProtectFromClose = FALSE;

    if (!NT_SUCCESS(status = NtSetInformationObject(writeHandle, ObjectHandleFlagInformation, &handleFlags, sizeof(handleFlags))))
        goto CleanupExit;

    // stdin is a pipe that is already at end-of-file: nothing here ever writes to a tool's
    // input. "wslc system session run" needs a valid input handle, and it must not be the NUL
    // device: with NUL as stdin, session run fails with ERROR_INVALID_HANDLE part way through
    // a larger output.
    if (!NT_SUCCESS(status = PhCreatePipe(&inputHandle, &inputWriteHandle)))
        goto CleanupExit;

    NtClose(inputWriteHandle);
    inputWriteHandle = NULL;

    if (!NT_SUCCESS(status = NtSetInformationObject(inputHandle, ObjectHandleFlagInformation, &handleFlags, sizeof(handleFlags))))
        goto CleanupExit;

    handleList[0] = writeHandle;
    handleList[1] = inputHandle;

    if (!NT_SUCCESS(status = PhInitializeProcThreadAttributeList(&attributeList, 1)))
        goto CleanupExit;
    if (!NT_SUCCESS(status = PhUpdateProcThreadAttribute(attributeList, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handleList, sizeof(handleList))))
        goto CleanupExit;

    memset(&startupInfo, 0, sizeof(STARTUPINFOEX));
    startupInfo.StartupInfo.cb = sizeof(STARTUPINFOEX);
    startupInfo.StartupInfo.dwFlags = STARTF_USESHOWWINDOW | STARTF_FORCEOFFFEEDBACK | STARTF_USESTDHANDLES;
    startupInfo.StartupInfo.wShowWindow = SW_HIDE;
    startupInfo.StartupInfo.hStdInput = inputHandle;
    startupInfo.StartupInfo.hStdOutput = writeHandle;
    startupInfo.StartupInfo.hStdError = writeHandle;
    startupInfo.lpAttributeList = attributeList;

    status = PhCreateProcessWin32Ex(
        FileName->Buffer,
        commandLine->Buffer,
        environment,
        NULL,
        &startupInfo,
        PH_CREATE_PROCESS_INHERIT_HANDLES | PH_CREATE_PROCESS_UNICODE_ENVIRONMENT | PH_CREATE_PROCESS_NEW_CONSOLE |
        PH_CREATE_PROCESS_DEFAULT_ERROR_MODE | PH_CREATE_PROCESS_EXTENDED_STARTUPINFO | PH_CREATE_PROCESS_SUSPENDED,
        NULL,
        NULL,
        &processHandle,
        &threadHandle
        );

    if (!NT_SUCCESS(status))
        goto CleanupExit;

    // Assign before the first instruction runs, so every process wsl.exe starts is in the job.
    if (!NT_SUCCESS(status = NtAssignProcessToJobObject(jobHandle, processHandle)))
    {
        PhTerminateProcess(processHandle, status);
        goto CleanupExit;
    }

    PhResumeThread(threadHandle, NULL);

    *ProcessHandle = processHandle;
    *ReadHandle = readHandle;
    *JobHandle = jobHandle;
    processHandle = NULL;
    readHandle = NULL;
    jobHandle = NULL;

CleanupExit:
    if (threadHandle)
        NtClose(threadHandle);
    if (processHandle)
        NtClose(processHandle);
    if (jobHandle)
        NtClose(jobHandle);
    // Our copy of the write end is always closed, so the read end reports end-of-file once wsl.exe exits.
    if (writeHandle)
        NtClose(writeHandle);
    if (inputHandle)
        NtClose(inputHandle);
    if (readHandle)
        NtClose(readHandle);
    // PhDeleteProcThreadAttributeList is not exported; the list is a PhAllocateZero allocation.
    if (attributeList)
        PhFree(attributeList);
    if (environment)
        RtlDestroyEnvironment(environment);

    PhDereferenceObject(commandLine);

    return status;
}

/**
 * Runs wsl.exe or wslc.exe hidden and captures its output.
 *
 * \param FileName The executable, from WslGetWslFileName or WslGetWslcFileName.
 * \param Arguments The command line arguments, without the executable name.
 * \param Output Receives the combined stdout and stderr text. The caller owns the string.
 * \return STATUS_SUCCESS if wsl.exe exited with code 0, STATUS_IO_TIMEOUT if it was killed
 * after WSL_COMMAND_TIMEOUT_MS, or another error status.
 * \remarks Must not be called on the GUI thread; it blocks until wsl.exe exits.
 */
NTSTATUS WslRunCommand(
    _In_ PPH_STRING FileName,
    _In_ PCPH_STRINGREF Arguments,
    _Out_opt_ PPH_BYTES *Output
    )
{
    return WslRunCommandEx(FileName, Arguments, WSL_COMMAND_TIMEOUT_MS, Output, FALSE);
}

/**
 * Runs wsl.exe or wslc.exe hidden and captures its output.
 *
 * \param FileName The executable, from WslGetWslFileName or WslGetWslcFileName.
 * \param Arguments The command line arguments, without the executable name.
 * \param TimeoutMs How long the tool may run before it is killed.
 * \param Output Receives the combined stdout and stderr text, or NULL. The caller owns the string.
 * \param OutputOnFailure TRUE to also return the output when the tool exits with a non-zero code
 * (STATUS_UNSUCCESSFUL), so its error message can be shown.
 * \return STATUS_SUCCESS if wsl.exe exited with code 0, STATUS_UNSUCCESSFUL if it exited with
 * another code, STATUS_IO_TIMEOUT if it was killed after TimeoutMs, or another error status.
 * \remarks Must not be called on the GUI thread; it blocks until wsl.exe exits.
 */
NTSTATUS WslRunCommandEx(
    _In_ PPH_STRING FileName,
    _In_ PCPH_STRINGREF Arguments,
    _In_ ULONG TimeoutMs,
    _Out_opt_ PPH_BYTES *Output,
    _In_ BOOLEAN OutputOnFailure
    )
{
    NTSTATUS status;
    HANDLE processHandle;
    HANDLE readHandle;
    HANDLE jobHandle;
    PH_BYTES_BUILDER bytesBuilder;
    LARGE_INTEGER timeout;
    ULONG64 startTickCount;
    PROCESS_BASIC_INFORMATION basicInfo;

    if (Output)
        *Output = NULL;

    if (!NT_SUCCESS(status = WslCreateProcess(FileName, Arguments, &processHandle, &readHandle, &jobHandle)))
        return status;

    PhInitializeBytesBuilder(&bytesBuilder, 256);

    // Drain the pipe while waiting, so wsl.exe never blocks on a full pipe buffer, and give up
    // after the timeout so a hung WSL service cannot hang the caller.
    startTickCount = NtGetTickCount64();
    PhTimeoutFromMilliseconds(&timeout, 50);

    while (TRUE)
    {
        ULONG bytesAvailable = 0;
        BOOLEAN exited;

        exited = NtWaitForSingleObject(processHandle, FALSE, &timeout) == STATUS_WAIT_0;

        while (NT_SUCCESS(PhPeekNamedPipe(readHandle, NULL, 0, NULL, &bytesAvailable, NULL)) && bytesAvailable != 0)
        {
            UCHAR buffer[PAGE_SIZE];
            ULONG bytesRead;

            if (!NT_SUCCESS(PhReadFile(readHandle, buffer, min(bytesAvailable, sizeof(buffer)), NULL, &bytesRead)) || bytesRead == 0)
                break;

            PhAppendBytesBuilderEx(&bytesBuilder, buffer, bytesRead, 0, NULL);
            bytesAvailable = 0;
        }

        if (exited)
            break;

        if (NtGetTickCount64() - startTickCount >= TimeoutMs)
        {
            NtTerminateJobObject(jobHandle, STATUS_IO_TIMEOUT);
            status = STATUS_IO_TIMEOUT;
            goto CleanupExit;
        }
    }

    if (!NT_SUCCESS(status = PhGetProcessBasicInformation(processHandle, &basicInfo)))
        goto CleanupExit;

    status = basicInfo.ExitStatus == 0 ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;

CleanupExit:
    // Both tools write UTF-8: wsl.exe because WslCreateProcess sets WSL_UTF8, wslc.exe always.
    if (Output && (NT_SUCCESS(status) || (OutputOnFailure && status == STATUS_UNSUCCESSFUL)))
        *Output = PhFinalBytesBuilderBytes(&bytesBuilder);
    else
        PhDeleteBytesBuilder(&bytesBuilder);
    NtClose(processHandle);
    NtClose(readHandle);
    NtClose(jobHandle);

    return status;
}

/**
 * Determines whether a distribution name can be passed to wsl.exe as a plain argument.
 *
 * \param Name The distribution name.
 * \return TRUE if the name has only letters, digits, '.', '_' and '-'.
 * \remarks wsl.exe does not remove quotes from a distribution name, so names are passed
 * unquoted, and this check keeps a name from splitting the command line.
 */
BOOLEAN WslIsSafeDistroName(
    _In_ PPH_STRING Name
    )
{
    if (Name->Length == 0)
        return FALSE;

    for (SIZE_T i = 0; i < Name->Length / sizeof(WCHAR); i++)
    {
        WCHAR c = Name->Buffer[i];

        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'.' || c == L'_' || c == L'-'))
            return FALSE;
    }

    return TRUE;
}

/**
 * Opens an interactive shell for a distribution in a new console window.
 *
 * \param DistroName The distribution name.
 * \return NTSTATUS code indicating success or failure.
 * \remarks This starts the distribution if it is stopped. It is only called on an explicit user action.
 */
NTSTATUS WslStartShell(
    _In_ PPH_STRING DistroName
    )
{
    NTSTATUS status;
    PPH_STRING fileName;
    PPH_STRING commandLine;

    if (!WslIsSafeDistroName(DistroName))
        return STATUS_INVALID_PARAMETER;

    fileName = WslGetWslFileName();
    if (!(commandLine = PhFormatString(L"\"%s\" --distribution %s --cd ~", fileName->Buffer, DistroName->Buffer)))
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

_Function_class_(PH_TYPE_DELETE_PROCEDURE)
static VOID NTAPI WslpContainerDetailsDeleteProcedure(
    _In_ PVOID Object,
    _In_ ULONG Flags
    )
{
    PWSL_CONTAINER_DETAILS details = Object;

    PhClearReference(&details->RestartText);
    PhClearReference(&details->ExitText);
    PhClearReference(&details->MemoryLimitText);
    PhClearReference(&details->CpuLimitText);
    PhClearReference(&details->PrivilegedText);
    PhClearReference(&details->User);
    PhClearReference(&details->Platform);
}

/**
 * Formats a system time as local date and time text.
 */
PPH_STRING WslFormatLocalTime(
    _In_ PLARGE_INTEGER Time
    )
{
    SYSTEMTIME systemTime;

    PhLargeIntegerToLocalSystemTime(&systemTime, Time);

    return PhFormatDateTime(&systemTime);
}

/**
 * Reads the details for the optional columns from the inspect output of a container, which
 * Docker API engines and wslc print alike.
 *
 * \param Object One container of the inspect output.
 * \return The details, or NULL if the object is not a container.
 */
PWSL_CONTAINER_DETAILS WslParseContainerDetails(
    _In_ PVOID Object
    )
{
    PWSL_CONTAINER_DETAILS details;
    PVOID hostConfig;
    PVOID state;
    PVOID config;

    if (PhGetJsonObjectType(Object) != PH_JSON_OBJECT_TYPE_OBJECT)
        return NULL;

    details = PhCreateObjectZero(sizeof(WSL_CONTAINER_DETAILS), WslpContainerDetailsType);
    details->RestartCount = PhGetJsonValueAsUlong(Object, "RestartCount");

    if ((hostConfig = PhGetJsonObject(Object, "HostConfig")) && PhGetJsonObjectType(hostConfig) == PH_JSON_OBJECT_TYPE_OBJECT)
    {
        PVOID restartPolicy;
        ULONG64 cpuQuota;
        ULONG64 cpuPeriod;

        if (restartPolicy = PhGetJsonObject(hostConfig, "RestartPolicy"))
        {
            PPH_STRING name = PhGetJsonValueAsString(restartPolicy, "Name");

            // An empty policy is the default, which is not to restart.
            if (PhIsNullOrEmptyString(name))
                PhMoveReference(&name, PhCreateString(L"no"));

            if (details->RestartCount != 0)
                details->RestartText = PhFormatString(L"%s (%lu)", name->Buffer, details->RestartCount);
            else
                PhSetReference(&details->RestartText, name);

            PhDereferenceObject(name);
        }

        if ((details->MemoryLimit = PhGetJsonValueAsUInt64(hostConfig, "Memory")) != 0)
            details->MemoryLimitText = PhFormatSize(details->MemoryLimit, ULONG_MAX);

        // "docker run --cpus" sets NanoCpus; --cpu-quota and --cpu-period set the same limit.
        details->NanoCpus = PhGetJsonValueAsUInt64(hostConfig, "NanoCpus");
        cpuQuota = PhGetJsonValueAsUInt64(hostConfig, "CpuQuota");
        cpuPeriod = PhGetJsonValueAsUInt64(hostConfig, "CpuPeriod");

        if (details->NanoCpus == 0 && cpuQuota != 0 && cpuPeriod != 0)
            details->NanoCpus = cpuQuota * 1000000000ULL / cpuPeriod;

        if (details->NanoCpus != 0)
        {
            PH_FORMAT format;

            PhInitFormatFD(&format, (DOUBLE)details->NanoCpus / 1e9, 2);
            format.Type |= FormatCropZeros;
            details->CpuLimitText = PhFormat(&format, 1, 0);
        }

        details->Privileged = PhGetJsonObjectBool(hostConfig, "Privileged");
        details->PrivilegedText = PhCreateString(details->Privileged ? L"Yes" : L"No");
    }

    if ((state = PhGetJsonObject(Object, "State")) && PhGetJsonObjectType(state) == PH_JSON_OBJECT_TYPE_OBJECT)
    {
        PPH_STRING status = PhGetJsonValueAsString(state, "Status");

        // Only a container that ran and stopped has an exit code worth showing.
        if (status && (PhEqualString2(status, L"exited", TRUE) || PhEqualString2(status, L"dead", TRUE)))
        {
            details->ExitCode = (LONG)PhGetJsonValueAsInt64(state, "ExitCode");

            if (PhGetJsonObjectBool(state, "OOMKilled"))
                details->ExitText = PhFormatString(L"%ld (OOM killed)", details->ExitCode);
            else
                details->ExitText = PhFormatString(L"%ld", details->ExitCode);
        }

        PhClearReference(&status);
    }

    if ((config = PhGetJsonObject(Object, "Config")) && PhGetJsonObjectType(config) == PH_JSON_OBJECT_TYPE_OBJECT)
    {
        // The user of the container and its image together; none set means root.
        details->User = PhGetJsonValueAsString(config, "User");

        if (PhIsNullOrEmptyString(details->User))
            PhMoveReference(&details->User, PhCreateString(L"root"));
    }

    return details;
}

/**
 * Gets the cached details of a container.
 *
 * \param Key Identifies the container, e.g. its full ID.
 * \param State The container's state. Details kept for another state are stale, e.g. the exit
 * code of a container that was restarted.
 * \return The details, referenced, or NULL if there are none for this state.
 */
PWSL_CONTAINER_DETAILS WslGetCachedContainerDetails(
    _In_ PPH_STRING Key,
    _In_opt_ PPH_STRING State
    )
{
    for (ULONG i = 0; WslpContainerDetailsCache && i < WslpContainerDetailsCache->Count; i++)
    {
        PWSL_CONTAINER_DETAILS_ENTRY entry = WslpContainerDetailsCache->Items[i];

        if (!PhEqualString(entry->Key, Key, TRUE))
            continue;

        if (PhCompareStringWithNull(entry->State, State, TRUE) != 0)
            return NULL;

        entry->LastUsedTime = NtGetTickCount64();

        return PhReferenceObject(entry->Details);
    }

    return NULL;
}

/**
 * Keeps the details of a container, replacing those kept for another state.
 */
VOID WslCacheContainerDetails(
    _In_ PPH_STRING Key,
    _In_opt_ PPH_STRING State,
    _In_ PWSL_CONTAINER_DETAILS Details
    )
{
    PWSL_CONTAINER_DETAILS_ENTRY entry = NULL;

    if (!WslpContainerDetailsCache)
        WslpContainerDetailsCache = PhCreateList(8);

    for (ULONG i = 0; i < WslpContainerDetailsCache->Count && !entry; i++)
    {
        if (PhEqualString(((PWSL_CONTAINER_DETAILS_ENTRY)WslpContainerDetailsCache->Items[i])->Key, Key, TRUE))
            entry = WslpContainerDetailsCache->Items[i];
    }

    if (!entry)
    {
        entry = PhAllocateZero(sizeof(WSL_CONTAINER_DETAILS_ENTRY));
        PhSetReference(&entry->Key, Key);
        PhAddItemList(WslpContainerDetailsCache, entry);
    }

    PhSetReference(&entry->State, State);
    PhSetReference(&entry->Details, Details);
    entry->LastUsedTime = NtGetTickCount64();
}

/**
 * Forgets the details of containers that have not been seen for a while.
 */
VOID WslPruneContainerDetails(
    VOID
    )
{
    ULONG64 now = NtGetTickCount64();

    if (!WslpContainerDetailsCache)
        return;

    for (ULONG i = WslpContainerDetailsCache->Count; i != 0; i--)
    {
        PWSL_CONTAINER_DETAILS_ENTRY entry = WslpContainerDetailsCache->Items[i - 1];

        if (now - entry->LastUsedTime < WSL_CONTAINER_DETAILS_LIFETIME_MS)
            continue;

        PhRemoveItemList(WslpContainerDetailsCache, i - 1);
        PhDereferenceObject(entry->Key);
        PhClearReference(&entry->State);
        PhDereferenceObject(entry->Details);
        PhFree(entry);
    }
}
