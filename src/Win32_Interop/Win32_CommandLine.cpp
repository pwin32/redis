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

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <exception>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "win32fixes.h"
#include <mswsock.h>

#include "Win32_Error.h"
#include "Win32_variadicFunctor.h"
#include "Win32_CommandLine.h"

// Win32_FDAPI.h includes modified winsock definitions that are useful in BindParam below. It
// also redefines the CRT close(FD) call as a macro. This conflicts with the fstream close
// definition. #undef solves the warning messages.
#undef close
#undef open

using namespace std;

ArgumentMap g_argMap;
vector<string> g_pathsAccessed;

string stripQuotes(string s) {
    if (s.length() >= 2) {
        if (s.at(0) == '\'' &&  s.at(s.length() - 1) == '\'') {
            if (s.length() > 2) {
                return s.substr(1, s.length() - 2);
            }
            else {
                return string("");
            }
        }
        if (s.at(0) == '\"' &&  s.at(s.length() - 1) == '\"') {
            if (s.length() > 2) {
                return s.substr(1, s.length() - 2);
            }
            else {
                return string("");
            }
        }
    }
    return s;
}

typedef class ParamExtractor {
public:
    ParamExtractor() {}
    virtual ~ParamExtractor() {}
    virtual vector<string> Extract(int argStartIndex, int argc, char** argv) = 0;
    virtual vector<string> Extract(vector<string> tokens, int StartIndex = 0) = 0;
} ParamExtractor;

typedef map<string, ParamExtractor*> RedisParameterMapper;

typedef class FixedParam : public ParamExtractor {
private:
    int parameterCount;

public:
    FixedParam(int count) {
        parameterCount = count;
    }

    vector<string> Extract(int argStartIndex, int argc, char** argv) {
        if (argStartIndex + parameterCount >= argc) {
            stringstream err;
            err << "Not enough parameters available for " << argv[argStartIndex];
            throw invalid_argument(err.str());
        }
        vector<string> params;
        for (int argIndex = argStartIndex + 1; argIndex < argStartIndex + 1 + parameterCount; argIndex++) {
            string param = string(argv[argIndex]);
            transform(param.begin(), param.end(), param.begin(), ::tolower);
            param = stripQuotes(param);
            params.push_back(param);
        }
        return params;
    }

    vector<string> Extract(vector<string> tokens, int startIndex = 0) {
        if (startIndex < 0 || parameterCount < 0 ||
            (size_t)startIndex + (size_t)parameterCount >= tokens.size()) {
            stringstream err;
            err << "Not enough parameters available";
            if (!tokens.empty()) err << " for " << tokens[0];
            throw invalid_argument(err.str());
        }
        vector<string> params;
        int skipCount = 1 + startIndex;
        for (string token : tokens) {
            if (skipCount > 0) {
                skipCount--;
                continue;
            }
            string param = string(token);
            transform(param.begin(), param.end(), param.begin(), ::tolower);
            param = stripQuotes(param);
            params.push_back(param);
        }
        return params;
    };
} FixedParam;

static FixedParam fp0 = FixedParam(0);
static FixedParam fp1 = FixedParam(1);
static FixedParam fp2 = FixedParam(2);
static FixedParam fp3 = FixedParam(3);
static FixedParam fp4 = FixedParam(4);

typedef class SaveParams : public ParamExtractor {
public:
    SaveParams() {}

    bool isStringAnInt(string test) {
        int x;
        char c;
        istringstream s(test);

        if (!(s >> x) ||            // not convertable to an int
            (s >> c)) {             // some character past the int
            return false;
        }
        else {
            return true;
        }
    }

    vector<string> Extract(int argStartIndex, int argc, char** argv) {
        vector<string> params;
        int argIndex = argStartIndex + 1;

        // save [seconds] [changes]
        // or
        // save ""      -- turns off RDB persistence
        if (argIndex >= argc) {
            stringstream err;
            err << "Not enough parameters available for " << argv[argStartIndex];
            throw invalid_argument(err.str());
        }
        if (strcmp(argv[argIndex], "\"\"") == 0 || strcmp(argv[argIndex], "''") == 0 || strcmp(argv[argIndex], "") == 0) {
            params.push_back(argv[argIndex]);
        }
        else if (argIndex + 1 < argc &&
            isStringAnInt(argv[argIndex]) &&
            isStringAnInt(argv[argIndex + 1])) {
            params.push_back(argv[argIndex]);
            params.push_back(argv[argIndex + 1]);
        }
        else {
            stringstream err;
            err << "Not enough parameters available for " << argv[argStartIndex];
            throw invalid_argument(err.str());
        }
        return params;
    }

    virtual vector<string> Extract(vector<string> tokens, int startIndex = 0) {
        vector<string> params;
        unsigned int parameterIndex = 1 + startIndex;
        if (tokens.size() == parameterIndex) {
            /* Redis accepts a bare save directive in configuration files as
             * the empty schedule, which disables automatic snapshots. */
            params.push_back("");
        }
        else if ((tokens.size() > parameterIndex) &&
            (tokens.at(parameterIndex) == string("\"\"") ||
                tokens.at(parameterIndex) == string("''"))) {
            params.push_back(tokens.at(parameterIndex));
        }
        else if ((tokens.size() > parameterIndex + 1) &&
            isStringAnInt(tokens.at(parameterIndex)) &&
            isStringAnInt(tokens.at(parameterIndex + 1))) {
            params.push_back(tokens.at(parameterIndex));
            params.push_back(tokens.at(parameterIndex + 1));
        }
        else {
            stringstream err;
            err << "Not enough parameters available for " << tokens.at(startIndex);
            throw invalid_argument(err.str());
        }
        return params;
    };
} SaveParams;

