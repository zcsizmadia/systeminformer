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

static CONST PH_STRINGREF WslpLxssKeyName = PH_STRINGREF_INIT(L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss");
static PPH_OBJECT_TYPE WslpSnapshotType = NULL;

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
        PhClearReference(&distro->Processes);
        PhFree(distro);
    }

    PhDereferenceObject(snapshot->Distributions);

    if (snapshot->Sessions)
        WslFreeSessions(snapshot->Sessions);
}

/**
 * Creates the object type used for snapshots. Must be called once before WslQuerySnapshot.
 */
VOID WslInitializeSnapshotType(
    VOID
    )
{
    WslpSnapshotType = PhCreateObjectType(L"WslSnapshot", 0, WslpSnapshotDeleteProcedure);
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

            WslpApplyRunningList(snapshot->Distributions, text);
            PhDereferenceObject(text);
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

    // Only the write end is inherited, and the handle list keeps any other inheritable
    // handle in this process out of wsl.exe.
    handleFlags.Inherit = TRUE;
    handleFlags.ProtectFromClose = FALSE;

    if (!NT_SUCCESS(status = NtSetInformationObject(writeHandle, ObjectHandleFlagInformation, &handleFlags, sizeof(handleFlags))))
        goto CleanupExit;

    if (!NT_SUCCESS(status = PhInitializeProcThreadAttributeList(&attributeList, 1)))
        goto CleanupExit;
    if (!NT_SUCCESS(status = PhUpdateProcThreadAttribute(attributeList, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &writeHandle, sizeof(HANDLE))))
        goto CleanupExit;

    memset(&startupInfo, 0, sizeof(STARTUPINFOEX));
    startupInfo.StartupInfo.cb = sizeof(STARTUPINFOEX);
    startupInfo.StartupInfo.dwFlags = STARTF_USESHOWWINDOW | STARTF_FORCEOFFFEEDBACK | STARTF_USESTDHANDLES;
    startupInfo.StartupInfo.wShowWindow = SW_HIDE;
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
    NTSTATUS status;
    HANDLE processHandle;
    HANDLE readHandle;
    HANDLE jobHandle;
    PH_BYTES_BUILDER bytesBuilder;
    LARGE_INTEGER timeout;
    ULONG64 startTickCount;
    PROCESS_BASIC_INFORMATION basicInfo;

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

        if (NtGetTickCount64() - startTickCount >= WSL_COMMAND_TIMEOUT_MS)
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
    if (Output && NT_SUCCESS(status))
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
    commandLine = PhFormatString(L"\"%s\" --distribution %s --cd ~", fileName->Buffer, DistroName->Buffer);

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
