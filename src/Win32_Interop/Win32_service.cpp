/*
* Copyright (c), Microsoft Open Technologies, Inc.
* All rights reserved.
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*  - Redistributions of source code must retain the above copyright notice,
*    this list of conditions and the following disclaimer.
*  - Redistributions in binary form must reproduce the above copyright notice,
*    this list of conditions and the following disclaimer in the documentation
*    and/or other materials provided with the distribution.
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
* FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
* DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
* SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
* CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
* OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
* OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
This code implements the following new command line arguments for redis:

--service-install [additional command line arguments to pass to redis when launched as a service]

This must be the first argument on the redis-server command line. Arguments after this are passed in the order they occur to redis when the
service is launched. The service will be configured as Autostart and will be launched as "NT AUTHORITY\NetworkService". Upon successful
installation a success message will be displayed and redis will exit. For instance:

redis-server --service-install redis.conf --loglevel verbose

This command does not start the service.

--service-uninstall

This will remove the redis service configuration information from the registry. Upon successful uninstallation a success message will be
displayed and redis will exit.

This does command not stop the service.

--service-start

This will start the redis service. Upon successful startup a success message will be displayed and redis will exit.

--service-stop

This will stop the redis service. Upon successful termination a success message will be displayed and redis will exit.

The [--service-name name] arguments, modifies the preceding commands to target a specific service name. If present, 
this should preceed the other arguments passed to redis. For instance:

    redis-server --service-install --service-name testServiceName redis.windows.conf --loglevel verbose 
*/

#include "win32_types.h"
#include <windows.h>
#include <windowsx.h>
#include <shlobj.h>
#include <shellapi.h>
#include <tchar.h>
#include <strsafe.h>
#include <aclapi.h>
#include "Win32_EventLog.h"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <string>
#include <sstream>
#include <vector>
#include <iostream>
#include <process.h>
#include "Win32_RedisLog.h"
#include "Win32_CommandLine.h"
#include "Win32_Error.h"
using namespace std;

#include "Win32_SmartHandle.h"

#pragma comment(lib, "advapi32.lib")

#define DEFAULT_SERVICE_NAME "Redis"  
#define MAX_SERVICE_NAME_LENGTH 256
string g_serviceName = DEFAULT_SERVICE_NAME;
wstring g_serviceNameWide = L"Redis";

SERVICE_STATUS g_ServiceStatus = {};
HANDLE g_ServiceStopEvent = INVALID_HANDLE_VALUE;
HANDLE g_ServiceStoppedEvent = INVALID_HANDLE_VALUE;
HANDLE g_ServiceReadyEvent = INVALID_HANDLE_VALUE;
vector<string> serviceRunArguments;
SERVICE_STATUS_HANDLE g_StatusHandle;
const ULONGLONG cThirtySeconds = 30 * 1000;
BOOL g_isRunningAsService = FALSE;
const int cPreshutdownInterval = 180000;
const wchar_t* cServiceInstallPipeName = L"\\\\.\\pipe\\redis-service-install";

extern "C" int main(int argc, char** argv);

typedef class ServicePipeWriter {
public:
    static ServicePipeWriter& getInstance() {
        static ServicePipeWriter    instance;
        return instance;
    }

private:
    HANDLE pipe = INVALID_HANDLE_VALUE;
    ServicePipeWriter() {
        pipe = CreateFileW(cServiceInstallPipeName, GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, NULL);
    }
    ServicePipeWriter(ServicePipeWriter const&);
    void operator=(ServicePipeWriter const&);
    ~ServicePipeWriter() {
        if (pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
        }
    }

public:
    void Write(string message) {
        if (pipe != INVALID_HANDLE_VALUE) {
            size_t offset = 0;
            while (offset < message.length()) {
                DWORD bytesWritten = 0;
                DWORD remaining = (DWORD)min<size_t>(message.length() - offset,
                                                      MAXDWORD);
                if (!WriteFile(pipe, message.data() + offset, remaining,
                               &bytesWritten, NULL) || bytesWritten == 0) {
                    ::serverLog(LL_WARNING,
                                "Unable to write service command output: %lu",
                                (unsigned long)GetLastError());
                    break;
                }
                offset += bytesWritten;
            }
        } else {
            ::serverLog(LL_WARNING, "%s", message.c_str());
        }
    }
} ServicePipeWriter;