static SaveParams savep = SaveParams();

typedef class BindParams : public ParamExtractor {
public:
    BindParams() {}

    dllfunctor_stdcall<int, LPCSTR, INT, LPWSAPROTOCOL_INFO, LPSOCKADDR, LPINT> f_WSAStringToAddressA =
        dllfunctor_stdcall<int, LPCSTR, INT, LPWSAPROTOCOL_INFO, LPSOCKADDR, LPINT>("ws2_32.dll", "WSAStringToAddressA");

    bool IsIPAddress(string address) {
        SOCKADDR_IN sockaddr4;
        sockaddr4.sin_family = AF_INET;
        SOCKADDR_IN6 sockaddr6;
        sockaddr6.sin6_family = AF_INET6;
        int addr4Length = sizeof(SOCKADDR_IN);
        int addr6Length = sizeof(SOCKADDR_IN6);
        DWORD err;
        if (ERROR_SUCCESS ==
            (err = f_WSAStringToAddressA(
                address.c_str(),
                AF_INET,
                NULL,
                (LPSOCKADDR) &sockaddr4,
                &addr4Length))) {
            return true;
        }
        else if (ERROR_SUCCESS ==
            (err = f_WSAStringToAddressA(
                address.c_str(),
                AF_INET6,
                NULL,
                (LPSOCKADDR) &sockaddr6,
                &addr6Length))) {
            return true;
        }
        else {
            return false;
        }
    }

    vector<string> Extract(int argStartIndex, int argc, char** argv) {
        vector<string> params;
        int argIndex = argStartIndex + 1;

        // bind [address1] [address2] ...
        while (argIndex < argc) {
            if (IsIPAddress(argv[argIndex])) {
                string param = string(argv[argIndex]);
                transform(param.begin(), param.end(), param.begin(), ::tolower);
                param = stripQuotes(param);
                params.push_back(param);
                argIndex++;
            }
            else {
                break;
            }
        }
        return params;
    }

    virtual vector<string> Extract(vector<string> tokens, int startIndex = 0) {
        vector<string> params;
        int skipCount = 1 + startIndex;
        for (string token : tokens) {
            if (skipCount > 0) {
                skipCount--;
                continue;
            }
            if (IsIPAddress(token)) {
                string param = string(token);
                transform(param.begin(), param.end(), param.begin(), ::tolower);
                param = stripQuotes(param);
                params.push_back(param);
            }
            else {
                break;
            }
        }
        return params;
    };
} BindParams;

static BindParams bp = BindParams();

typedef class SentinelParams : public  ParamExtractor {
private:
    RedisParameterMapper subCommands;

public:
    SentinelParams() {
        subCommands = RedisParameterMapper
        {
            { "monitor",                    &fp4 },    // sentinel monitor [master name] [ip] [port] [quorum]
            { "down-after-milliseconds",    &fp2 },    // sentinel down-after-milliseconds [master name] [milliseconds]
            { "failover-timeout",           &fp2 },    // sentinel failover-timeout [master name] [number]
            { "parallel-syncs",             &fp2 },    // sentinel parallel-syncs [master name] [number]
            { "notification-script",        &fp2 },    // sentinel notification-script [master name] [scriptPath]
            { "client-reconfig-script",     &fp2 },    // sentinel client-reconfig-script [master name] [scriptPath]
            { "auth-pass",                  &fp2 },    // sentinel auth-pass [master name] [password]
            { "current-epoch",              &fp1 },    // sentinel current-epoch <epoch>
            { "myid",                       &fp1 },    // sentinel myid <id>
            { "config-epoch",               &fp2 },    // sentinel config-epoch [name] [epoch]
            { "leader-epoch",               &fp2 },    // sentinel leader-epoch [name] [epoch]
            { "known-slave",                &fp3 },    // sentinel known-slave <name> <ip> <port>
            { "known-replica",              &fp3 },    // sentinel known-slave <name> <ip> <port>
            { "known-sentinel",             &fp4 },    // sentinel known-sentinel <name> <ip> <port> [runid]
            { "rename-command",             &fp3 },    // sentinel rename-command <name> <command> <renamed-command>
            { "announce-ip",                &fp1 },    // sentinel announce-ip <ip>
            { "announce-port",              &fp1 },    // sentinel announce-port <port>
            { "deny-scripts-reconfig",      &fp1 }     // sentinel deny-scripts-reconfig [yes/no]
        };
    }

    vector<string> Extract(int argStartIndex, int argc, char** argv) {
        stringstream err;
        if (argStartIndex + 1 >= argc) {
            err << "Not enough parameters available for " << argv[argStartIndex];
            throw invalid_argument(err.str());
        }
        if (subCommands.find(argv[argStartIndex + 1]) == subCommands.end()) {
            err << "Could not find sentinel subcommand " << argv[argStartIndex + 1];
            throw invalid_argument(err.str());
        }

        vector<string> params;
        params.push_back(argv[argStartIndex + 1]);
        vector<string> subParams = subCommands[argv[argStartIndex + 1]]->Extract(argStartIndex + 1, argc, argv);
        for (string p : subParams) {
            transform(p.begin(), p.end(), p.begin(), ::tolower);
            p = stripQuotes(p);
            params.push_back(p);
        }
        return params;
    }

    vector<string> Extract(vector<string> tokens, int startIndex = 0) {
        stringstream err;
        if (tokens.size() < 2) {
            err << "Not enough parameters available for " << tokens.at(0);
            throw invalid_argument(err.str());
        }
        string subcommand = tokens.at(startIndex + 1);
        if (subCommands.find(subcommand) == subCommands.end()) {
            err << "Could not find sentinel subcommand " << subcommand;
            throw invalid_argument(err.str());
        }

        vector<string> params;
        params.push_back(subcommand);

        vector<string> subParams = subCommands[subcommand]->Extract(tokens, startIndex + 1);

        for (string p : subParams) {
            transform(p.begin(), p.end(), p.begin(), ::tolower);
            p = stripQuotes(p);
            params.push_back(p);
        }
        return params;
    };
} SentinelParams;

