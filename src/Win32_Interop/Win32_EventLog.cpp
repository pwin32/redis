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

#include "win32_types.h"

#include <Windows.h>
#include <cerrno>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <iostream>
#include <sstream>
using namespace std;

#include "Win32_EventLog.h"
#include "Win32_Error.h"
#include "Win32_SmartHandle.h"
#include "EventLog.h"

static bool eventLogEnabled = true;
static string eventLogIdentity = "redis";

void RedisEventLog::SetEventLogIdentity(const char* identity) {
    eventLogIdentity = string(identity);
}

void RedisEventLog::UninstallEventLogSource() {
    static const wchar_t eventLogApplicationPath[] = L"SYSTEM\\CurrentControlSet\\Services\\EventLog\\Application\\";
    static const wchar_t eventLogPath[] = L"SYSTEM\\CurrentControlSet\\Services\\EventLog\\";
    static const wchar_t eventLogName[] = L"redis";
    static const wchar_t redisServer[] = L"redis-server";
    SmartRegistryHandle appKey;
    if (ERROR_SUCCESS == RegOpenKeyW(HKEY_LOCAL_MACHINE, eventLogApplicationPath, appKey)) {
        SmartRegistryHandle eventLogNameKey;
        if (ERROR_SUCCESS == RegOpenKeyW(appKey, eventLogName, eventLogNameKey)) {
            LSTATUS status = RegDeleteKeyW(appKey, eventLogName);
            if (ERROR_SUCCESS != status) {
                throw std::system_error(status, system_category(), "RegDeleteKeyW failed");
            }
        }
    }

    SmartRegistryHandle eventLogKey;
    if (ERROR_SUCCESS == RegOpenKeyW(HKEY_LOCAL_MACHINE, eventLogPath, eventLogKey)) {
        SmartRegistryHandle eventServiceKey;
        if (ERROR_SUCCESS == RegOpenKeyW(eventLogKey, eventLogName, eventServiceKey)) {
            SmartRegistryHandle eventServiceSubKey;
            if (ERROR_SUCCESS == RegOpenKeyW(eventServiceKey, redisServer, eventServiceSubKey)) {
                LSTATUS status = RegDeleteKeyW(eventServiceKey, redisServer);
                if (ERROR_SUCCESS != status) {
                    throw std::system_error(status, system_category(), "RegDeleteKeyW failed");
                }
                status = RegDeleteKeyW(eventLogKey, eventLogName);
                if (ERROR_SUCCESS != status) {
                    throw std::system_error(status, system_category(), "RegDeleteKeyW failed");
                }
            }
        }
    }
}

// sets up the registry keys required for the EventViewer message filter
void RedisEventLog::InstallEventLogSource(string appPath) {
    static const wchar_t eventLogPath[] = L"SYSTEM\\CurrentControlSet\\Services\\EventLog\\";
    static const wchar_t eventLogName[] = L"redis";
    static const wchar_t application[] = L"Application";
    static const wchar_t typesSupported[] = L"TypesSupported";
    static const wchar_t eventMessageFile[] = L"EventMessageFile";
    wchar_t *wideAppPath = win32_utf8_path_to_wide(appPath.c_str());
    LSTATUS status;

    if (wideAppPath == NULL) {
        throw std::system_error(errno, generic_category(),
                                "UTF-8 event log path conversion failed");
    }

    SmartRegistryHandle eventLogKey;
    status = RegOpenKeyW(HKEY_LOCAL_MACHINE, eventLogPath, eventLogKey);
    if (ERROR_SUCCESS != status) {
        win32_free(wideAppPath);
        throw std::system_error(status, system_category(), "RegOpenKeyW failed");
    }
    DWORD value = EVENTLOG_ERROR_TYPE | EVENTLOG_WARNING_TYPE |
                  EVENTLOG_INFORMATION_TYPE;
    size_t appPathBytesSize = (wcslen(wideAppPath) + 1) * sizeof(wchar_t);
    if (appPathBytesSize > MAXDWORD) {
        win32_free(wideAppPath);
        throw std::length_error("Event log message path is too long");
    }
    DWORD appPathBytes = (DWORD)appPathBytesSize;

    /* Register redis as an Application source.  Do not also create a
     * top-level EventLog\\redis key: RegisterEventSourceW receives only a
     * source name, so a same-named custom log makes its destination
     * ambiguous and can divert records away from Application.  The legacy
     * custom-log keys are still removed by UninstallEventLogSource(). */
    SmartRegistryHandle applicationKey;
    status = RegOpenKeyW(eventLogKey, application, applicationKey);
    if (ERROR_SUCCESS != status) {
        win32_free(wideAppPath);
        throw std::system_error(status, system_category(), "RegOpenKeyW failed");
    }
    SmartRegistryHandle redis2;
    if (ERROR_SUCCESS != RegOpenKeyW(applicationKey, eventLogName, redis2)) {
        status = RegCreateKeyW(applicationKey, eventLogName, redis2);
        if (ERROR_SUCCESS != status) {
            win32_free(wideAppPath);
            throw std::system_error(status, system_category(), "RegCreateKeyW failed");
        }
    }
    status = RegSetValueExW(redis2, typesSupported, 0, REG_DWORD,
                            (const BYTE*) &value, sizeof(DWORD));
    if (ERROR_SUCCESS != status) {
        win32_free(wideAppPath);
        throw std::system_error(status, system_category(), "RegSetValueExW failed");
    }
    status = RegSetValueExW(redis2, eventMessageFile, 0, REG_SZ,
                            (const BYTE*)wideAppPath, appPathBytes);
    if (ERROR_SUCCESS != status) {
        win32_free(wideAppPath);
        throw std::system_error(status, system_category(), "RegSetValueExW failed");
    }
    win32_free(wideAppPath);
}

