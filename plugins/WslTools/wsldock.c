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

// Container engines with a Docker API, e.g. Docker Desktop, Skrog or Podman, whose containers
// run in a WSL distribution.
//
// Such an engine serves the Docker API on a local named pipe. The pipes are found from the
// Docker CLI contexts, DOCKER_HOST and the names of the existing pipes, and each is confirmed by
// asking it for its containers. An engine is placed in the distribution whose processes are in
// the cgroups of its running containers; the cgroup ID is the container ID.
//
// Querying an engine can start its VM, e.g. Docker Desktop resumes from Resource Saver, so no
// pipe is queried unless a running distribution has an engine process, and an engine placed
// before is only queried while its distribution runs.

// A refresh asks every engine, so an engine that does not answer must not hold it up.
#define WSL_ENGINE_TIMEOUT_MS 1000
#define WSL_ENGINE_MAX_RESPONSE (16 * 1024 * 1024)

// Where an engine was placed before, so that it is not queried while its distribution is
// stopped. Used only by the provider thread.
typedef struct _WSL_ENGINE_PLACEMENT
{
    PPH_STRING PipeName;
    PPH_STRING DistroId;
    // From GET /version, asked again only when another process serves the pipe.
    HANDLE ServerProcessId;
    PPH_STRING ProductText;
    PPH_STRING EngineText;
} WSL_ENGINE_PLACEMENT, *PWSL_ENGINE_PLACEMENT;

static PPH_LIST WslpEnginePlacements = NULL;

/**
 * Determines whether a pipe name is safe to open as "\\.\pipe\<name>".
 *
 * \return TRUE if the name is not empty and only has letters, digits and "._-".
 */