static SentinelParams sp = SentinelParams();

// Map of argument name to argument processing engine.
static RedisParameterMapper g_redisArgMap =
{
    // QFork flags
    { cQFork,                           &fp2 },    // qfork [QForkControlMemoryMap handle] [parent process id]
    { cPersistenceAvailable,            &fp1 },    // persistence-available [yes/no]

    // service commands
    { cServiceName,                     &fp1 },    // service-name [name]
    { cServiceRun,                      &fp0 },    // service-run
    { cServiceInstall,                  &fp0 },    // service-install
    { cServiceUninstall,                &fp0 },    // service-uninstall
    { cServiceStart,                    &fp0 },    // service-start
    { cServiceStop,                     &fp0 },    // service-stop

    // redis commands (ordered as they appear in config.c/loadServerConfigFromString())
    { "timeout",                        &fp1 },    // timeout [value]
    { "tcp-keepalive",                  &fp1 },    // tcp-keepalive [value]
    { "protected-mode",                 &fp1 },    // protected-mode [yes/no]
    { "port",                           &fp1 },    // port [port number]
    { "tcp-backlog",                    &fp1 },    // tcp-backlog [number]
    { "bind",                           &bp },     // bind [address] [address] ...
    { "unixsocket",                     &fp1 },    // unixsocket [path]
    { "unixsocketperm",                 &fp1 },    // unixsocketperm [perm]
    { "save",                           &savep },  // save [seconds] [changes] or save ""
    { cDir,                             &fp1 },    // dir [path]
    { "loglevel",                       &fp1 },    // lovlevel [value]
    { "logfile",                        &fp1 },    // logfile [file]
    { "always-show-logo",               &fp1 },    // always-show-logo [yes/no]
    { "syslog-enabled",                 &fp1 },    // syslog-enabled [yes/no]
    { "syslog-ident",                   &fp1 },    // syslog-ident [string]
    { "syslog-facility",                &fp1 },    // syslog-facility [string]
    { "databases",                      &fp1 },    // databases [number]
    //"include" is handled in ParseConfFile()
    { "maxclients",                     &fp1 },    // maxclients [number]
    { "maxmemory",                      &fp1 },    // maxmemory [bytes]
    { "maxmemory-policy",               &fp1 },    // maxmemory-policy [policy]
    { "maxmemory-samples",              &fp1 },    // maxmemory-samples [number]
    { "proto-max-bulk-len",             &fp1 },    // proto-max-bulk-len [number]
    { "client-query-buffer-limit",      &fp1 },    // client-query-buffer-limit [number]
    { "lfu-log-factor",                 &fp1 },    // lfu-log-factor [number]
    { "lfu-decay-time",                 &fp1 },    // lfu-decay-time [number]
    { "slaveof",                        &fp2 },    // slaveof [masterip] [master port]
    { "replicaof",                      &fp2 },    // replicaof [masterip] [master port]
    { "repl-ping-slave-period",         &fp1 },    // repl-ping-slave-period [number]
    { "repl-ping-replica-period",       &fp1 },    // repl-ping-replica-period [number]
    { "repl-timeout",                   &fp1 },    // repl-timeout [number]
    { "repl-disable-tcp-nodelay",       &fp1 },    // repl-disable-tcp-nodelay [yes/no]
    { "repl-diskless-sync",             &fp1 },    // repl-diskless-sync [yes/no]
    { "repl-diskless-sync-delay",       &fp1 },    // repl-diskless-sync-delay [number]
    { "repl-backlog-size",              &fp1 },    // repl-backlog-size [number]
    { "repl-backlog-ttl",               &fp1 },    // repl-backlog-ttl [number]
    { "masterauth",                     &fp1 },    // masterauth [master-password]
    { "slave-serve-stale-data",         &fp1 },    // slave-serve-stale-data [yes/no]
    { "replica-serve-stale-data",       &fp1 },    // replica-serve-stale-data [yes/no]
    { "slave-read-only",                &fp1 },    // slave-read-only [yes/no]
    { "replica-read-only",              &fp1 },    // replica-read-only [yes/no]
    { "slave-ignore-maxmemory",         &fp1 },    // slave-ignore-maxmemory [yes/no]
    { "replica-ignore-maxmemory",       &fp1 },    // replica-ignore-maxmemory [yes/no]
    { "rdbcompression",                 &fp1 },    // rdbcompression [yes/no]
    { "rdbchecksum",                    &fp1 },    // rdbchecksum [yes/no]
    { "activerehashing",                &fp1 },    // activerehashing [yes/no]
    { "lazyfree-lazy-eviction",         &fp1 },    // lazyfree-lazy-eviction [yes/no]
    { "lazyfree-lazy-expire",           &fp1 },    // lazyfree-lazy-expire [yes/no]
    { "lazyfree-lazy-server-del",       &fp1 },    // lazyfree-lazy-server-del [yes/no]
    { "slave-lazy-flush",               &fp1 },    // slave-lazy-flush [yes/no]
    { "replica-lazy-flush",             &fp1 },    // replica-lazy-flush [yes/no]
    { "activedefrag",                   &fp1 },    // activedefrag [yes/no]
    { "daemonize",                      &fp1 },    // daemonize [yes/no]
    { "dynamic-hz",                     &fp1 },    // dynamic-hz [yes/no]
    { "hz",                             &fp1 },    // hz [number]
    { "appendonly",                     &fp1 },    // appendonly [yes/no]
    { "appendfilename",                 &fp1 },    // appendfilename [filename]
    { "appenddirname",                  &fp1 },    // appenddirname [dirname]
    { "no-appendfsync-on-rewrite",      &fp1 },    // no-appendfsync-on-rewrite [value]
    { "appendfsync",                    &fp1 },    // appendfsync [value]
    { "auto-aof-rewrite-percentage",    &fp1 },    // auto-aof-rewrite-percentage [number]
    { "auto-aof-rewrite-min-size",      &fp1 },    // auto-aof-rewrite-min-size [number]
    { "aof-rewrite-incremental-fsync",  &fp1 },    // aof-rewrite-incremental-fsync [yes/no]
    { "rdb-save-incremental-fsync",     &fp1 },    // rdb-save-incremental-fsync [yes/no]
    { "aof-load-truncated",             &fp1 },    // aof-load-truncated [yes/no]
    { "aof-use-rdb-preamble",           &fp1 },    // aof-use-rdb-preamble [yes/no]
    { "requirepass",                    &fp1 },    // requirepass [string]
    { "pidfile",                        &fp1 },    // pidfile [file]
    { "dbfilename",                     &fp1 },    // dbfilename [filename]
    { "active-defrag-threshold-lower",  &fp1 },    // active-defrag-threshold-lower [number]
    { "active-defrag-threshold-upper",  &fp1 },    // active-defrag-threshold-upper [number]
    { "active-defrag-ignore-bytes",     &fp1 },    // active-defrag-ignore-bytes [number]
    { "active-defrag-cycle-min",        &fp1 },    // active-defrag-cycle-min [number]
    { "active-defrag-cycle-max",        &fp1 },    // active-defrag-cycle-max [number]
    { "active-defrag-max-scan-fields",  &fp1 },    // active-defrag-max-scan-fields [number]
    { "hash-max-ziplist-entries",       &fp1 },    // hash-max-ziplist-entries [number]
    { "hash-max-ziplist-value",         &fp1 },    // hash-max-ziplist-value [number]
    { "stream-node-max-bytes",          &fp1 },    // stream-node-max-bytes [number]
    { "stream-node-max-entries",        &fp1 },    // stream-node-max-entries [number]
    { "list-max-ziplist-entries",       &fp1 },    // list-max-ziplist-entries [number]     DEAD OPTION
    { "list-max-ziplist-value",         &fp1 },    // list-max-ziplist-value [number]       DEAD OPTION
    { "list-max-ziplist-size",          &fp1 },    // list-max-ziplist-size [number]
    { "list-compress-depth",            &fp1 },    // list-compress-depth[number]
    { "set-max-intset-entries",         &fp1 },    // set-max-intset-entries [number]
    { "zset-max-ziplist-entries",       &fp1 },    // zset-max-ziplist-entries [number]
    { "zset-max-ziplist-value",         &fp1 },    // zset-max-ziplist-value [number]
    { "hll-sparse-max-bytes",           &fp1 },    // hll-sparse-max-bytes [number]
    { "rename-command",                 &fp2 },    // rename-command [command] [string]
    { "cluster-enabled",                &fp1 },    // cluster-enabled [yes/no]
    { "cluster-config-file",            &fp1 },    // cluster-config-file [filename]
    { "cluster-announce-ip",            &fp1 },    // cluster-announce-ip [string]
    { "cluster-announce-port",          &fp1 },    // cluster-announce-port [number]
    { "cluster-announce-bus-port",      &fp1 },    // cluster-announce-bus-port [number]
    { "cluster-require-full-coverage",  &fp1 },    // cluster-require-full-coverage [yes/no]
    { "cluster-node-timeout",           &fp1 },    // cluster-node-timeout [number]
    { "cluster-migration-barrier",      &fp1 },    // cluster-migration-barrier [number]
    { "cluster-slave-validity-factor",  &fp1 },    // cluster-slave-validity-factor [number]
    { "cluster-replica-validity-factor",&fp1 },    // cluster-replica-validity-factor [number]
    { "cluster-slave-no-failover",      &fp1 },    // cluster-slave-no-failover [yes/no]
    { "cluster-replica-no-failover",    &fp1 },    // cluster-replica-no-failover [yes/no]
    { "lua-time-limit",                 &fp1 },    // lua-time-limit [number]
    { "lua-replicate-commands",         &fp1 },    // lua-replicate-commands [yes/no]
    { "slowlog-log-slower-than",        &fp1 },    // slowlog-log-slower-than [number]
    { "latency-monitor-threshold",      &fp1 },    // latency-monitor-threshold [number]
    { "slowlog-max-len",                &fp1 },    // slowlog-max-len [number]
    { "client-output-buffer-limit",     &fp4 },    // client-output-buffer-limit [class] [hard limit] [soft limit] [soft seconds]
    { "stop-writes-on-bgsave-error",    &fp1 },    // stop-writes-on-bgsave-error [yes/no]
    { "slave-priority",                 &fp1 },    // slave-priority [number]
    { "replica-priority",               &fp1 },    // replica-priority [number]
    { "slave-announce-ip",              &fp1 },    // slave-announce-ip [string]
    { "replica-announce-ip",            &fp1 },    // replica-announce-ip [string]
    { "slave-announce-port",            &fp1 },    // slave-announce-port [number]
    { "replica-announce-port",          &fp1 },    // replica-announce-port [number]
    { "min-slaves-to-write",            &fp1 },    // min-slaves-to-write [number]
    { "min-replicas-to-write",          &fp1 },    // min-replicas-to-write [number]
    { "min-slaves-max-lag",             &fp1 },    // min-slaves-max-lag [number]
    { "min-replicas-max-lag",           &fp1 },    // min-replicas-max-lag [number]
    { "notify-keyspace-events",         &fp1 },    // notify-keyspace-events [string]
    { "supervised",                     &fp1 },    // supervised [upstart|systemd|auto|no]
    /* loadmodule accepts arbitrary module arguments after the filename.
     * It is not needed by the Windows bootstrap layer, so let the generic
     * forwarding path consume the complete directive. */
    { "sentinel",                       &sp  },    // sentinel commands
    { "watchdog-period",                &fp1 },    // watchdog-period [number]
    { cInclude,                         &fp1 }     // include [path]
};