int RedisEventLog::LogMessage(LPCSTR msg, const WORD type) {
    DWORD eventID;
    switch (type) {
        case EVENTLOG_ERROR_TYPE:
            eventID = MSG_ERROR_1;
            break;
        case EVENTLOG_WARNING_TYPE:
            eventID = MSG_WARNING_1;
            break;
        case EVENTLOG_INFORMATION_TYPE:
            eventID = MSG_INFO_1;
            break;
        default:
            std::cerr << "Unrecognized type: " << type << "\n";
            eventID = MSG_INFO_1;
            break;
    }

    wchar_t *wideMessage = win32_utf8_to_wide(msg);
    if (wideMessage == NULL) {
        DWORD error = GetLastError();
        std::cerr << "Failed to convert event log message from UTF-8\n";
        return error == ERROR_SUCCESS ? ERROR_NO_UNICODE_TRANSLATION :
                                        (int)error;
    }
    HANDLE hEventLog = RegisterEventSourceW(0, L"redis");
    int result = ERROR_SUCCESS;

    if (0 == hEventLog) {
        result = (int)GetLastError();
        std::cerr << "Failed open source '" << this->eventLogName << "': " << result << endl;
    } else {
        LPCWSTR messages[] = {wideMessage};
        if (FALSE == ReportEventW(hEventLog, type, 0, eventID, 0, 1, 0,
                                  messages, 0)) {
            result = (int)GetLastError();
            std::cerr << "Failed to write message: " << result << endl;
        }

        DeregisterEventSource(hEventLog);
    }
    win32_free(wideMessage);
    return result;
}

void RedisEventLog::LogError(string msg) {
    try {
        if (eventLogEnabled == true) {
            stringstream ss;
            ss << "syslog-ident = " << eventLogIdentity << endl;
            ss << msg;
            RedisEventLog().LogMessage(ss.str().c_str(), EVENTLOG_ERROR_TYPE);
        }
    }
    catch (...) {
    }
}

string RedisEventLog::GetEventLogIdentity() {
    return eventLogIdentity;
}

void RedisEventLog::EnableEventLog(bool enabled) {
    eventLogEnabled = enabled;
}

bool RedisEventLog::IsEventLogEnabled() {
    return eventLogEnabled;
}

extern "C" void setSyslogEnabled(int enabled) {
    try {
        if (enabled == 1) {
            RedisEventLog().EnableEventLog(true);
        } else {
            RedisEventLog().EnableEventLog(false);
        }
    }
    catch (...) {}
}

extern "C" void setSyslogIdent(char* identity) {
    try {
        RedisEventLog().SetEventLogIdentity(identity);
    }
    catch (...) {}
}

extern "C" int WriteEventLog(const char* msg) {
    try {
        stringstream ss;
        ss << "syslog-ident = " << RedisEventLog().GetEventLogIdentity() << endl;
        ss << msg;
        return RedisEventLog().LogMessage(ss.str().c_str(), EVENTLOG_INFORMATION_TYPE);
    }
    catch (...) { return ERROR_GEN_FAILURE; }
}

extern "C" int IsEventLogEnabled() {
    try {
        if (RedisEventLog().IsEventLogEnabled() == true) {
            return 1;
        }
    }
    catch (...) {}
    return 0;
}