static wstring Utf8ToWideString(const string& value) {
    wchar_t *wide = win32_utf8_to_wide(value.c_str());
    if (wide == NULL) {
        throw std::system_error(errno, generic_category(),
                                "UTF-8 to UTF-16 conversion failed");
    }
    wstring result(wide);
    win32_free(wide);
    return result;
}

static string WideToUtf8String(const wstring& value) {
    char *utf8 = win32_wide_to_utf8(value.c_str());
    if (utf8 == NULL) {
        throw std::system_error(errno, generic_category(),
                                "UTF-16 to UTF-8 conversion failed");
    }
    string result(utf8);
    win32_free(utf8);
    return result;
}

static void OutputDebugStringUtf8(const string& value) {
    wchar_t *wide = win32_utf8_to_wide(value.c_str());
    if (wide == NULL) return;
    OutputDebugStringW(wide);
    win32_free(wide);
}

static void AppendQuotedArgument(wstring& commandLine, const wstring& argument) {
    size_t backslashes = 0;

    commandLine.push_back(L'"');
    for (wchar_t c : argument) {
        if (c == L'\\') {
            backslashes++;
            continue;
        }
        if (c == L'"') {
            commandLine.append(backslashes * 2 + 1, L'\\');
            commandLine.push_back(L'"');
            backslashes = 0;
            continue;
        }
        commandLine.append(backslashes, L'\\');
        backslashes = 0;
        commandLine.push_back(c);
    }
    commandLine.append(backslashes * 2, L'\\');
    commandLine.push_back(L'"');
}

static wstring BuildWindowsCommandLine(const vector<wstring>& arguments) {
    wstring commandLine;
    for (size_t index = 0; index < arguments.size(); index++) {
        if (index != 0) commandLine.push_back(L' ');
        AppendQuotedArgument(commandLine, arguments[index]);
    }
    return commandLine;
}

static wstring GetServiceExecutablePathWide() {
    wchar_t *path = win32_get_module_filename_wide();
    if (path == NULL) {
        throw std::system_error(errno, generic_category(),
                                "GetModuleFileNameW failed");
    }
    wstring result(path);
    win32_free(path);
    return result;
}

static void DrainServicePipe(HANDLE pipe) {
    const DWORD messageBufferSize = 10000;
    char buffer[messageBufferSize + 1];

    for (;;) {
        DWORD bytesRead = 0;
        if (!ReadFile(pipe, buffer, messageBufferSize, &bytesRead, NULL)) {
            DWORD error = GetLastError();
            if (error == ERROR_NO_DATA || error == ERROR_PIPE_LISTENING ||
                error == ERROR_PIPE_NOT_CONNECTED ||
                error == ERROR_BROKEN_PIPE) return;
            throw std::system_error(error, system_category(), "ReadFile failed");
        }
        if (bytesRead == 0) return;
        buffer[bytesRead] = '\0';
        ::serverLog(LL_WARNING, "%s", buffer);
    }
}

BOOL RelaunchAsElevatedProcess(int argc, char** argv) {
    // create pipe for launched process to communicate back on
    HANDLE rawPipe = CreateNamedPipeW(
        cServiceInstallPipeName, PIPE_ACCESS_INBOUND,
        PIPE_TYPE_BYTE, 1, 0, 0, PIPE_NOWAIT, NULL);
    if (rawPipe == INVALID_HANDLE_VALUE) {
        throw std::system_error(GetLastError(), system_category(),
                                "CreateNamedPipeW failed");
    }
    SmartHandle pipe(rawPipe);

    vector<wstring> parameterArguments;
    for (int n = 1; n < argc; n++) {
        parameterArguments.push_back(Utf8ToWideString(argv[n]));
    }
    wstring params = BuildWindowsCommandLine(parameterArguments);
    if (params.length() >= 32767) {
        throw std::length_error("Elevated command line exceeds Windows limit");
    }
    wstring executable = GetServiceExecutablePathWide();

    // Launch itself as administrator.
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(SHELLEXECUTEINFOW);
    sei.lpVerb = L"runas";
    sei.lpFile = executable.c_str();
    sei.lpParameters = params.c_str();
    sei.hwnd = 0;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpDirectory = 0;
    sei.hInstApp = 0;

    if (ShellExecuteExW(&sei)) {
        if (sei.hProcess != NULL) {
            for (;;) {
                DrainServicePipe(pipe);
                DWORD waitResult = WaitForSingleObject(sei.hProcess, 50);
                if (waitResult == WAIT_OBJECT_0) break;
                if (waitResult == WAIT_FAILED) {
                    DWORD error = GetLastError();
                    CloseHandle(sei.hProcess);
                    throw std::system_error(error, system_category(),
                                            "WaitForSingleObject failed");
                }
            }
            DrainServicePipe(pipe);
            CloseHandle(sei.hProcess);
        }
        return TRUE;
    } else {
        throw std::system_error(GetLastError(), system_category(), "ShellExecuteExW failed");
    }
}