/*
 * The Windows bootstrap runs before redis_main() and only needs a small
 * subset of command-line configuration directives.  Keep the complete map
 * above for config-file scanning (service installation needs to discover
 * paths and persistence metadata), but do not make this layer the
 * authoritative parser for ordinary Redis options.  Redis' core parser must
 * see those options unchanged so it can provide its normal arity and value
 * diagnostics.
 */
static bool IsBootstrapCommandLineArgument(const string& argument) {
    return argument == cQFork ||
        argument == cPersistenceAvailable ||
        argument == cServiceName ||
        argument == cServiceRun ||
        argument == cServiceInstall ||
        argument == cServiceUninstall ||
        argument == cServiceStart ||
        argument == cServiceStop ||
        argument == cSyslogEnabled ||
        argument == cSyslogIdent ||
        argument == cLogfile ||
        argument == cDir;
}

/* These options are owned by the Windows wrapper.  A malformed value must
 * fail before QFork/service setup can inspect it. */
static bool IsStrictBootstrapCommandLineArgument(const string& argument) {
    return argument == cQFork ||
        argument == cPersistenceAvailable ||
        argument == cServiceName ||
        argument == cServiceRun ||
        argument == cServiceInstall ||
        argument == cServiceUninstall ||
        argument == cServiceStart ||
        argument == cServiceStop;
}