BOOLEAN WslIsSafePipeName(
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
 * Converts a Win32 error of a pipe operation to a status.
 *
 * \remarks PhDosErrorToNtStatus asserts on errors missing from its table, which several pipe
 * errors are.
 */
static NTSTATUS WslpPipeErrorToStatus(
    _In_ ULONG Error
    )
{
    switch (Error)
    {
    case ERROR_BROKEN_PIPE:
    case ERROR_PIPE_NOT_CONNECTED:
    case ERROR_NO_DATA:
    case ERROR_HANDLE_EOF:
        return STATUS_PIPE_BROKEN;
    case ERROR_PIPE_BUSY:
        return STATUS_PIPE_BUSY;
    case ERROR_FILE_NOT_FOUND:
        return STATUS_OBJECT_NAME_NOT_FOUND;
    case ERROR_ACCESS_DENIED:
        return STATUS_ACCESS_DENIED;
    case ERROR_OPERATION_ABORTED:
        return STATUS_CANCELLED;
    default:
        return __NTSTATUS_FROM_WIN32(Error);
    }
}

/**
 * Reads from or writes to an overlapped pipe handle, waiting until a deadline.
 *
 * \param FileHandle The pipe, opened with FILE_FLAG_OVERLAPPED.
 * \param EventHandle A manual-reset event for the operation.
 * \param Write TRUE to write, FALSE to read.
 * \param Buffer The data to write, or the buffer to read into.
 * \param Length The number of bytes.
 * \param Deadline The tick count after which the operation is cancelled.
 * \param Transferred Receives the number of bytes transferred.
 * \return STATUS_SUCCESS, STATUS_IO_TIMEOUT, or the status of the failed operation, e.g.
 * STATUS_PIPE_BROKEN when the server closed the pipe.
 */
static NTSTATUS WslpPipeTransfer(
    _In_ HANDLE FileHandle,
    _In_ HANDLE EventHandle,
    _In_ BOOLEAN Write,
    _When_(Write, _In_reads_bytes_(Length)) _When_(!Write, _Out_writes_bytes_to_(Length, *Transferred)) PVOID Buffer,
    _In_ ULONG Length,
    _In_ ULONG64 Deadline,
    _Out_ PULONG Transferred
    )
{
    OVERLAPPED overlapped;
    ULONG64 now = NtGetTickCount64();
    DWORD bytes = 0;
    BOOL result;

    *Transferred = 0;

    if (now >= Deadline)
        return STATUS_IO_TIMEOUT;

    memset(&overlapped, 0, sizeof(OVERLAPPED));
    overlapped.hEvent = EventHandle;
    ResetEvent(EventHandle);

    if (Write)
        result = WriteFile(FileHandle, Buffer, Length, NULL, &overlapped);
    else
        result = ReadFile(FileHandle, Buffer, Length, NULL, &overlapped);

    if (!result && GetLastError() != ERROR_IO_PENDING)
        return WslpPipeErrorToStatus(GetLastError());

    if (WaitForSingleObject(EventHandle, (ULONG)(Deadline - now)) != WAIT_OBJECT_0)
    {
        // A server that stops answering must not hang the provider thread.
        CancelIoEx(FileHandle, &overlapped);
        GetOverlappedResult(FileHandle, &overlapped, &bytes, TRUE);
        return STATUS_IO_TIMEOUT;
    }

    // A message-mode pipe reports ERROR_MORE_DATA for a message longer than the buffer; the
    // rest arrives with the next read.
    if (!GetOverlappedResult(FileHandle, &overlapped, &bytes, FALSE) && GetLastError() != ERROR_MORE_DATA)
        return WslpPipeErrorToStatus(GetLastError());

    *Transferred = bytes;

    return STATUS_SUCCESS;
}

/**
 * Finds the end of the HTTP headers.
 *
 * \return The offset of the body, or 0 if the headers are not complete yet.
 */
static SIZE_T WslpFindHttpBody(
    _In_reads_bytes_(Length) PCSTR Data,
    _In_ SIZE_T Length
    )
{
    for (SIZE_T i = 0; i + 3 < Length; i++)
    {
        if (Data[i] == '\r' && Data[i + 1] == '\n' && Data[i + 2] == '\r' && Data[i + 3] == '\n')
            return i + 4;
    }

    return 0;
}

/**
 * Parses the size line of a chunk, e.g. "1f4" or "1f4;name=value".
 *
 * \return TRUE if the line starts with a hexadecimal size of at most WSL_ENGINE_MAX_RESPONSE.
 */
static BOOLEAN WslpParseChunkSize(
    _In_reads_bytes_(Length) PCSTR Line,
    _In_ SIZE_T Length,
    _Out_ PSIZE_T Size
    )
{
    SIZE_T size = 0;
    SIZE_T i;

    *Size = 0;

    for (i = 0; i < Length; i++)
    {
        CHAR c = Line[i];
        ULONG digit;

        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (c >= 'a' && c <= 'f')
            digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            digit = c - 'A' + 10;
        else
            break;

        size = size * 16 + digit;

        // Anything larger is refused anyway, and a bounded size cannot overflow the checks of
        // the caller on 32-bit.
        if (size > WSL_ENGINE_MAX_RESPONSE)
            return FALSE;
    }

    if (i == 0)
        return FALSE;

    *Size = size;

    return TRUE;
}

/**
 * Decodes a body sent with "Transfer-Encoding: chunked".
 *
 * \param Data The encoded body.
 * \param Length The number of bytes received so far.
 * \param Body Receives the decoded body when the last chunk has been received, or NULL. Can be
 * NULL to only check whether it has been.
 * \return TRUE if the last chunk has been received.
 */
static BOOLEAN WslpDecodeChunkedBody(
    _In_reads_bytes_(Length) PCSTR Data,
    _In_ SIZE_T Length,
    _Out_opt_ PPH_BYTES *Body
    )
{
    PH_BYTES_BUILDER builder;
    SIZE_T offset = 0;
    BOOLEAN complete = FALSE;

    if (Body)
    {
        *Body = NULL;
        PhInitializeBytesBuilder(&builder, Length);
    }

    // Each chunk is "<hex size>\r\n<data>\r\n"; a chunk of size 0 ends the body.
    while (offset < Length)
    {
        PCSTR lineEnd = memchr(Data + offset, '\n', Length - offset);
        SIZE_T size;

        if (!lineEnd || !WslpParseChunkSize(Data + offset, (SIZE_T)(lineEnd - (Data + offset)), &size))
            break;

        offset = (SIZE_T)(lineEnd - Data) + 1;

        if (size == 0)
        {
            complete = TRUE;
            break;
        }

        // The data and its CRLF must have arrived. Written so that it cannot overflow.
        if (size > Length - offset || Length - offset - size < 2)
            break;

        if (Body)
            PhAppendBytesBuilderEx(&builder, (PVOID)(Data + offset), size, 0, NULL);

        offset += size + 2;
    }

    if (Body)
    {
        if (complete)
            *Body = PhFinalBytesBuilderBytes(&builder);
        else
            PhDeleteBytesBuilder(&builder);
    }

    return complete;
}
/**
 * Parses the status line and headers of an HTTP response.
 *
 * \param Data The response, up to the end of the headers.
 * \param Length The length of the headers.
 * \param StatusCode Receives the status code, e.g. 200.
 * \param ContentLength Receives the Content-Length, or SIZE_MAX if there is none.
 * \param Chunked Receives whether the body is sent in chunks.
 * \param ApiVersion Receives the Api-Version header, which every Docker API response has.
 * \param Server Receives the Server header, e.g. "Docker/29.8.1 (linux)".
 * \return TRUE if the status line could be parsed.
 */
static BOOLEAN WslpParseHttpHeaders(
    _In_reads_bytes_(Length) PCSTR Data,
    _In_ SIZE_T Length,
    _Out_ PULONG StatusCode,
    _Out_ PSIZE_T ContentLength,
    _Out_ PBOOLEAN Chunked,
    _Out_opt_ PPH_STRING *ApiVersion,
    _Out_opt_ PPH_STRING *Server
    )
{
    static CONST PH_STRINGREF whitespace = PH_STRINGREF_INIT(L" \t\r");
    PPH_STRING text;
    PH_STRINGREF remaining;
    PH_STRINGREF line;
    PH_STRINGREF version;
    PH_STRINGREF code;
    ULONG64 value;

    *StatusCode = 0;
    *ContentLength = SIZE_MAX;
    *Chunked = FALSE;

    if (ApiVersion)
        *ApiVersion = NULL;
    if (Server)
        *Server = NULL;

    if (!(text = PhConvertUtf8ToUtf16Ex((PSTR)Data, Length)))
        return FALSE;

    remaining = text->sr;

    // "HTTP/1.1 200 OK"
    PhSplitStringRefAtChar(&remaining, L'\n', &line, &remaining);
    PhSplitStringRefAtChar(&line, L' ', &version, &line);
    PhSplitStringRefAtChar(&line, L' ', &code, &line);
    PhTrimStringRef(&code, &whitespace, 0);

    if (!PhStartsWithStringRef2(&version, L"HTTP/", FALSE) || !PhStringToUInt64(&code, 10, &value))
    {
        PhDereferenceObject(text);
        return FALSE;
    }

    *StatusCode = (ULONG)value;

    while (remaining.Length != 0)
    {
        PH_STRINGREF name;
        PH_STRINGREF headerValue;

        PhSplitStringRefAtChar(&remaining, L'\n', &line, &remaining);

        if (!PhSplitStringRefAtChar(&line, L':', &name, &headerValue))
            continue;

        PhTrimStringRef(&name, &whitespace, 0);
        PhTrimStringRef(&headerValue, &whitespace, 0);

        if (PhEqualStringRef2(&name, L"Content-Length", TRUE) && PhStringToUInt64(&headerValue, 10, &value))
            *ContentLength = (SIZE_T)value;
        else if (PhEqualStringRef2(&name, L"Transfer-Encoding", TRUE) && PhFindStringInStringRefZ(&headerValue, L"chunked", TRUE) != SIZE_MAX)
            *Chunked = TRUE;
        else if (ApiVersion && PhEqualStringRef2(&name, L"Api-Version", TRUE))
            PhMoveReference(ApiVersion, PhCreateString2(&headerValue));
        else if (Server && PhEqualStringRef2(&name, L"Server", TRUE))
            PhMoveReference(Server, PhCreateString2(&headerValue));
    }

    PhDereferenceObject(text);

    return TRUE;
}

/**
 * Determines whether an HTTP response has been received completely.
 */
static BOOLEAN WslpIsHttpResponseComplete(
    _In_reads_bytes_(Length) PCSTR Data,
    _In_ SIZE_T Length
    )
{
    SIZE_T bodyOffset;
    ULONG statusCode;
    SIZE_T contentLength;
    BOOLEAN chunked;

    if (!(bodyOffset = WslpFindHttpBody(Data, Length)))
        return FALSE;
    if (!WslpParseHttpHeaders(Data, bodyOffset, &statusCode, &contentLength, &chunked, NULL, NULL))
        return FALSE;

    // These responses have no body.
    if (statusCode == 204 || statusCode == 304 || statusCode < 200)
        return TRUE;
    if (chunked)
        return WslpDecodeChunkedBody(Data + bodyOffset, Length - bodyOffset, NULL);
    if (contentLength != SIZE_MAX)
        return Length - bodyOffset >= contentLength;

    // Without a length the body ends when the server closes the pipe.
    return FALSE;
}

/**
 * Sends a request to a Docker API engine and waits for the response.
 *
 * \param PipeName The engine's pipe, without "\\.\pipe\".
 * \param Method The HTTP method, e.g. "GET" or "POST".
 * \param Path The path and query, e.g. "/containers/json?all=1". Only ASCII.
 * \param TimeoutMs How long the request may take in total.
 * \param StatusCode Receives the HTTP status code.
 * \param Body Receives the response body, or NULL if it has none. The caller owns it.
 * \param ServerText Receives the Server header, or NULL.
 * \param ServerProcessId Receives the process that serves the pipe, or NULL.
 * \return STATUS_SUCCESS if a Docker API response was received, whatever its status code;
 * STATUS_NOT_SUPPORTED if the pipe answered but not as a Docker API; otherwise the error.
 * \remarks The pipe is opened for identification only, so a server squatting on the name
 * cannot impersonate System Informer, which may be running elevated.
 */
static NTSTATUS WslpEngineRequest(
    _In_ PPH_STRING PipeName,
    _In_ PCSTR Method,
    _In_ PCSTR Path,
    _In_ ULONG TimeoutMs,
    _Out_opt_ PULONG StatusCode,
    _Out_opt_ PPH_BYTES *Body,
    _Out_opt_ PPH_STRING *ServerText,
    _Out_opt_ PHANDLE ServerProcessId
    )
{
    static CONST PSTR requestTail = " HTTP/1.1\r\nHost: docker\r\nUser-Agent: SystemInformer-WslTools\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    NTSTATUS status;
    ULONG64 deadline = NtGetTickCount64() + TimeoutMs;
    PPH_STRING fileName;
    HANDLE fileHandle = INVALID_HANDLE_VALUE;
    HANDLE eventHandle = NULL;
    PH_BYTES_BUILDER request;
    PH_BYTES_BUILDER response;
    PPH_BYTES requestBytes = NULL;
    PPH_BYTES responseBytes = NULL;
    ULONG transferred;
    SIZE_T bodyOffset;
    ULONG statusCode;
    SIZE_T contentLength;
    BOOLEAN chunked;
    PPH_STRING apiVersion = NULL;
    PPH_STRING server = NULL;

    if (StatusCode)
        *StatusCode = 0;
    if (Body)
        *Body = NULL;
    if (ServerText)
        *ServerText = NULL;
    if (ServerProcessId)
        *ServerProcessId = NULL;

    if (!WslIsSafePipeName(PipeName))
        return STATUS_INVALID_PARAMETER;
    if (!(fileName = PhFormatString(L"\\\\.\\pipe\\%s", PipeName->Buffer)))
        return STATUS_NO_MEMORY;

    for (ULONG attempt = 0; attempt < 2; attempt++)
    {
        fileHandle = CreateFile(
            fileName->Buffer,
            GENERIC_READ | GENERIC_WRITE,
            0,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
            NULL
            );

        // Every instance of the pipe is busy; wait once for one to become free.
        if (fileHandle != INVALID_HANDLE_VALUE || GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipe(fileName->Buffer, 500))
            break;
    }

    PhDereferenceObject(fileName);

    if (fileHandle == INVALID_HANDLE_VALUE)
        return WslpPipeErrorToStatus(GetLastError());

    if (ServerProcessId)
    {
        ULONG processId;

        if (GetNamedPipeServerProcessId(fileHandle, &processId))
            *ServerProcessId = UlongToHandle(processId);
    }

    if (!(eventHandle = CreateEvent(NULL, TRUE, FALSE, NULL)))
    {
        status = WslpPipeErrorToStatus(GetLastError());
        goto CleanupExit;
    }

    PhInitializeBytesBuilder(&request, 256);
    PhAppendBytesBuilderEx(&request, (PVOID)Method, strlen(Method), 0, NULL);
    PhAppendBytesBuilderEx(&request, " ", 1, 0, NULL);
    PhAppendBytesBuilderEx(&request, (PVOID)Path, strlen(Path), 0, NULL);
    PhAppendBytesBuilderEx(&request, requestTail, strlen(requestTail), 0, NULL);
    requestBytes = PhFinalBytesBuilderBytes(&request);

    if (!NT_SUCCESS(status = WslpPipeTransfer(fileHandle, eventHandle, TRUE, requestBytes->Buffer, (ULONG)requestBytes->Length, deadline, &transferred)))
        goto CleanupExit;

    PhInitializeBytesBuilder(&response, 4096);

    while (TRUE)
    {
        CHAR buffer[PAGE_SIZE];

        status = WslpPipeTransfer(fileHandle, eventHandle, FALSE, buffer, sizeof(buffer), deadline, &transferred);

        // The server closing the pipe ends a response that has no length.
        if (status == STATUS_PIPE_BROKEN || status == STATUS_END_OF_FILE || (NT_SUCCESS(status) && transferred == 0))
        {
            status = STATUS_SUCCESS;
            break;
        }

        if (!NT_SUCCESS(status))
            break;

        PhAppendBytesBuilderEx(&response, buffer, transferred, 0, NULL);

        if (response.Bytes->Length > WSL_ENGINE_MAX_RESPONSE)
        {
            status = STATUS_BUFFER_OVERFLOW;
            break;
        }

        if (WslpIsHttpResponseComplete(response.Bytes->Buffer, response.Bytes->Length))
            break;
    }

    responseBytes = PhFinalBytesBuilderBytes(&response);

    if (!NT_SUCCESS(status))
        goto CleanupExit;

    // A response without the Api-Version header is not from a Docker API.
    if (!(bodyOffset = WslpFindHttpBody(responseBytes->Buffer, responseBytes->Length)) ||
        !WslpParseHttpHeaders(responseBytes->Buffer, bodyOffset, &statusCode, &contentLength, &chunked, &apiVersion, &server) ||
        !apiVersion)
    {
        status = STATUS_NOT_SUPPORTED;
        goto CleanupExit;
    }

    if (StatusCode)
        *StatusCode = statusCode;

    if (Body && responseBytes->Length > bodyOffset)
    {
        if (chunked)
            WslpDecodeChunkedBody(responseBytes->Buffer + bodyOffset, responseBytes->Length - bodyOffset, Body);
        else
            *Body = PhCreateBytesEx(responseBytes->Buffer + bodyOffset, min(responseBytes->Length - bodyOffset, contentLength));
    }

    if (ServerText)
        PhSetReference(ServerText, server);

CleanupExit:
    PhClearReference(&apiVersion);
    PhClearReference(&server);
    PhClearReference(&requestBytes);
    PhClearReference(&responseBytes);

    if (eventHandle)
        NtClose(eventHandle);

    CloseHandle(fileHandle);

    return status;
}

/**
 * Sends a request to a Docker API engine and waits for the response.
 *
 * \param PipeName The engine's pipe, without "\\.\pipe\".
 * \param Method The HTTP method, e.g. "POST".
 * \param Path The path and query, e.g. "/containers/<id>/stop". Only ASCII.
 * \param TimeoutMs How long the request may take in total.
 * \param StatusCode Receives the HTTP status code.
 * \param Body Receives the response body, or NULL if it has none. The caller owns it.
 * \return STATUS_SUCCESS if a Docker API response was received, whatever its status code.
 * \remarks Must not be called on the GUI thread; it waits for the engine.
 */
NTSTATUS WslEngineRequest(
    _In_ PPH_STRING PipeName,
    _In_ PCSTR Method,
    _In_ PCSTR Path,
    _In_ ULONG TimeoutMs,
    _Out_opt_ PULONG StatusCode,
    _Out_opt_ PPH_BYTES *Body
    )
{
    return WslpEngineRequest(PipeName, Method, Path, TimeoutMs, StatusCode, Body, NULL, NULL);
}

/**
 * Gets the error message of a failed Docker API request.
 *
 * \param Body The response body, e.g. {"message":"No such container: ..."}.
 * \return The message, or NULL if there is none.
 */
PPH_STRING WslGetEngineErrorMessage(
    _In_opt_ PPH_BYTES Body
    )
{
    PVOID object;
    PPH_STRING message = NULL;

    if (!Body || Body->Length == 0)
        return NULL;

    if (NT_SUCCESS(PhCreateJsonParserEx(&object, Body, FALSE)) && object)
    {
        if (PhGetJsonObjectType(object) == PH_JSON_OBJECT_TYPE_OBJECT)
            message = PhGetJsonValueAsString(object, "message");

        PhFreeJsonObject(object);
    }

    if (message && message->Length == 0)
        PhClearReference(&message);

    return message;
}

/**
 * Gets the pipe name of a Docker host, e.g. "npipe:////./pipe/docker_engine".
 *
 * \return The pipe name, or NULL if the host is not a pipe on this computer.
 */
static PPH_STRING WslpGetHostPipeName(
    _In_ PPH_STRING Host
    )
{
    static CONST PH_STRINGREF scheme = PH_STRINGREF_INIT(L"npipe:");
    static CONST PH_STRINGREF localPrefix = PH_STRINGREF_INIT(L"./pipe/");
    PH_STRINGREF remaining = Host->sr;
    PPH_STRING name;

    if (!PhStartsWithStringRef(&remaining, &scheme, TRUE))
        return NULL;

    PhSkipStringRef(&remaining, scheme.Length);

    while (remaining.Length != 0 && remaining.Buffer[0] == L'/')
        PhSkipStringRef(&remaining, sizeof(WCHAR));

    // Only "." is this computer; a server name would open the pipe over the network.
    if (!PhStartsWithStringRef(&remaining, &localPrefix, TRUE))
        return NULL;

    PhSkipStringRef(&remaining, localPrefix.Length);
    name = PhCreateString2(&remaining);

    if (!WslIsSafePipeName(name))
        PhClearReference(&name);

    return name;
}

/**
 * Adds a pipe name to a list, unless it is already there.
 */
static VOID WslpAddCandidatePipe(
    _Inout_ PPH_LIST Candidates,
    _In_ PPH_STRING PipeName
    )
{
    for (ULONG i = 0; i < Candidates->Count; i++)
    {
        if (PhEqualString(Candidates->Items[i], PipeName, TRUE))
            return;
    }

    PhAddItemList(Candidates, PhReferenceObject(PipeName));
}

/**
 * Adds the pipes of the Docker CLI contexts, e.g. "skrog" or "desktop-linux".
 *
 * \remarks Each context is %USERPROFILE%\.docker\contexts\meta\<hash>\meta.json, or under
 * %DOCKER_CONFIG% when that is set, with {"Endpoints":{"docker":{"Host":"npipe:..."}}}.
 */
static VOID WslpAddContextPipes(
    _Inout_ PPH_LIST Candidates
    )
{
    static CONST PH_STRINGREF dockerConfigName = PH_STRINGREF_INIT(L"DOCKER_CONFIG");
    static CONST PH_STRINGREF defaultConfig = PH_STRINGREF_INIT(L"%USERPROFILE%\\.docker");
    PPH_STRING configDirectory = NULL;
    PPH_STRING pattern;
    WIN32_FIND_DATA findData;
    HANDLE findHandle;

    if (!NT_SUCCESS(PhQueryEnvironmentVariable(NULL, &dockerConfigName, &configDirectory)) || PhIsNullOrEmptyString(configDirectory))
        PhMoveReference(&configDirectory, PhExpandEnvironmentStrings(&defaultConfig));

    if (!configDirectory)
        return;

    pattern = PhFormatString(L"%s\\contexts\\meta\\*", configDirectory->Buffer);

    if (pattern && (findHandle = FindFirstFile(pattern->Buffer, &findData)) != INVALID_HANDLE_VALUE)
    {
        do
        {
            PPH_STRING metaFileName;
            PPH_BYTES meta;
            PVOID object;

            if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || findData.cFileName[0] == L'.')
                continue;

            if (!(metaFileName = PhFormatString(L"%s\\contexts\\meta\\%s\\meta.json", configDirectory->Buffer, findData.cFileName)))
                continue;

            if (NT_SUCCESS(PhFileReadAllTextWin32(&meta, metaFileName->Buffer, FALSE)))
            {
                if (meta->Length != 0 && NT_SUCCESS(PhCreateJsonParserEx(&object, meta, FALSE)) && object)
                {
                    PVOID endpoints;
                    PVOID docker;
                    PPH_STRING host;
                    PPH_STRING pipeName;

                    if ((endpoints = PhGetJsonObject(object, "Endpoints")) &&
                        (docker = PhGetJsonObject(endpoints, "docker")) &&
                        (host = PhGetJsonValueAsString(docker, "Host")))
                    {
                        if (pipeName = WslpGetHostPipeName(host))
                        {
                            WslpAddCandidatePipe(Candidates, pipeName);
                            PhDereferenceObject(pipeName);
                        }

                        PhDereferenceObject(host);
                    }

                    PhFreeJsonObject(object);
                }

                PhDereferenceObject(meta);
            }

            PhDereferenceObject(metaFileName);
        } while (FindNextFile(findHandle, &findData));

        FindClose(findHandle);
    }

    PhClearReference(&pattern);
    PhDereferenceObject(configDirectory);
}