bool IsProcessElevated() {
    SmartHandle shToken;

    // Open the primary access token of the process with TOKEN_QUERY.
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, shToken)) {
        throw std::system_error(GetLastError(), system_category(), "OpenProcessTokenFailed failed");
    }

    // Retrieve token elevation information.
    TOKEN_ELEVATION elevation;
    DWORD dwSize;
    if (!GetTokenInformation(shToken, TokenElevation, &elevation,
        sizeof(elevation), &dwSize)) {
        throw std::system_error(GetLastError(), system_category(), "OpenProcessTokenFailed failed");
    }

    return  (elevation.TokenIsElevated != 0);
}

VOID InitializeServiceName() {
    if (g_argMap.find(cServiceName) != g_argMap.end()) {
        string requested = g_argMap[cServiceName].at(0).at(0);
        wstring requestedWide = Utf8ToWideString(requested);
        if (requestedWide.length() > MAX_SERVICE_NAME_LENGTH) {
            throw std::runtime_error("Service name too long.");
        }
        g_serviceName = requested;
        g_serviceNameWide = requestedWide;
    }
}

DWORD AddAceToObjectsSecurityDescriptor(
    LPWSTR pszObjName,
    SE_OBJECT_TYPE ObjectType,  
    LPWSTR pszTrustee,
    TRUSTEE_FORM TrusteeForm,   
    DWORD dwAccessRights,       
    ACCESS_MODE AccessMode,     
    DWORD dwInheritance         
    ) {
    DWORD dwRes = 0;
    PACL pOldDACL = NULL, pNewDACL = NULL;
    PSECURITY_DESCRIPTOR pSD = NULL;
    EXPLICIT_ACCESSW ea;

    if (NULL == pszObjName)
        return ERROR_INVALID_PARAMETER;

    dwRes = GetNamedSecurityInfoW(pszObjName, ObjectType,
        DACL_SECURITY_INFORMATION,
        NULL, NULL, &pOldDACL, NULL, &pSD);
    if (ERROR_SUCCESS != dwRes) {
        ::serverLog(LL_WARNING, "GetNamedSecurityInfo Error %u\n", dwRes);
        goto Cleanup;
    }

    ZeroMemory(&ea, sizeof(ea));
    ea.grfAccessPermissions = dwAccessRights;
    ea.grfAccessMode = AccessMode;
    ea.grfInheritance = dwInheritance;
    ea.Trustee.TrusteeForm = TrusteeForm;
    ea.Trustee.ptstrName = pszTrustee;

    dwRes = SetEntriesInAclW(1, &ea, pOldDACL, &pNewDACL);
    if (ERROR_SUCCESS != dwRes) {
        ::serverLog(LL_WARNING, "SetEntriesInAcl Error %u\n", dwRes);
        goto Cleanup;
    }

    dwRes = SetNamedSecurityInfoW(pszObjName, ObjectType,
        DACL_SECURITY_INFORMATION,
        NULL, NULL, pNewDACL, NULL);
    if (ERROR_SUCCESS != dwRes) {
        ::serverLog(LL_WARNING, "SetNamedSecurityInfo Error %u\n", dwRes);
        goto Cleanup;
    }

Cleanup:

    if (pSD != NULL)
        LocalFree((HLOCAL)pSD);
    if (pNewDACL != NULL)
        LocalFree((HLOCAL)pNewDACL);

    return dwRes;
}