static bool IsServiceActionArgument(const string& argument) {
    return argument == cServiceRun ||
        argument == cServiceInstall ||
        argument == cServiceUninstall ||
        argument == cServiceStart ||
        argument == cServiceStop;
}

static bool IsCommandLineHexDigit(char value) {
    return (value >= '0' && value <= '9') ||
        (value >= 'a' && value <= 'f') ||
        (value >= 'A' && value <= 'F');
}

static int CommandLineHexDigitToInt(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return value - 'A' + 10;
}

/* Match sdssplitargs() without linking the Windows bootstrap layer to Redis'
 * allocator-backed SDS implementation.  Win32_CommandLine.cpp is shared by
 * redis-server, redis-cli, redis-benchmark, and hiredis-test, while only the
 * server otherwise links src/sds.c. */
static vector<string> SplitCommandLineArgument(const char *argument) {
    vector<string> tokens;
    if (argument == NULL) return tokens;

    const char *p = argument;
    while (true) {
        while (*p && isspace(static_cast<unsigned char>(*p))) p++;
        if (!*p) return tokens;

        string current;
        bool inDoubleQuotes = false;
        bool inSingleQuotes = false;
        bool done = false;
        while (!done) {
            if (inDoubleQuotes) {
                if (*p == '\\' && *(p + 1) == 'x' &&
                    IsCommandLineHexDigit(*(p + 2)) &&
                    IsCommandLineHexDigit(*(p + 3))) {
                    current.push_back(static_cast<char>(
                        CommandLineHexDigitToInt(*(p + 2)) * 16 +
                        CommandLineHexDigitToInt(*(p + 3))));
                    p += 3;
                }
                else if (*p == '\\' && *(p + 1)) {
                    p++;
                    switch (*p) {
                    case 'n': current.push_back('\n'); break;
                    case 'r': current.push_back('\r'); break;
                    case 't': current.push_back('\t'); break;
                    case 'b': current.push_back('\b'); break;
                    case 'a': current.push_back('\a'); break;
                    default: current.push_back(*p); break;
                    }
                }
                else if (*p == '"') {
                    if (*(p + 1) &&
                        !isspace(static_cast<unsigned char>(*(p + 1)))) {
                        return vector<string>();
                    }
                    done = true;
                }
                else if (!*p) {
                    return vector<string>();
                }
                else {
                    current.push_back(*p);
                }
            }
            else if (inSingleQuotes) {
                if (*p == '\\' && *(p + 1) == '\'') {
                    p++;
                    current.push_back('\'');
                }
                else if (*p == '\'') {
                    if (*(p + 1) &&
                        !isspace(static_cast<unsigned char>(*(p + 1)))) {
                        return vector<string>();
                    }
                    done = true;
                }
                else if (!*p) {
                    return vector<string>();
                }
                else {
                    current.push_back(*p);
                }
            }
            else {
                switch (*p) {
                case ' ':
                case '\n':
                case '\r':
                case '\t':
                case '\0':
                    done = true;
                    break;
                case '"':
                    inDoubleQuotes = true;
                    break;
                case '\'':
                    inSingleQuotes = true;
                    break;
                default:
                    current.push_back(*p);
                    break;
                }
            }
            if (*p) p++;
        }
        tokens.push_back(current);
    }
}