/**
 * Gets the pipes that serve a Docker API: those of the Docker CLI contexts, e.g.
 * "dockerDesktopLinuxEngine" or "skrog_engine", of DOCKER_HOST, the CLI's default
 * "docker_engine", and Podman's "podman-machine-<name>".
 *
 * \return The names of the candidate pipes that exist. Free the list with PhDereferenceObjects
 * and PhDereferenceObject.
 * \remarks Only the pipes a Docker client would use are asked. The names cannot be matched by
 * a pattern: Docker Desktop alone has some thirty "docker..." pipes for its own services,
 * which could misread an HTTP request.
 */
static PPH_LIST WslpGetCandidatePipes(
    VOID
    )
{
    static CONST PH_STRINGREF dockerHostName = PH_STRINGREF_INIT(L"DOCKER_HOST");
    static CONST PH_STRINGREF defaultPipeName = PH_STRINGREF_INIT(L"docker_engine");
    static CONST PH_STRINGREF podmanPrefix = PH_STRINGREF_INIT(L"podman-machine-");
    PPH_LIST candidates;
    PPH_LIST existing;
    PPH_LIST result;
    PPH_STRING dockerHost;
    WIN32_FIND_DATA findData;
    HANDLE findHandle;

    candidates = PhCreateList(4);
    existing = PhCreateList(64);

    if ((findHandle = FindFirstFile(L"\\\\.\\pipe\\*", &findData)) != INVALID_HANDLE_VALUE)
    {
        do
        {
            PhAddItemList(existing, PhCreateString(findData.cFileName));
        } while (FindNextFile(findHandle, &findData));

        FindClose(findHandle);
    }

    // Configured endpoints first, so they are preferred when two pipes serve the same engine.
    WslpAddContextPipes(candidates);

    if (NT_SUCCESS(PhQueryEnvironmentVariable(NULL, &dockerHostName, &dockerHost)))
    {
        PPH_STRING pipeName;

        if (pipeName = WslpGetHostPipeName(dockerHost))
        {
            WslpAddCandidatePipe(candidates, pipeName);
            PhDereferenceObject(pipeName);
        }

        PhDereferenceObject(dockerHost);
    }

    // Without a context the Docker CLI uses "docker_engine". Podman does not add a context.
    for (ULONG i = 0; i < existing->Count; i++)
    {
        PPH_STRING name = existing->Items[i];

        if (!WslIsSafePipeName(name))
            continue;

        if (PhEqualStringRef(&name->sr, &defaultPipeName, TRUE) || PhStartsWithStringRef(&name->sr, &podmanPrefix, TRUE))
            WslpAddCandidatePipe(candidates, name);
    }

    // Keep the candidates that exist now.
    result = PhCreateList(candidates->Count);

    for (ULONG i = 0; i < candidates->Count; i++)
    {
        for (ULONG j = 0; j < existing->Count; j++)
        {
            if (PhEqualString(candidates->Items[i], existing->Items[j], TRUE))
            {
                PhAddItemList(result, PhReferenceObject(candidates->Items[i]));
                break;
            }
        }
    }

    PhDereferenceObjects(candidates->Items, candidates->Count);
    PhDereferenceObject(candidates);
    PhDereferenceObjects(existing->Items, existing->Count);
    PhDereferenceObject(existing);

    return result;
}