VOID SetAccessACLOnFolder(string user, string folder) {
    wstring wideUser = Utf8ToWideString(user);
    wchar_t *wideFolder = win32_utf8_path_to_wide(folder.c_str());
    DWORD status;
    if (wideFolder == NULL) {
        throw std::system_error(errno, generic_category(),
                                "ServiceInstall: folder path conversion failed");
    }
    status = AddAceToObjectsSecurityDescriptor(
        wideFolder, SE_OBJECT_TYPE::SE_FILE_OBJECT,
        const_cast<LPWSTR>(wideUser.c_str()), TRUSTEE_FORM::TRUSTEE_IS_NAME,
        GENERIC_ALL, GRANT_ACCESS, SUB_CONTAINERS_AND_OBJECTS_INHERIT);
    win32_free(wideFolder);
    if (status != ERROR_SUCCESS) {
        throw std::system_error(status, system_category(),
                                "ServiceInstall: AddAceToObjectsSecurityDescriptor failed");
    }
}

VOID ServiceInstall(int argc, char ** argv) {
    SmartServiceHandle shSCManager;
    SmartServiceHandle shService;
    string userName = "NT AUTHORITY\\NetworkService";
    wstring wideUserName = Utf8ToWideString(userName);

    InitializeServiceName();

    // build arguments to pass to service when it auto starts
    wstring executablePath = GetServiceExecutablePathWide();
    vector<wstring> serviceArguments;
    for (int a = 0; a < argc; a++) {
        if (a == 0) {
            serviceArguments.push_back(executablePath);
        } else if (a == 1) {
            // replace --service-install argument with --service-run
            serviceArguments.push_back(Utf8ToWideString("--" + cServiceRun));
        } else {
            serviceArguments.push_back(Utf8ToWideString(argv[a]));
        }
    }
    wstring serviceCommandLine = BuildWindowsCommandLine(serviceArguments);

    shSCManager = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (shSCManager.Invalid()) {
        throw std::system_error(GetLastError(), system_category(), "OpenSCManager failed");
    }

    shService = CreateServiceW(
        shSCManager,
        g_serviceNameWide.c_str(),
        g_serviceNameWide.c_str(),
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        serviceCommandLine.c_str(),
        NULL, NULL, NULL,
        wideUserName.c_str(),
        NULL);
    if (shService.Invalid()) {
        throw std::system_error(GetLastError(), system_category(), "CreateService failed");
    }

    SERVICE_PRESHUTDOWN_INFO preshutdownInfo;
    preshutdownInfo.dwPreshutdownTimeout = cPreshutdownInterval;
    if (FALSE == ChangeServiceConfig2W(shService, SERVICE_CONFIG_PRESHUTDOWN_INFO, &preshutdownInfo)) {
        throw std::system_error(GetLastError(), system_category(), "ChangeServiceConfig2 failed");
    }

    RedisEventLog().InstallEventLogSource(WideToUtf8String(executablePath));

    // make sure NT AUTHORITY\\NetworkService" has rights to every directory where a files may be accessed (CONF,AOF,RDB,DAT)
    stringstream aceMessage;
    aceMessage << "Granting read/write access to 'NT AUTHORITY\\NetworkService' on: ";
    for (auto folder : GetAccessPaths()) {
        SetAccessACLOnFolder(userName, folder);
        aceMessage << "\"" << folder.c_str() << "\" ";
    }
    ServicePipeWriter::getInstance().Write(aceMessage.str().c_str());

    ServicePipeWriter::getInstance().Write("Redis successfully installed as a service.");
}

VOID ServiceStart(int argc, char ** argv) {
    SmartServiceHandle shSCManager;
    SmartServiceHandle shService;

    InitializeServiceName();

    shSCManager = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (shSCManager.Invalid()) {
        throw std::system_error(GetLastError(), system_category(), "OpenSCManager failed");
    }
    shService = OpenServiceW(shSCManager, g_serviceNameWide.c_str(), SERVICE_ALL_ACCESS);
    if (shService.Invalid()) {
        throw std::system_error(GetLastError(), system_category(), "OpenService failed");
    }
    if (FALSE == StartServiceW(shService, 0, NULL)) {
        throw std::system_error(GetLastError(), system_category(), "StartService failed");
    }

    // it will take atleast a couple of seconds for the service to start.
    Sleep(2000);

    SERVICE_STATUS status;
    DWORD start = GetTickCount();
    for (;;) {
        if (QueryServiceStatus(shService, &status) == FALSE) {
            throw std::system_error(GetLastError(), system_category(),
                                    "QueryServiceStatus failed");
        }
        if (status.dwCurrentState == SERVICE_RUNNING) {
            ServicePipeWriter::getInstance().Write("Redis service successfully started.");
            break;
        } else if (status.dwCurrentState == SERVICE_STOPPED) {
            DWORD error = status.dwWin32ExitCode == ERROR_SUCCESS ?
                          ERROR_SERVICE_NOT_ACTIVE : status.dwWin32ExitCode;
            throw std::system_error(error, system_category(),
                                    "Redis service failed to start");
        }

        DWORD current = GetTickCount();
        if (current - start >= cThirtySeconds) {
            throw std::system_error(ERROR_SERVICE_REQUEST_TIMEOUT,
                                    system_category(),
                                    "Redis service start timed out");
        }
        Sleep(100);
    }

}