static bool IsLongCommandLineArgument(const char *argument) {
    return argument != NULL && argument[0] == '-' && argument[1] == '-';
}

/* Reproduce redis_main()'s handled_last_config_arg state machine.  The
 * resulting mask identifies actual option names; a --prefixed token in a
 * value position is deliberately not treated as a Windows bootstrap flag. */
static vector<bool> RedisCommandLineOptionMask(int argc, char **argv,
    int startIndex) {
    vector<bool> option(argc, false);
    bool handledLastConfigArg = true;
    for (int i = startIndex; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == '\0' &&
            (i == startIndex || i == argc - 1)) {
            continue;
        }

        if (handledLastConfigArg && IsLongCommandLineArgument(argv[i])) {
            option[i] = true;
            vector<string> tokens = SplitCommandLineArgument(argv[i]);
            if (tokens.size() == 1) {
                handledLastConfigArg = false;
                string name = tokens.empty() ? string() : tokens[0];
                transform(name.begin(), name.end(), name.begin(), ::tolower);
                if (i != argc - 1 &&
                    IsLongCommandLineArgument(argv[i + 1]) &&
                    (name == "--save" || name == "--sentinel")) {
                    /* Redis treats these two pseudo-options as empty and
                     * starts a fresh option on the next argv element. */
                    handledLastConfigArg = true;
                }
            }
            else {
                /* The option and value(s) are contained in one argv item. */
                handledLastConfigArg = true;
            }
        }
        else {
            handledLastConfigArg = true;
        }
    }
    return option;
}

static bool IsBootstrapCommandLineArgumentAt(const string& argument,
    int index, bool serviceInvocation, int serviceNameIndex) {
    if (!IsBootstrapCommandLineArgument(argument)) {
        return false;
    }

    /* QFork and service actions are generated/recognized only at argv[1]. */
    if (argument == cQFork) {
        return index == 1;
    }
    if (IsServiceActionArgument(argument)) {
        return serviceInvocation && index == 1;
    }

    /* --service-name is a prefix modifier, not a Redis value. */
    return argument != cServiceName || index == serviceNameIndex;
}

static void RecordCommandLineArgument(const string& argument,
    const vector<string>& params) {
    /* ParseConfFile() runs after the command line.  Keep the latest command
     * line occurrence at slot zero so the Windows consumers retain the same
     * precedence as Redis' core parser; config-file values are appended
     * afterward and do not displace it. */
    vector<vector<string>>& values = g_argMap[argument];
    if (values.empty()) {
        values.push_back(params);
    }
    else {
        values[0] = params;
    }
}

std::vector<std::string> &split(const std::string &s, char delim, std::vector<std::string> &elems) {
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        if (!item.empty())
            elems.push_back(item);
    }
    return elems;
}

std::vector<std::string> split(const std::string &s, char delim) {
    std::vector<std::string> elems;
    split(s, delim, elems);
    return elems;
}

vector<string> Tokenize(string line) {
    vector<string> tokens;
    stringstream token;

    // no need to parse empty lines, or comment lines (which may have unbalanced quotes)
    if ((line.length() == 0) ||
        ((line.length() != 0) && (*line.begin()) == '#')) {
        return tokens;
    }

    for (string::const_iterator sit = line.begin(); sit != line.end(); sit++) {
        char c = *(sit);
        if (isspace(c) && token.str().length() > 0) {
            tokens.push_back(token.str());
            token.str("");
        }
        else if (c == '\'' || c == '\"') {
            char endQuote = c;
            string::const_iterator endQuoteIt = sit;
            while (++endQuoteIt != line.end()) {
                if (*endQuoteIt == endQuote) break;
            }
            if (endQuoteIt != line.end()) {
                while (++sit != endQuoteIt) {
                    token << (*sit);
                }

                // The code above strips quotes. In certain cases (save "") the quotes should be preserved around empty strings
                if (token.str().length() == 0)
                    token << endQuote << endQuote;

                /* Quoted Redis values are not necessarily paths.  Preserve
                 * their bytes exactly; Windows path normalization belongs at
                 * the filesystem API boundary. */
                tokens.push_back(token.str());

                token.str("");
            }
            else {
                // stuff the imbalanced quote character and continue
                token << (*sit);
            }
        }
        else {
            token << c;
        }
    }
    if (token.str().length() > 0) {
        tokens.push_back(token.str());
    }

    return tokens;
}