/**
 * Formats the ports of a container as the Docker CLI does, e.g. "0.0.0.0:8080->80/tcp".
 *
 * \param Ports The "Ports" array of GET /containers/json.
 * \return The ports, separated by ", ", or NULL if there are none.
 */
static PPH_STRING WslpFormatEnginePorts(
    _In_ PVOID Ports
    )
{
    PPH_STRING text = NULL;
    ULONG count = PhGetJsonArrayLength(Ports);

    for (ULONG i = 0; i < count; i++)
    {
        PVOID port = PhGetJsonArrayIndexObject(Ports, i);
        PPH_STRING ip;
        PPH_STRING type;
        ULONG64 privatePort;
        ULONG64 publicPort;
        PPH_STRING part;

        if (!port || PhGetJsonObjectType(port) != PH_JSON_OBJECT_TYPE_OBJECT)
            continue;

        ip = PhGetJsonValueAsString(port, "IP");
        type = PhGetJsonValueAsString(port, "Type");
        privatePort = PhGetJsonValueAsUInt64(port, "PrivatePort");
        publicPort = PhGetJsonValueAsUInt64(port, "PublicPort");

        // An IPv6 address is bracketed, e.g. "[::]:8080->80/tcp".
        if (publicPort != 0 && !PhIsNullOrEmptyString(ip))
        {
            BOOLEAN ipv6 = PhFindCharInString(ip, 0, L':') != SIZE_MAX;

            part = PhFormatString(L"%s%s%s:%I64u->%I64u/%s", ipv6 ? L"[" : L"", ip->Buffer, ipv6 ? L"]" : L"", publicPort, privatePort, PhGetStringOrDefault(type, L"tcp"));
        }
        else
        {
            part = PhFormatString(L"%I64u/%s", privatePort, PhGetStringOrDefault(type, L"tcp"));
        }

        if (part)
        {
            if (text)
                PhMoveReference(&text, PhFormatString(L"%s, %s", text->Buffer, part->Buffer));
            else
                PhSetReference(&text, part);

            PhDereferenceObject(part);
        }

        PhClearReference(&ip);
        PhClearReference(&type);
    }

    return text;
}