VOID ServiceStop(int argc, char ** argv) {
    SmartServiceHandle shSCManager;
    SmartServiceHandle shService;

    InitializeServiceName();

    shSCManager = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (shSCManager.Invalid()) {
        throw std::system_error(GetLastError(), system_category(), "OpenSCManager failed");
    }
    shService = OpenServiceW(shSCManager, g_serviceNameWide.c_str(), SERVICE_ALL_ACCESS);
    if (shService.Invalid()) {
        throw std::system_error(GetLastError(), system_category(), "OpenService failed");
    }
    SERVICE_STATUS status;
    if (FALSE == ControlService(shService, SERVICE_CONTROL_STOP, &status)) {
        throw std::system_error(GetLastError(), system_category(), "ControlService failed");
    }

    DWORD start = GetTickCount();
    for (;;) {
        if (QueryServiceStatus(shService, &status) == FALSE) {
            throw std::system_error(GetLastError(), system_category(),
                                    "QueryServiceStatus failed");
        }
        if (status.dwCurrentState == SERVICE_STOPPED) {
            ServicePipeWriter::getInstance().Write("Redis service successfully stopped.");
            break;
        }
        DWORD current = GetTickCount();
        if (current - start >= cThirtySeconds) {
            throw std::system_error(ERROR_SERVICE_REQUEST_TIMEOUT,
                                    system_category(),
                                    "Redis service stop timed out");
        }
        Sleep(100);
    }
}

VOID ServiceUninstall(int argc, char** argv) {
    SmartServiceHandle shSCManager;
    SmartServiceHandle shService;

    InitializeServiceName();

    shSCManager = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (shSCManager.Invalid()) {
        throw std::system_error(GetLastError(), system_category(), "OpenSCManager failed");
    }
    shService = OpenServiceW(shSCManager, g_serviceNameWide.c_str(), SERVICE_ALL_ACCESS);
    if (shService.Valid()) {
        if (FALSE == DeleteService(shService)) {
            throw std::system_error(GetLastError(), system_category(), "DeleteService failed");
        }
    }

    RedisEventLog().UninstallEventLogSource();

    ServicePipeWriter::getInstance().Write("Redis service successfully uninstalled.");
}

static string GetServiceExecutablePath() {
    return WideToUtf8String(GetServiceExecutablePathWide());
}

unsigned __stdcall ServiceWorkerThread(void *lpParam) {
    (void)lpParam;
    try {
        if (serviceRunArguments.size() > (size_t)INT_MAX) {
            throw std::length_error("Service argument count exceeds INT_MAX");
        }
        int argc = (int)(serviceRunArguments.size());
        vector<vector<char>> argumentStorage;
        vector<char *> redisArgv;
        argumentStorage.reserve(argc);
        redisArgv.reserve(argc + 1);
        for (const string& arg : serviceRunArguments) {
            argumentStorage.emplace_back(arg.begin(), arg.end());
            argumentStorage.back().push_back('\0');
            redisArgv.push_back(argumentStorage.back().data());
        }
        redisArgv.push_back(nullptr);

        // When the service starts the current directory is %systemdir%. If the launching user does not have permission there(i.e., NETWORK SERVICE), the 
        // memory mapped file will not be able to be created. Thus Redis will fail to start. Setting the current directory to the executable directory
        // should fix this.
        string currentDir = GetServiceExecutablePath();
        auto pos = currentDir.find_last_of("\\/");
        if (pos != string::npos) currentDir.erase(pos);

        if (win32_set_current_directory_utf8(currentDir.c_str()) != 0) {
            throw std::system_error(errno, generic_category(),
                                    "SetCurrentDirectoryW failed");
        }

        // call redis main without the --service-run argument
        int result = main(argc, redisArgv.data());
        if (g_ServiceStoppedEvent != INVALID_HANDLE_VALUE && g_ServiceStoppedEvent != NULL)
            SetEvent(g_ServiceStoppedEvent);
        return result == 0 ? ERROR_SUCCESS :
               (result > 0 ? (DWORD)result : ERROR_PROCESS_ABORTED);
    } catch (const std::system_error& syserr) {
        stringstream err;
        err << "ServiceWorkerThread: system error caught. error code=0x" << hex << syserr.code().value() << ", message = " << syserr.what() << endl;
        OutputDebugStringUtf8(err.str());
    } catch (const std::runtime_error& runerr) {
        stringstream err;
        err << "runtime error caught. message=" << runerr.what() << endl;
        OutputDebugStringUtf8(err.str());
    } catch (...) {
        OutputDebugStringW(L"ServiceWorkerThread: other exception caught.\n");
    }

    if (g_ServiceStoppedEvent != INVALID_HANDLE_VALUE && g_ServiceStoppedEvent != NULL)
        SetEvent(g_ServiceStoppedEvent);
    return ERROR_PROCESS_ABORTED;
}