void ParseConfFile(string confFile, string cwd, ArgumentMap& argMap);

static void ParseConfLine(const string& line, const string& cwd,
                          ArgumentMap& argMap) {
    vector<string> tokens = Tokenize(line);
    if (tokens.size() > 0) {
        string parameter = tokens.at(0);
        transform(parameter.begin(), parameter.end(), parameter.begin(), ::tolower);
        if (parameter.at(0) == '#') {
            return;
        }
        else if (parameter.compare(cInclude) == 0) {
            if (tokens.size() > 1) {
                ParseConfFile(tokens.at(1), cwd, argMap);
            }
        }
        else {
            auto extractor = g_redisArgMap.find(parameter);
            if (extractor == g_redisArgMap.end()) {
                return;
            }

            try {
                vector<string> params = extractor->second->Extract(tokens);
                argMap[parameter].push_back(params);
            }
            catch (const invalid_argument &) {
                /* This pre-parser only discovers Windows service and QFork
                 * metadata. Redis config.c performs authoritative syntax
                 * and arity validation after startup initialization. */
            }
        }
    }
}

void ParseConfFile(string confFile, string cwd, ArgumentMap& argMap) {
    char *fullConfFilePath = win32_get_full_path_utf8(confFile.c_str());
    wchar_t *wideConfFilePath;
    FILE *config;
    string line;

    if (fullConfFilePath == NULL) {
        throw std::system_error(errno, generic_category(),
                                "GetFullPathNameW failed");
    }
    wideConfFilePath = win32_utf8_path_to_wide(fullConfFilePath);
    if (wideConfFilePath == NULL) {
        win32_free(fullConfFilePath);
        throw std::system_error(errno, generic_category(),
                                "UTF-8 configuration path conversion failed");
    }

    config = _wfopen(wideConfFilePath, L"rb");
    win32_free(wideConfFilePath);
    if (config == NULL) {
        stringstream ss;
        ss << "Failed to open the .conf file: " << confFile << " CWD=" << cwd.c_str();
        win32_free(fullConfFilePath);
        throw invalid_argument(ss.str());
    }

    {
        string fullPath(fullConfFilePath);
        size_t separator = fullPath.find_last_of("\\/");
        if (separator == string::npos) {
            fclose(config);
            win32_free(fullConfFilePath);
            throw std::runtime_error("Configuration path has no directory");
        }
        if (separator == 2 && fullPath.size() >= 3 && fullPath[1] == ':')
            separator++;
        g_pathsAccessed.push_back(fullPath.substr(0, separator));
    }
    win32_free(fullConfFilePath);

    char chunk[4096];
    while (fgets(chunk, sizeof(chunk), config) != NULL) {
        line.append(chunk);
        if (!line.empty() && line.back() == '\n') {
            ParseConfLine(line, cwd, argMap);
            line.clear();
        }
    }
    if (ferror(config)) {
        int readError = errno == 0 ? EIO : errno;
        fclose(config);
        throw std::system_error(readError, generic_category(),
                                "Failed to read configuration file");
    }
    if (!line.empty()) ParseConfLine(line, cwd, argMap);
    fclose(config);
}

vector<string> incompatibleNoPersistenceCommands{
    "min-slaves-to-write",
    "min-replicas-to-write",
    "min-slaves-max-lag",
    "min-replicas-max-lag",
    "appendonly",
    "appendfilename",
    "appenddirname",
    "appendfsync",
    "no-appendfsync-on-rewrite",
    "auto-aof-rewrite-percentage",
    "auto-aof-rewrite-min-size",
    "aof-rewrite-incremental-fsync",
    "save"
};

void ValidateCommandlineCombinations() {
    if (g_argMap.find(cPersistenceAvailable) != g_argMap.end()) {
        if (g_argMap[cPersistenceAvailable].at(0).at(0) == cNo) {
            string incompatibleCommand = "";
            for (auto command : incompatibleNoPersistenceCommands) {
                if (g_argMap.find(command) != g_argMap.end()) {
                    incompatibleCommand = command;
                    break;
                }
            }
            if (incompatibleCommand.length() > 0) {
                stringstream ss;
                ss << "'" << cPersistenceAvailable << " " << cNo << "' command not compatible with '" << incompatibleCommand << "'. Exiting.";
                throw std::invalid_argument(ss.str().c_str());
            }
        }
    }
}