/**
 * Creates a container from an element of GET /containers/json.
 *
 * \return The container, or NULL if the element has no valid ID.
 */
static PWSL_CONTAINER WslpParseEngineContainer(
    _In_ PVOID Object
    )
{
    PWSL_CONTAINER container;
    PVOID names;
    PVOID ports;

    if (PhGetJsonObjectType(Object) != PH_JSON_OBJECT_TYPE_OBJECT)
        return NULL;

    container = PhAllocateZero(sizeof(WSL_CONTAINER));
    container->Id = PhGetJsonValueAsString(Object, "Id");

    // The ID is used in request paths, so it must be hexadecimal.
    if (!container->Id || !WslIsSafeContainerId(container->Id))
    {
        WslFreeContainer(container);
        return NULL;
    }

    // "Names" is e.g. ["/fervent_feynman"].
    if ((names = PhGetJsonObject(Object, "Names")) && PhGetJsonObjectType(names) == PH_JSON_OBJECT_TYPE_ARRAY && PhGetJsonArrayLength(names) != 0)
    {
        PVOID first = PhGetJsonArrayIndexObject(names, 0);

        if (first && (container->Name = PhGetJsonObjectString(first)) && container->Name->Length != 0 && container->Name->Buffer[0] == L'/')
            PhMoveReference(&container->Name, PhSubstring(container->Name, 1, container->Name->Length / sizeof(WCHAR) - 1));
    }

    if (PhIsNullOrEmptyString(container->Name))
        PhMoveReference(&container->Name, PhSubstring(container->Id, 0, min(12, container->Id->Length / sizeof(WCHAR))));

    container->Image = PhGetJsonValueAsString(Object, "Image");
    container->State = PhGetJsonValueAsString(Object, "State");
    container->Status = PhGetJsonValueAsString(Object, "Status");

    if ((ports = PhGetJsonObject(Object, "Ports")) && PhGetJsonObjectType(ports) == PH_JSON_OBJECT_TYPE_ARRAY)
        container->Ports = WslpFormatEnginePorts(ports);

    container->Running = container->State && PhEqualString2(container->State, L"running", TRUE);

    // The API reports states in lower case, like wslc; show them like the other rows. The
    // string is new and not shared yet, so it can still be changed.
    if (!PhIsNullOrEmptyString(container->State))
        container->State->Buffer[0] = RtlUpcaseUnicodeChar(container->State->Buffer[0]);

    return container;
}

/**
 * Frees an engine.
 */
static VOID WslpFreeEngine(
    _In_ PWSL_ENGINE Engine
    )
{
    for (ULONG i = 0; i < Engine->Containers->Count; i++)
        WslFreeContainer(Engine->Containers->Items[i]);

    PhDereferenceObject(Engine->Containers);
    PhClearReference(&Engine->PipeName);
    PhClearReference(&Engine->ProductText);
    PhClearReference(&Engine->EngineText);
    PhClearReference(&Engine->ServerText);
    PhClearReference(&Engine->DistroId);
    PhFree(Engine);
}