DWORD WINAPI ServiceCtrlHandler(DWORD dwControl, DWORD dwEventType, LPVOID lpEventData, LPVOID lpContext) {
    (void)dwEventType;
    (void)lpEventData;
    (void)lpContext;
    try {
        switch (dwControl) {
        case SERVICE_CONTROL_PRESHUTDOWN:
        {
            if (SetEvent(g_ServiceStopEvent) == FALSE)
                return GetLastError();

            g_ServiceStatus.dwControlsAccepted = 0;
            g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
            g_ServiceStatus.dwWin32ExitCode = 0;
            g_ServiceStatus.dwCheckPoint = 4;
            g_ServiceStatus.dwWaitHint = cPreshutdownInterval;

            if (SetServiceStatus(g_StatusHandle, &g_ServiceStatus) == FALSE) {
                return GetLastError();
            }

            break;
        }

        case SERVICE_CONTROL_STOP:
        {
            /* Redis polls g_ServiceStopEvent from its event loop. Report the
             * pending state here and let ServiceMain publish SERVICE_STOPPED
             * only after the worker thread has actually exited. */
            g_ServiceStatus.dwControlsAccepted = 0;
            g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
            g_ServiceStatus.dwWin32ExitCode = 0;
            g_ServiceStatus.dwCheckPoint = 1;
            g_ServiceStatus.dwWaitHint = cPreshutdownInterval;

            if (SetServiceStatus(g_StatusHandle, &g_ServiceStatus) == FALSE) {
                return GetLastError();
            }

            if (SetEvent(g_ServiceStopEvent) == FALSE) {
                DWORD error = GetLastError();
                g_ServiceStatus.dwControlsAccepted =
                    SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_PRESHUTDOWN;
                g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
                g_ServiceStatus.dwCheckPoint = 0;
                g_ServiceStatus.dwWaitHint = 0;
                SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
                return error;
            }
            break;
        }

        default:
        {
                   break;
        }
        }
        return NO_ERROR;
    } catch (...) {
        return ERROR_EXCEPTION_IN_SERVICE;
    }
}