void ParseCommandLineArguments(int argc, char** argv) {
    if (argc < 2) {
        return;
    }

    bool confFile = false;
    string confFilePath;
    bool serviceInvocation = false;
    int serviceNameIndex = -1;
    int redisArgumentStart = 1;

    /* Redis normally accepts a config file only as argv[1].  The Windows
     * service wrapper is the sole exception: its action and optional
     * --service-name pair precede the Redis argument list that the service
     * worker later reconstructs.  Resolve that one filename up front so
     * option values can never be mistaken for replacement config files. */
    if (argv[1][0] != '-') {
        confFile = true;
        confFilePath = argv[1];
        redisArgumentStart = 2;
    }
    else if (IsLongCommandLineArgument(argv[1])) {
        vector<string> firstTokens = SplitCommandLineArgument(argv[1]);
        string firstArgument;
        if (firstTokens.size() == 1 &&
            firstTokens[0].substr(0, 2) == "--") {
            firstArgument = firstTokens[0].substr(2);
        }
        transform(firstArgument.begin(), firstArgument.end(),
            firstArgument.begin(), ::tolower);
        if (IsServiceActionArgument(firstArgument)) {
            serviceInvocation = true;
            int candidate = 2;
            if (candidate < argc) {
                vector<string> serviceNameTokens =
                    SplitCommandLineArgument(argv[candidate]);
                if (!serviceNameTokens.empty() &&
                    serviceNameTokens[0].substr(0, 2) == "--") {
                    string serviceNameArgument =
                        serviceNameTokens[0].substr(2);
                    transform(serviceNameArgument.begin(),
                        serviceNameArgument.end(),
                        serviceNameArgument.begin(), ::tolower);
                    if (serviceNameArgument == cServiceName) {
                        serviceNameIndex = candidate;
                        candidate += serviceNameTokens.size() > 1 ? 1 : 2;
                    }
                }
            }
            if (candidate < argc && argv[candidate][0] != '-') {
                confFile = true;
                confFilePath = argv[candidate];
                redisArgumentStart = candidate + 1;
            }
            else {
                redisArgumentStart = candidate;
            }
        }
    }

    vector<bool> optionArgument =
        RedisCommandLineOptionMask(argc, argv, redisArgumentStart);
    if (serviceInvocation) {
        optionArgument[1] = true;
    }
    if (serviceNameIndex >= 0 && serviceNameIndex < argc) {
        optionArgument[serviceNameIndex] = true;
    }

    for (int n = 1; n < argc; n++) {
        if (!optionArgument[n]) {
            continue;
        }

        vector<string> tokens = SplitCommandLineArgument(argv[n]);
        if (tokens.empty() || tokens[0].substr(0, 2) != "--") {
            continue;
        }
        string argument = tokens[0].substr(2);
        transform(argument.begin(), argument.end(), argument.begin(), ::tolower);

        // Some -- arguments are passed directly to redis.c::main().
        if (find(cRedisArgsForMainC.begin(), cRedisArgsForMainC.end(),
            argument) != cRedisArgsForMainC.end()) {
            continue;
        }

        bool bootstrapArgumentAt = IsBootstrapCommandLineArgumentAt(
            argument, n, serviceInvocation, serviceNameIndex);
        if (!bootstrapArgumentAt) {
            /* Ordinary Redis options remain untouched for redis_main().
             * Record their presence for the Windows no-persistence
             * compatibility check, but do not consume values or perform
             * syntax validation in this bootstrap layer. */
            if (!IsBootstrapCommandLineArgument(argument)) {
                RecordCommandLineArgument(argument, vector<string>());
            }
            continue;
        }

        // -- arguments processed before calling redis.c::main()
        vector<string> params;
        auto extractor = g_redisArgMap.find(argument);
        bool inlineValues = tokens.size() > 1;
        try {
            if (argument == cServiceRun) {
                // When the service starts the current directory is %systemdir%. This needs to be changed to the
                // directory the executable is in so that the .conf file can be loaded.
                char *modulePath = win32_get_module_filename_utf8();
                if (modulePath == NULL) {
                    throw std::system_error(errno, generic_category(),
                                            "GetModuleFileNameW failed");
                }
                string currentDir = modulePath;
                free(modulePath);
                auto pos = currentDir.find_last_of("\\/");
                if (pos == string::npos)
                    throw std::runtime_error("Executable path has no directory");
                currentDir.erase(pos);

                if (win32_set_current_directory_utf8(currentDir.c_str()) != 0) {
                    throw std::system_error(errno, generic_category(),
                                            "SetCurrentDirectoryW failed");
                }
            }
            else if (inlineValues) {
                params = extractor->second->Extract(tokens);
            }
            else {
                params = extractor->second->Extract(n, argc, argv);
            }
        }
        catch (const invalid_argument&) {
            if (IsStrictBootstrapCommandLineArgument(argument)) {
                throw;
            }
            /* Let redis_main() report malformed ordinary metadata options
             * (for example --logfile without a value). */
            continue;
        }
        RecordCommandLineArgument(argument, params);
        if (!inlineValues && extractor != g_redisArgMap.end()) {
            if (params.size() > (size_t)(INT_MAX - n)) {
                throw length_error("Command line has too many arguments");
            }
            n += (int) params.size();
        }
    }

    char *cwdBuffer = win32_get_current_directory_utf8();
    if (cwdBuffer == NULL) {
        throw std::system_error(errno, generic_category(),
                                "ParseCommandLineArguments: GetCurrentDirectoryW failed");
    }
    string cwd(cwdBuffer);
    win32_free(cwdBuffer);

    if (confFile) {
        ParseConfFile(confFilePath, cwd, g_argMap);
    }

    // grab directory where RDB/AOF files will be created so that service install can add access allowed ACE to path
    string fileCreationDirectory = ".\\";
    if (g_argMap.find(cDir) != g_argMap.end()) {
        fileCreationDirectory = g_argMap[cDir][0][0];
    }
    char *fullPath = win32_get_full_path_utf8(fileCreationDirectory.c_str());
    if (fullPath == NULL) {
        throw std::system_error(errno, generic_category(),
                                "GetFullPathNameW failed for data directory");
    }
    fileCreationDirectory = fullPath;
    win32_free(fullPath);
    g_pathsAccessed.push_back(fileCreationDirectory);

    ValidateCommandlineCombinations();
}

vector<string> GetAccessPaths() {
    return g_pathsAccessed;
}