/**
 * Frees an engine list returned by WslQueryEngines.
 */
VOID WslFreeEngines(
    _In_ PPH_LIST Engines
    )
{
    for (ULONG i = 0; i < Engines->Count; i++)
        WslpFreeEngine(Engines->Items[i]);

    PhDereferenceObject(Engines);
}

/**
 * Gets the product that serves an engine's pipe from the version resource of the server
 * process, e.g. "Rancher Desktop 1.16.0", or from its name, e.g. "Skrog" for skrog.exe.
 *
 * \return The product, or NULL if the process is gone.
 */
static PPH_STRING WslpGetServerProductText(
    _In_ HANDLE ServerProcessId
    )
{
    PPH_PROCESS_ITEM processItem;
    PPH_STRING text = NULL;
    PPH_STRING name;

    if (!ServerProcessId || !(processItem = PhReferenceProcessItem(ServerProcessId)))
        return NULL;

    name = !PhIsNullOrEmptyString(processItem->VersionInfo.ProductName) ? processItem->VersionInfo.ProductName : processItem->VersionInfo.FileDescription;

    if (!PhIsNullOrEmptyString(name))
    {
        if (!PhIsNullOrEmptyString(processItem->VersionInfo.FileVersion))
        {
            PPH_STRING version = WslFormatDisplayVersion(&processItem->VersionInfo.FileVersion->sr);

            text = PhFormatString(L"%s %s", name->Buffer, version->Buffer);
            PhDereferenceObject(version);
        }
        else
        {
            PhSetReference(&text, name);
        }
    }
    else if (processItem->ProcessName && processItem->ProcessName->Length != 0)
    {
        PH_STRINGREF baseName = processItem->ProcessName->sr;
        PH_STRINGREF extension;

        // Without a version resource the name is all there is, e.g. "skrog.exe".
        PhSplitStringRefAtLastChar(&processItem->ProcessName->sr, L'.', &baseName, &extension);
        text = PhCreateString2(&baseName);
        text->Buffer[0] = RtlUpcaseUnicodeChar(text->Buffer[0]);
    }

    PhDereferenceObject(processItem);

    return text;
}

/**
 * Gets the product and the engine of an engine from GET /version.
 *
 * \param Engine The engine.
 * \param ProductText Receives the product, e.g. "Docker Desktop 4.92.0" from Platform.Name,
 * "Podman 5.2.0" from its "Podman Engine" component, or else that of the pipe's server process.
 * \param EngineText Receives the engine, e.g. "Docker 29.8.1", or NULL for Podman.
 */
static VOID WslpQueryEngineVersion(
    _In_ PWSL_ENGINE Engine,
    _Out_ PPH_STRING *ProductText,
    _Out_ PPH_STRING *EngineText
    )
{
    static CONST PH_STRINGREF podmanComponent = PH_STRINGREF_INIT(L"Podman Engine");
    static CONST PH_STRINGREF plainPlatform = PH_STRINGREF_INIT(L"Docker Engine");
    ULONG statusCode;
    PPH_BYTES body;
    PVOID object;
    PPH_STRING platformName = NULL;
    PPH_STRING version = NULL;
    PPH_STRING podmanVersion = NULL;

    *ProductText = NULL;
    *EngineText = NULL;

    if (NT_SUCCESS(WslEngineRequest(Engine->PipeName, "GET", "/version", WSL_ENGINE_TIMEOUT_MS, &statusCode, &body)))
    {
        if (statusCode == 200 && body && NT_SUCCESS(PhCreateJsonParserEx(&object, body, FALSE)) && object)
        {
            PVOID platform;
            PVOID components;

            if (PhGetJsonObjectType(object) == PH_JSON_OBJECT_TYPE_OBJECT)
            {
                if (platform = PhGetJsonObject(object, "Platform"))
                    platformName = PhGetJsonValueAsString(platform, "Name");

                version = PhGetJsonValueAsString(object, "Version");

                if ((components = PhGetJsonObject(object, "Components")) && PhGetJsonObjectType(components) == PH_JSON_OBJECT_TYPE_ARRAY)
                {
                    for (ULONG i = 0; i < PhGetJsonArrayLength(components) && !podmanVersion; i++)
                    {
                        PVOID component = PhGetJsonArrayIndexObject(components, i);
                        PPH_STRING componentName;

                        if (!component || !(componentName = PhGetJsonValueAsString(component, "Name")))
                            continue;

                        if (PhEqualStringRef(&componentName->sr, &podmanComponent, TRUE))
                            podmanVersion = PhGetJsonValueAsString(component, "Version");

                        PhDereferenceObject(componentName);
                    }
                }
            }

            PhFreeJsonObject(object);
        }

        PhClearReference(&body);
    }

    if (!PhIsNullOrEmptyString(podmanVersion))
    {
        // Podman is the engine itself.
        *ProductText = PhFormatString(L"Podman %s", podmanVersion->Buffer);
    }
    else
    {
        // Docker Desktop names itself, e.g. "Docker Desktop 4.92.0 (240144)"; the build number
        // is left out. A plain "Docker Engine - Community" says nothing about the product.
        if (!PhIsNullOrEmptyString(platformName) && !PhStartsWithStringRef(&platformName->sr, &plainPlatform, TRUE))
        {
            PH_STRINGREF name = platformName->sr;
            PH_STRINGREF build;
            PH_STRINGREF firstPart;

            if (PhSplitStringRefAtLastChar(&name, L'(', &firstPart, &build) && firstPart.Length != 0)
            {
                static CONST PH_STRINGREF whitespace = PH_STRINGREF_INIT(L" ");

                PhTrimStringRef(&firstPart, &whitespace, 0);
                name = firstPart;
            }

            *ProductText = PhCreateString2(&name);
        }
        else
        {
            *ProductText = WslpGetServerProductText(Engine->ServerProcessId);
        }

        if (!PhIsNullOrEmptyString(version))
            *EngineText = PhFormatString(L"Docker %s", version->Buffer);
    }

    PhClearReference(&platformName);
    PhClearReference(&version);
    PhClearReference(&podmanVersion);
}

/**
 * Asks a pipe for its containers.
 *
 * \return The engine, without a distribution yet, or NULL if the pipe is not a Docker API.
 */