VOID WINAPI ServiceMain(DWORD argc, LPWSTR *argv) {
    (void)argc;
    (void)argv;
    DWORD failureCode = ERROR_SUCCESS;
    HANDLE hThread = NULL;

    try {
        g_StatusHandle = RegisterServiceCtrlHandlerExW(g_serviceNameWide.c_str(), ServiceCtrlHandler, NULL);
        if (g_StatusHandle == NULL) return;

        ZeroMemory(&g_ServiceStatus, sizeof(g_ServiceStatus));
        g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        g_ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
        g_ServiceStatus.dwWaitHint = cPreshutdownInterval;
        if (!SetServiceStatus(g_StatusHandle, &g_ServiceStatus)) return;

        g_ServiceStoppedEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        g_ServiceStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        g_ServiceReadyEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        if (g_ServiceStoppedEvent == NULL || g_ServiceStopEvent == NULL ||
            g_ServiceReadyEvent == NULL) {
            failureCode = GetLastError();
            goto service_cleanup;
        }

        g_ServiceStatus.dwCheckPoint = 1;
        if (!SetServiceStatus(g_StatusHandle, &g_ServiceStatus)) {
            failureCode = GetLastError();
            goto service_cleanup;
        }

        {
            uintptr_t thread = _beginthreadex(NULL, 0, ServiceWorkerThread, NULL, 0, NULL);
            if (thread == 0) {
                failureCode = GetLastError();
                if (failureCode == ERROR_SUCCESS) failureCode = ERROR_SERVICE_REQUEST_TIMEOUT;
                goto service_cleanup;
            }
            hThread = (HANDLE)thread;
        }

        for (;;) {
            HANDLE waitHandles[2] = { g_ServiceReadyEvent, hThread };
            DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, 1000);
            if (waitResult == WAIT_OBJECT_0) {
                g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_PRESHUTDOWN;
                g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
                g_ServiceStatus.dwWin32ExitCode = ERROR_SUCCESS;
                g_ServiceStatus.dwCheckPoint = 0;
                g_ServiceStatus.dwWaitHint = 0;
                if (!SetServiceStatus(g_StatusHandle, &g_ServiceStatus))
                    failureCode = GetLastError();
                break;
            }
            if (waitResult == WAIT_OBJECT_0 + 1) {
                if (!GetExitCodeThread(hThread, &failureCode) || failureCode == STILL_ACTIVE)
                    failureCode = ERROR_PROCESS_ABORTED;
                break;
            }
            if (waitResult == WAIT_TIMEOUT) {
                g_ServiceStatus.dwCheckPoint++;
                g_ServiceStatus.dwWaitHint = cPreshutdownInterval;
                if (!SetServiceStatus(g_StatusHandle, &g_ServiceStatus)) {
                    failureCode = GetLastError();
                    break;
                }
                continue;
            }
            failureCode = GetLastError();
            break;
        }

        if (g_ServiceStatus.dwCurrentState == SERVICE_RUNNING) {
            DWORD waitResult = WaitForSingleObject(hThread, INFINITE);
            if (waitResult != WAIT_OBJECT_0) {
                failureCode = GetLastError();
            } else if (!GetExitCodeThread(hThread, &failureCode)) {
                failureCode = GetLastError();
            }
        }

service_cleanup:
        if (hThread != NULL) {
            CloseHandle(hThread);
            hThread = NULL;
        }
        if (g_ServiceReadyEvent != INVALID_HANDLE_VALUE && g_ServiceReadyEvent != NULL) {
            CloseHandle(g_ServiceReadyEvent);
            g_ServiceReadyEvent = INVALID_HANDLE_VALUE;
        }
        if (g_ServiceStoppedEvent != INVALID_HANDLE_VALUE && g_ServiceStoppedEvent != NULL) {
            CloseHandle(g_ServiceStoppedEvent);
            g_ServiceStoppedEvent = INVALID_HANDLE_VALUE;
        }
        if (g_ServiceStopEvent != INVALID_HANDLE_VALUE && g_ServiceStopEvent != NULL) {
            CloseHandle(g_ServiceStopEvent);
            g_ServiceStopEvent = INVALID_HANDLE_VALUE;
        }

        g_ServiceStatus.dwControlsAccepted = 0;
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceStatus.dwWin32ExitCode = failureCode;
        g_ServiceStatus.dwCheckPoint = 0;
        g_ServiceStatus.dwWaitHint = 0;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
    } catch (...) {
        if (hThread != NULL) CloseHandle(hThread);
        if (g_ServiceReadyEvent != INVALID_HANDLE_VALUE && g_ServiceReadyEvent != NULL) {
            CloseHandle(g_ServiceReadyEvent);
            g_ServiceReadyEvent = INVALID_HANDLE_VALUE;
        }
        if (g_ServiceStoppedEvent != INVALID_HANDLE_VALUE && g_ServiceStoppedEvent != NULL) {
            CloseHandle(g_ServiceStoppedEvent);
            g_ServiceStoppedEvent = INVALID_HANDLE_VALUE;
        }
        if (g_ServiceStopEvent != INVALID_HANDLE_VALUE && g_ServiceStopEvent != NULL) {
            CloseHandle(g_ServiceStopEvent);
            g_ServiceStopEvent = INVALID_HANDLE_VALUE;
        }
        g_ServiceStatus.dwControlsAccepted = 0;
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceStatus.dwWin32ExitCode = ERROR_EXCEPTION_IN_SERVICE;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
    }
}