static PWSL_ENGINE WslpQueryEngine(
    _In_ PPH_STRING PipeName
    )
{
    PWSL_ENGINE engine = NULL;
    ULONG statusCode;
    PPH_BYTES body;
    PPH_STRING server;
    HANDLE serverProcessId;
    PVOID object;

    if (!NT_SUCCESS(WslpEngineRequest(PipeName, "GET", "/containers/json?all=1", WSL_ENGINE_TIMEOUT_MS, &statusCode, &body, &server, &serverProcessId)))
        return NULL;

    // The Server header ends with the OS, e.g. "Docker/29.8.1 (linux)". Windows containers do
    // not run in WSL.
    if (server && PhFindStringInStringRefZ(&server->sr, L"(windows)", TRUE) != SIZE_MAX)
        statusCode = 0;

    if (statusCode == 200 && body && NT_SUCCESS(PhCreateJsonParserEx(&object, body, FALSE)) && object)
    {
        if (PhGetJsonObjectType(object) == PH_JSON_OBJECT_TYPE_ARRAY)
        {
            ULONG count = PhGetJsonArrayLength(object);

            engine = PhAllocateZero(sizeof(WSL_ENGINE));
            PhSetReference(&engine->PipeName, PipeName);
            engine->ServerText = server;
            server = NULL;
            engine->ServerProcessId = serverProcessId;
            engine->Containers = PhCreateList(max(count, 1));

            for (ULONG i = 0; i < count; i++)
            {
                PVOID element = PhGetJsonArrayIndexObject(object, i);
                PWSL_CONTAINER container;

                if (element && (container = WslpParseEngineContainer(element)))
                    PhAddItemList(engine->Containers, container);
            }
        }

        PhFreeJsonObject(object);
    }

    PhClearReference(&body);
    PhClearReference(&server);

    return engine;
}

/**
 * Finds a container of an engine by its full ID.
 */
static PWSL_CONTAINER WslpFindEngineContainer(
    _In_ PWSL_ENGINE Engine,
    _In_ PPH_STRING Id
    )
{
    for (ULONG i = 0; i < Engine->Containers->Count; i++)
    {
        PWSL_CONTAINER container = Engine->Containers->Items[i];

        if (PhEqualString(container->Id, Id, TRUE))
            return container;
    }

    return NULL;
}

/**
 * Determines whether two engines are one engine served on two pipes, e.g. Docker Desktop on
 * "docker_engine" and "dockerDesktopLinuxEngine".
 */
static BOOLEAN WslpIsSameEngine(
    _In_ PWSL_ENGINE Engine1,
    _In_ PWSL_ENGINE Engine2
    )
{
    if (Engine1->ServerProcessId != Engine2->ServerProcessId || Engine1->Containers->Count != Engine2->Containers->Count)
        return FALSE;

    for (ULONG i = 0; i < Engine1->Containers->Count; i++)
    {
        if (!WslpFindEngineContainer(Engine2, ((PWSL_CONTAINER)Engine1->Containers->Items[i])->Id))
            return FALSE;
    }

    return TRUE;
}

/**
 * Determines whether an engine is a Docker API in front of WSLC, whose containers the WSLC
 * sessions already show.
 */
static BOOLEAN WslpIsSessionEngine(
    _In_ PWSL_ENGINE Engine,
    _In_opt_ PPH_LIST Sessions
    )
{
    for (ULONG i = 0; Sessions && i < Sessions->Count; i++)
    {
        PWSL_SESSION session = Sessions->Items[i];

        for (ULONG j = 0; j < session->Containers->Count; j++)
        {
            PWSL_CONTAINER sessionContainer = session->Containers->Items[j];

            for (ULONG k = 0; k < Engine->Containers->Count; k++)
            {
                // "wslc list" prints the short container ID.
                if (PhStartsWithString(((PWSL_CONTAINER)Engine->Containers->Items[k])->Id, sessionContainer->Id, TRUE))
                    return TRUE;
            }
        }
    }

    return FALSE;
}

/**
 * Determines whether a distribution runs a container engine, e.g. dockerd or Podman.
 */
static BOOLEAN WslpHasEngineProcess(
    _In_ PWSL_DISTRO_ITEM Distro
    )
{
    static CONST PH_STRINGREF dockerdName = PH_STRINGREF_INIT(L"dockerd");
    static CONST PH_STRINGREF podmanName = PH_STRINGREF_INIT(L"podman");

    if (Distro->State != WslDistroStateRunning || !Distro->Processes)
        return FALSE;

    for (ULONG i = 0; i < Distro->Processes->Processes->Count; i++)
    {
        PWSL_LINUX_PROCESS process = Distro->Processes->Processes->Items[i];

        if (process->Name && (PhEqualStringRef(&process->Name->sr, &dockerdName, FALSE) || PhEqualStringRef(&process->Name->sr, &podmanName, FALSE)))
            return TRUE;
    }

    return FALSE;
}

/**
 * Counts the processes of a distribution that run in the containers of an engine.
 */
static ULONG WslpCountEngineProcesses(
    _In_ PWSL_ENGINE Engine,
    _In_ PWSL_DISTRO_ITEM Distro
    )
{
    ULONG count = 0;

    for (ULONG i = 0; i < Distro->Processes->Processes->Count; i++)
    {
        PWSL_LINUX_PROCESS process = Distro->Processes->Processes->Items[i];

        if (process->ContainerId && WslpFindEngineContainer(Engine, process->ContainerId))
            count++;
    }

    return count;
}

/**
 * Finds a distribution of a snapshot by its id.
 */
static PWSL_DISTRO_ITEM WslpFindSnapshotDistro(
    _In_ PWSL_SNAPSHOT Snapshot,
    _In_ PPH_STRING Id
    )
{
    for (ULONG i = 0; i < Snapshot->Distributions->Count; i++)
    {
        PWSL_DISTRO_ITEM distro = Snapshot->Distributions->Items[i];

        if (PhEqualString(distro->Id, Id, TRUE))
            return distro;
    }

    return NULL;
}

/**
 * Finds where an engine was placed before.
 */
static PWSL_ENGINE_PLACEMENT WslpFindEnginePlacement(
    _In_ PPH_STRING PipeName
    )
{
    for (ULONG i = 0; WslpEnginePlacements && i < WslpEnginePlacements->Count; i++)
    {
        PWSL_ENGINE_PLACEMENT placement = WslpEnginePlacements->Items[i];

        if (PhEqualString(placement->PipeName, PipeName, TRUE))
            return placement;
    }

    return NULL;
}

/**
 * Remembers where an engine is placed.
 */
static PWSL_ENGINE_PLACEMENT WslpSetEnginePlacement(
    _In_ PPH_STRING PipeName,
    _In_ PPH_STRING DistroId
    )
{
    PWSL_ENGINE_PLACEMENT placement;

    if (!WslpEnginePlacements)
        WslpEnginePlacements = PhCreateList(2);

    if (!(placement = WslpFindEnginePlacement(PipeName)))
    {
        placement = PhAllocateZero(sizeof(WSL_ENGINE_PLACEMENT));
        PhSetReference(&placement->PipeName, PipeName);
        PhAddItemList(WslpEnginePlacements, placement);
    }

    PhSetReference(&placement->DistroId, DistroId);

    return placement;
}

/**
 * Forgets where the engines were placed, e.g. when the tab is hidden.
 */
VOID WslResetEngines(
    VOID
    )
{
    if (!WslpEnginePlacements)
        return;

    for (ULONG i = 0; i < WslpEnginePlacements->Count; i++)
    {
        PWSL_ENGINE_PLACEMENT placement = WslpEnginePlacements->Items[i];

        PhDereferenceObject(placement->PipeName);
        PhDereferenceObject(placement->DistroId);
        PhClearReference(&placement->ProductText);
        PhClearReference(&placement->EngineText);
        PhFree(placement);
    }

    PhClearList(WslpEnginePlacements);
}

/**
 * Sums the CPU and memory usage of each running container over its processes, which the
 * distribution's collector measures. This avoids "stats" requests, which are slow.
 */
static VOID WslpUpdateEngineStats(
    _In_ PWSL_ENGINE Engine,
    _In_ PWSL_DISTRO_ITEM Distro
    )
{
    PWSL_PROCESS_FRAME frame = Distro->Processes;

    for (ULONG i = 0; i < frame->Processes->Count; i++)
    {
        PWSL_LINUX_PROCESS process = frame->Processes->Items[i];
        PWSL_CONTAINER container;

        if (!process->ContainerId || !(container = WslpFindEngineContainer(Engine, process->ContainerId)))
            continue;

        container->MemoryBytes += process->ResidentBytes;
        container->NumberOfProcesses++;

        if (process->HaveCpuUsage)
            container->CpuUsage += process->CpuUsage;
    }

    for (ULONG i = 0; i < Engine->Containers->Count; i++)
    {
        PWSL_CONTAINER container = Engine->Containers->Items[i];

        container->HaveStats = container->Running && frame->HaveCpuUsage;
    }
}

/**
 * Finds the container engines with a Docker API, and the distributions their containers run in.
 *
 * \param Snapshot The snapshot, with the frames of the running distributions attached.
 * \return The engines placed in a running distribution, or NULL if there are none. Free the
 * list with WslFreeEngines.
 * \remarks An engine without running containers is placed in the one distribution with an
 * engine process that no other engine was placed in; otherwise it is left out.
 */
PPH_LIST WslQueryEngines(
    _In_ struct _WSL_SNAPSHOT *Snapshot
    )
{
    PPH_LIST engineDistros;
    PPH_LIST candidates;
    PPH_LIST engines;
    PPH_LIST unplaced;

    engineDistros = PhCreateList(2);

    for (ULONG i = 0; i < Snapshot->Distributions->Count; i++)
    {
        PWSL_DISTRO_ITEM distro = Snapshot->Distributions->Items[i];

        if (WslpHasEngineProcess(distro))
            PhAddItemList(engineDistros, distro);
    }

    if (engineDistros->Count == 0)
    {
        PhDereferenceObject(engineDistros);
        return NULL;
    }

    candidates = WslpGetCandidatePipes();
    engines = PhCreateList(2);
    unplaced = PhCreateList(2);

    for (ULONG i = 0; i < candidates->Count; i++)
    {
        PPH_STRING pipeName = candidates->Items[i];
        PWSL_ENGINE_PLACEMENT placement = WslpFindEnginePlacement(pipeName);
        PWSL_DISTRO_ITEM placedDistro = placement ? WslpFindSnapshotDistro(Snapshot, placement->DistroId) : NULL;
        PWSL_ENGINE engine;
        BOOLEAN duplicate = FALSE;
        PWSL_DISTRO_ITEM bestDistro = NULL;
        ULONG bestCount = 0;

        // Asking an engine whose distribution stopped could start it again.
        if (placement && (!placedDistro || placedDistro->State != WslDistroStateRunning))
            continue;

        if (!(engine = WslpQueryEngine(pipeName)))
            continue;

        for (ULONG j = 0; j < engines->Count && !duplicate; j++)
            duplicate = WslpIsSameEngine(engines->Items[j], engine);
        for (ULONG j = 0; j < unplaced->Count && !duplicate; j++)
            duplicate = WslpIsSameEngine(unplaced->Items[j], engine);

        if (duplicate || WslpIsSessionEngine(engine, Snapshot->Sessions))
        {
            WslpFreeEngine(engine);
            continue;
        }

        for (ULONG j = 0; j < engineDistros->Count; j++)
        {
            ULONG count = WslpCountEngineProcesses(engine, engineDistros->Items[j]);

            if (count > bestCount)
            {
                bestCount = count;
                bestDistro = engineDistros->Items[j];
            }
        }

        if (!bestDistro && placedDistro && WslpHasEngineProcess(placedDistro))
            bestDistro = placedDistro;

        if (bestDistro)
        {
            PhSetReference(&engine->DistroId, bestDistro->Id);
            PhAddItemList(engines, engine);
        }
        else
        {
            PhAddItemList(unplaced, engine);
        }
    }

    // A single engine without running containers goes to the single remaining engine distribution.
    if (unplaced->Count == 1)
    {
        PWSL_DISTRO_ITEM freeDistro = NULL;
        ULONG freeCount = 0;

        for (ULONG i = 0; i < engineDistros->Count; i++)
        {
            PWSL_DISTRO_ITEM distro = engineDistros->Items[i];
            BOOLEAN taken = FALSE;

            for (ULONG j = 0; j < engines->Count && !taken; j++)
                taken = PhEqualString(((PWSL_ENGINE)engines->Items[j])->DistroId, distro->Id, TRUE);

            if (!taken)
            {
                freeDistro = distro;
                freeCount++;
            }
        }

        if (freeCount == 1 && freeDistro)
        {
            PWSL_ENGINE engine = unplaced->Items[0];

            PhSetReference(&engine->DistroId, freeDistro->Id);
            PhAddItemList(engines, engine);
            PhClearList(unplaced);
        }
    }

    for (ULONG i = 0; i < unplaced->Count; i++)
        WslpFreeEngine(unplaced->Items[i]);

    for (ULONG i = 0; i < engines->Count; i++)
    {
        PWSL_ENGINE engine = engines->Items[i];

        PWSL_ENGINE_PLACEMENT placement = WslpSetEnginePlacement(engine->PipeName, engine->DistroId);

        // The product and version only change with the process that serves the pipe.
        if (!placement->ProductText || placement->ServerProcessId != engine->ServerProcessId)
        {
            PhClearReference(&placement->ProductText);
            PhClearReference(&placement->EngineText);
            WslpQueryEngineVersion(engine, &placement->ProductText, &placement->EngineText);
            placement->ServerProcessId = engine->ServerProcessId;
        }

        PhSetReference(&engine->ProductText, placement->ProductText);
        PhSetReference(&engine->EngineText, placement->EngineText);
        WslpUpdateEngineStats(engine, WslpFindSnapshotDistro(Snapshot, engine->DistroId));
    }

    PhDereferenceObject(unplaced);
    PhDereferenceObjects(candidates->Items, candidates->Count);
    PhDereferenceObject(candidates);
    PhDereferenceObject(engineDistros);

    if (engines->Count == 0)
        PhClearReference(&engines);

    return engines;
}