void ServiceRun() {
    SERVICE_TABLE_ENTRYW ServiceTable[] =
    {
        { const_cast<LPWSTR>(g_serviceNameWide.c_str()),
          (LPSERVICE_MAIN_FUNCTIONW)ServiceMain },
        { NULL, NULL }
    };

    if (StartServiceCtrlDispatcherW(ServiceTable) == FALSE) {
        throw std::system_error(GetLastError(), system_category(), "StartServiceCtrlDispatcherW failed");
    }
}

void BuildServiceRunArguments(int argc, char** argv) {
    InitializeServiceName();
	string serviceNameFullArgument = "--" + cServiceName;
	serviceRunArguments.clear();

    // build argument list to be used by ServiceRun
    for (int n = 0; n < argc; n++) {
        if (n == 0) {
            serviceRunArguments.push_back(GetServiceExecutablePath());
        } else if (_stricmp(argv[n], ("--" + cServiceRun).c_str()) == 0) {
            continue;
        } else {
			if (_stricmp(argv[n], serviceNameFullArgument.c_str()) == 0) {
                // bypass --service-name argument and the name of the service
                if (n + 1 < argc) n++;
                continue; 
            } else {
                serviceRunArguments.push_back(argv[n]);
            }
        }
    }
}

extern "C" BOOL HandleServiceCommands(int argc, char **argv) {
    try {
        if (argc > 1) {
            string servicearg = string(argv[1]);
            servicearg = servicearg.substr(2, servicearg.length());
            std::transform(servicearg.begin(), servicearg.end(), servicearg.begin(), ::tolower);
            if (servicearg == cServiceInstall) {
                if (!IsProcessElevated()) {
                    return RelaunchAsElevatedProcess(argc, argv);
                } else {
                    ServiceInstall(argc, argv);
                    return TRUE;
                }
            } else if (servicearg == cServiceUninstall) {
                if (!IsProcessElevated()) {
                    return RelaunchAsElevatedProcess(argc, argv);
                } else {
                    ServiceUninstall(argc, argv);
                    return TRUE;
                }
            } else if (servicearg == cServiceRun) {
                g_isRunningAsService = TRUE;
                BuildServiceRunArguments(argc, argv);
                ServiceRun();
                return TRUE;
            } else if (servicearg == cServiceStart) {
                if (!IsProcessElevated()) {
                    return RelaunchAsElevatedProcess(argc, argv);
                } else {
                    ServiceStart(argc, argv);
                    return TRUE;
                }
            } else if (servicearg == cServiceStop) {
                if (!IsProcessElevated()) {
                    return RelaunchAsElevatedProcess(argc, argv);
                } else {
                    ServiceStop(argc, argv);
                    return TRUE;
                }
            }
        }

        // not a service command. start redis normally.
        return FALSE;
    } catch (const std::system_error& syserr) {
        stringstream ss;
        ss << "HandleServiceCommands: system error caught. error code=" << syserr.code().value() << ", message = " << syserr.what() << endl;
        ServicePipeWriter::getInstance().Write(ss.str());
        exit(1);
    } catch (const std::runtime_error& runerr) {
        stringstream err;
        err << "HandleServiceCommands: runtime error caught. message=" << runerr.what() << endl;
        ServicePipeWriter::getInstance().Write(err.str());
        exit(1);
    } catch (...) {
        stringstream ss;
        ss << "HandleServiceCommands: other exception caught." << endl;
        ServicePipeWriter::getInstance().Write(ss.str());
        exit(1);
    }
}

extern "C" BOOL ServiceStopIssued() {
    if (g_ServiceStopEvent == INVALID_HANDLE_VALUE) return FALSE;
    return (WaitForSingleObject(g_ServiceStopEvent, 0) == WAIT_OBJECT_0) ? TRUE : FALSE;
}

extern "C" void ServiceSetReady() {
    if (!g_isRunningAsService || g_ServiceReadyEvent == INVALID_HANDLE_VALUE ||
        g_ServiceReadyEvent == NULL) return;
    SetEvent(g_ServiceReadyEvent);
}

extern "C" BOOL RunningAsService() {
    return g_isRunningAsService;
}

extern "C" const char* GetServiceName()  {
    return g_serviceName.c_str();
}
