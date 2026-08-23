param(
    [string]$BuildDir = (Join-Path $PSScriptRoot "..\..\build\mingw64"),
    [string]$ServiceName = "RedisPortTest",
    [int]$Port = 6397
)

$ErrorActionPreference = "Stop"

if (-not ("RedisPortEventResource" -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

public static class RedisPortEventResource {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr LoadLibraryEx(
        string fileName, IntPtr file, uint flags);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern int FormatMessage(
        uint flags, IntPtr source, uint messageId, uint languageId,
        StringBuilder buffer, int size, IntPtr arguments);

    [DllImport("kernel32.dll")]
    private static extern bool FreeLibrary(IntPtr module);

    public static string Read(string path, uint messageId) {
        const uint LOAD_LIBRARY_AS_DATAFILE = 0x00000002;
        const uint FORMAT_MESSAGE_IGNORE_INSERTS = 0x00000200;
        const uint FORMAT_MESSAGE_FROM_HMODULE = 0x00000800;

        IntPtr module = LoadLibraryEx(path, IntPtr.Zero, LOAD_LIBRARY_AS_DATAFILE);
        if (module == IntPtr.Zero) {
            throw new Win32Exception(Marshal.GetLastWin32Error());
        }

        try {
            StringBuilder buffer = new StringBuilder(256);
            int length = FormatMessage(
                FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_FROM_HMODULE,
                module, messageId, 0x409, buffer, buffer.Capacity, IntPtr.Zero);
            if (length == 0) {
                throw new Win32Exception(Marshal.GetLastWin32Error());
            }
            return buffer.ToString();
        } finally {
            FreeLibrary(module);
        }
    }
}
"@
}

function Assert-LastExitCode([string]$Operation) {
    if ($LASTEXITCODE -ne 0) {
        throw "$Operation failed with exit code $LASTEXITCODE"
    }
}

function Wait-ForRedis([string]$Cli, [int]$RedisPort) {
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    do {
        $reply = & $Cli -h 127.0.0.1 -p $RedisPort PING 2>$null
        if ($LASTEXITCODE -eq 0 -and $reply -eq "PONG") {
            return
        }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)

    throw "Redis service did not accept connections on port $RedisPort"
}

function Test-PortAvailable([int]$RedisPort) {
    $listener = $null
    try {
        $listener = [Net.Sockets.TcpListener]::new(
            [Net.IPAddress]::Loopback, $RedisPort)
        $listener.Server.ExclusiveAddressUse = $true
        $listener.Start()
        return $true
    } catch {
        return $false
    } finally {
        if ($listener) {
            $listener.Stop()
        }
    }
}

function Wait-ForPortClosed([int]$RedisPort) {
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    do {
        if (Test-PortAvailable $RedisPort) {
            return
        }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)

    throw "Port $RedisPort remained open after the service stopped"
}

function Get-MatchingRedisServiceProcesses(
    [string]$Name,
    [string]$ExpectedServer
) {
    $escapedName = [regex]::Escape($Name)
    $serviceNamePattern =
        '(?i)(?:^|\s)"?--service-name"?(?:\s+|=)"?{0}"?(?=\s|$)' -f
            $escapedName
    $serviceRunPattern = '(?i)(?:^|\s)"?--service-run"?(?=\s|$)'

    Get-CimInstance Win32_Process -Filter "Name = 'redis-server.exe'" |
        Where-Object {
            $_.ExecutablePath -and
            $_.ExecutablePath -ieq $ExpectedServer -and
            $_.CommandLine -and
            $_.CommandLine -match $serviceRunPattern -and
            $_.CommandLine -match $serviceNamePattern
        }
}

function Get-ExactRedisServiceProcess(
    [string]$Name,
    [string]$ExpectedServer
) {
    $serviceRecord = Get-CimInstance Win32_Service -Filter (
        "Name = '{0}'" -f $Name)
    if (@($serviceRecord).Count -ne 1) {
        throw "Expected exactly one SCM record for service $Name"
    }
    if ($serviceRecord.State -ne "Running" -or
        [uint32]$serviceRecord.ProcessId -eq 0) {
        throw "Service $Name has no running SCM process"
    }

    $matches = @(Get-MatchingRedisServiceProcesses $Name $ExpectedServer)
    if ($matches.Count -ne 1) {
        throw "Expected exactly one owned redis-server process for service $Name; found $($matches.Count)"
    }
    if ([uint32]$matches[0].ProcessId -ne [uint32]$serviceRecord.ProcessId) {
        throw "SCM PID $($serviceRecord.ProcessId) does not match owned Redis PID $($matches[0].ProcessId)"
    }
    return $matches[0]
}

function Wait-ForProcessExit([uint32]$ProcessId) {
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    do {
        $process = Get-CimInstance Win32_Process -Filter (
            "ProcessId = {0}" -f $ProcessId) -ErrorAction SilentlyContinue
        if (-not $process) {
            return
        }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)

    throw "Redis service PID $ProcessId remained alive after stop"
}

function Assert-RedisRound(
    [string]$Cli,
    [int]$RedisPort,
    [string]$Key,
    [string]$Value
) {
    $pingReply = & $Cli -h 127.0.0.1 -p $RedisPort PING
    Assert-LastExitCode "Pinging the Redis service"
    if ($pingReply -ne "PONG") {
        throw "Unexpected PING reply: $pingReply"
    }

    $setReply = & $Cli -h 127.0.0.1 -p $RedisPort SET $Key $Value
    Assert-LastExitCode "Writing through the Redis service"
    if ($setReply -ne "OK") {
        throw "Unexpected SET reply: $setReply"
    }

    $getReply = & $Cli -h 127.0.0.1 -p $RedisPort GET $Key
    Assert-LastExitCode "Reading through the Redis service"
    if ($getReply -ne $Value) {
        throw "Unexpected GET reply: $getReply"
    }
}

function Start-RedisServiceInstance(
    [string]$Server,
    [string]$Cli,
    [System.ServiceProcess.ServiceController]$Controller,
    [string]$Name,
    [int]$RedisPort
) {
    & $Server --service-start --service-name $Name | Out-Null
    Assert-LastExitCode "Starting the Redis service"
    $Controller.WaitForStatus(
        [System.ServiceProcess.ServiceControllerStatus]::Running,
        [TimeSpan]::FromSeconds(30))
    Wait-ForRedis $Cli $RedisPort
    return (Get-ExactRedisServiceProcess $Name $Server)
}

function Stop-RedisServiceInstance(
    [string]$Server,
    [System.ServiceProcess.ServiceController]$Controller,
    [string]$Name,
    [int]$RedisPort,
    [uint32]$ExpectedProcessId
) {
    & $Server --service-stop --service-name $Name | Out-Null
    Assert-LastExitCode "Stopping the Redis service"
    $Controller.WaitForStatus(
        [System.ServiceProcess.ServiceControllerStatus]::Stopped,
        [TimeSpan]::FromSeconds(30))
    Wait-ForProcessExit $ExpectedProcessId
    Wait-ForPortClosed $RedisPort
}

function Get-TaggedRedisApplicationEvent(
    [datetime]$Since,
    [string]$Name
) {
    # Some hosted Windows images reject a FilterHashtable that combines
    # ProviderName and StartTime (Get-WinEvent reports "The parameter is
    # incorrect"). Query the bounded recent Application log instead and apply
    # the provider/time/message predicates in PowerShell. This keeps the
    # assertion strict while avoiding an image-specific provider filter.
    try {
        return Get-WinEvent -LogName "Application" -MaxEvents 256 -ErrorAction Stop |
            Where-Object {
                $_.ProviderName -ieq "redis" -and
                $_.TimeCreated -ge $Since -and
                $_.Message -match [regex]::Escape($Name)
            } | Select-Object -First 1
    } catch {
        return $null
    }
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "The service test requires an elevated Windows token"
}

if ($ServiceName -ieq "Redis") {
    throw "Refusing to use the installed Redis service name"
}

if ($ServiceName -notmatch '^[A-Za-z0-9_.-]+$') {
    throw "ServiceName must contain only letters, digits, dot, underscore, or hyphen"
}

if (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue) {
    throw "Service $ServiceName already exists"
}

$buildPath = (Resolve-Path $BuildDir).Path
$server = (Resolve-Path (Join-Path $buildPath "redis-server.exe")).Path
$cli = (Resolve-Path (Join-Path $buildPath "redis-cli.exe")).Path
$eventLogDll = (Resolve-Path (Join-Path $buildPath "EventLog.dll")).Path
$dataDir = Join-Path $buildPath "service-test"
$logFile = Join-Path $dataDir "redis-service.log"

foreach ($resource in @($server, $eventLogDll)) {
    $message = [RedisPortEventResource]::Read($resource, 0x60000000)
    if ($message.Trim() -ne "%1") {
        throw "Unexpected EventLog message resource in ${resource}: $message"
    }
}

if (-not (Test-PortAvailable $Port)) {
    throw "Port $Port is already in use"
}

if (Test-Path $dataDir) {
    throw "Refusing to reuse existing service test directory $dataDir"
}

$binaryPath = (
    '"{0}" --service-run --service-name {1} --persistence-available no --port {2} --bind 127.0.0.1 --dir "{3}" --logfile "{4}" --loglevel verbose --syslog-enabled yes --syslog-ident {1}' -f
        $server, $ServiceName, $Port, $dataDir, $logFile
)
$serviceInstallArguments = @(
    "--service-install",
    "--service-name", $ServiceName,
    "--persistence-available", "no",
    "--port", "$Port",
    "--bind", "127.0.0.1",
    "--dir", $dataDir,
    "--logfile", $logFile,
    "--loglevel", "verbose",
    "--syslog-enabled", "yes",
    "--syslog-ident", $ServiceName)
$eventLogSourcePath =
    "HKLM:\SYSTEM\CurrentControlSet\Services\EventLog\Application\redis"
$legacyEventLogPath =
    "HKLM:\SYSTEM\CurrentControlSet\Services\EventLog\redis"
$eventLogSourceWasPresent = Test-Path $eventLogSourcePath
$legacyEventLogWasPresent = Test-Path $legacyEventLogPath
$service = $null
$serviceCreated = $false
$serviceCreationAttempted = $false
$serviceInstalledByRedis = $false
$dataDirCreated = $false
$testSucceeded = $false

try {
    New-Item -ItemType Directory -Path $dataDir | Out-Null
    $dataDirCreated = $true
    $networkService = (
        [Security.Principal.SecurityIdentifier]"S-1-5-20").Translate(
            [Security.Principal.NTAccount]).Value
    $acl = Get-Acl $dataDir
    $rule = [Security.AccessControl.FileSystemAccessRule]::new(
        $networkService,
        [Security.AccessControl.FileSystemRights]::FullControl,
        [Security.AccessControl.InheritanceFlags]"ContainerInherit, ObjectInherit",
        [Security.AccessControl.PropagationFlags]::None,
        [Security.AccessControl.AccessControlType]::Allow)
    $acl.SetAccessRule($rule)
    Set-Acl -Path $dataDir -AclObject $acl

    $serviceCreationAttempted = $true
    if ($eventLogSourceWasPresent -or $legacyEventLogWasPresent) {
        # Preserve an existing Redis installation's shared Event Log source.
        # The manual SCM path avoids modifying either current or legacy shared
        # Event Log registration when either is already present.
        $serviceParameters = @{
            Name = $ServiceName
            DisplayName = $ServiceName
            BinaryPathName = $binaryPath
            StartupType = "Manual"
        }
        New-Service @serviceParameters | Out-Null
        & sc.exe config $ServiceName "obj=" "NT AUTHORITY\NetworkService" | Out-Null
        Assert-LastExitCode "Configuring the service account"
    } else {
        # Redis registers its Application Event Log source as part of the
        # supported service-install command. Use that path on clean CI hosts,
        # then remove only the source created by this test during cleanup.
        $serviceInstalledByRedis = $true
        & $server @serviceInstallArguments | Out-Null
        Assert-LastExitCode "Installing the Redis test service"
    }
    $serviceCreated = $true
    $service = Get-Service -Name $ServiceName

    if (-not $legacyEventLogWasPresent -and
        (Test-Path $legacyEventLogPath)) {
        throw "Redis service install created an ambiguous legacy custom Event Log"
    }

    $eventStart = Get-Date
    $firstProcess = Start-RedisServiceInstance $server $cli $service $ServiceName $Port
    $firstProcessId = [uint32]$firstProcess.ProcessId
    Assert-RedisRound $cli $Port "service-test-first" "mingw-first"

    $eventDeadline = [DateTime]::UtcNow.AddSeconds(15)
    $event = $null
    do {
        $event = Get-TaggedRedisApplicationEvent $eventStart $ServiceName
        if (-not $event) {
            Start-Sleep -Milliseconds 250
        }
    } while (-not $event -and [DateTime]::UtcNow -lt $eventDeadline)
    if (-not $event) {
        Write-Host "No tagged Application event found; recent Application records follow."
        try {
            Get-WinEvent -LogName "Application" -MaxEvents 64 -ErrorAction Stop |
                Select-Object TimeCreated, ProviderName, Id, LevelDisplayName, Message |
                Format-List | Out-String | Write-Host
        } catch {
            Write-Host "Unable to inspect recent Application records: $($_.Exception.Message)"
        }
        try {
            $source = Get-ItemProperty $eventLogSourcePath -ErrorAction Stop
            Write-Host (
                "Event source redis: EventMessageFile={0}; TypesSupported={1}" -f
                    $source.EventMessageFile, $source.TypesSupported)
        } catch {
            Write-Host "Unable to inspect redis Application Event Log source: $($_.Exception.Message)"
        }
        if (Test-Path $legacyEventLogPath) {
            Write-Host "Legacy custom redis Event Log registration is present."
            try {
                Get-WinEvent -LogName "redis" -MaxEvents 32 -ErrorAction Stop |
                    Select-Object TimeCreated, ProviderName, Id, LevelDisplayName, Message |
                    Format-List | Out-String | Write-Host
            } catch {
                Write-Host "Unable to inspect legacy redis Event Log: $($_.Exception.Message)"
            }
        }
        throw "The Redis service did not emit a tagged Application event"
    }

    Stop-RedisServiceInstance $server $service $ServiceName $Port $firstProcessId

    $secondProcess = Start-RedisServiceInstance $server $cli $service $ServiceName $Port
    $secondProcessId = [uint32]$secondProcess.ProcessId
    if ($secondProcessId -eq $firstProcessId) {
        throw "Service restart reused PID $firstProcessId instead of creating a distinct process"
    }
    Assert-RedisRound $cli $Port "service-test-second" "mingw-second"
    Stop-RedisServiceInstance $server $service $ServiceName $Port $secondProcessId

    $testSucceeded = $true
    Write-Host (
        "ALL SERVICE TESTS PASSED first_pid={0} second_pid={1}" -f
            $firstProcessId, $secondProcessId)
} finally {
    $cleanupErrors = @()
    $current = $null

    if ($serviceCreationAttempted) {
        $cleanupProcessId = 0
        try {
            $current = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
            $serviceRecord = Get-CimInstance Win32_Service -Filter (
                "Name = '{0}'" -f $ServiceName) -ErrorAction SilentlyContinue
            if ($serviceRecord -and [uint32]$serviceRecord.ProcessId -ne 0) {
                $cleanupProcessId = [uint32]$serviceRecord.ProcessId
            }
        } catch {
            $cleanupErrors += "Querying cleanup state: $($_.Exception.Message)"
        }

        if ($current -and
            $current.Status -ne [System.ServiceProcess.ServiceControllerStatus]::Stopped) {
            try {
                Stop-Service -Name $ServiceName -Force -ErrorAction Stop
                $current.WaitForStatus(
                    [System.ServiceProcess.ServiceControllerStatus]::Stopped,
                    [TimeSpan]::FromSeconds(30))
            } catch {
                $cleanupErrors += "Stopping service: $($_.Exception.Message)"
            }
        }
        if ($cleanupProcessId -ne 0) {
            try {
                Wait-ForProcessExit $cleanupProcessId
            } catch {
                $cleanupErrors += "Waiting for PID exit: $($_.Exception.Message)"
            }
        }
        try {
            Wait-ForPortClosed $Port
        } catch {
            $cleanupErrors += "Waiting for port closure: $($_.Exception.Message)"
        }

        try {
            if ($serviceInstalledByRedis) {
                & $server --service-uninstall --service-name $ServiceName | Out-Null
                Assert-LastExitCode "Uninstalling the Redis test service"
            } else {
                $registered = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
                if ($registered) {
                    $registered.Dispose()
                    & sc.exe delete $ServiceName | Out-Null
                    Assert-LastExitCode "Deleting the test service"
                }
            }
        } catch {
            $cleanupErrors += "Deleting service: $($_.Exception.Message)"
        }

        try {
            $deadline = [DateTime]::UtcNow.AddSeconds(30)
            do {
                $registered = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
                if (-not $registered) {
                    break
                }
                $registered.Dispose()
                Start-Sleep -Milliseconds 250
            } while ([DateTime]::UtcNow -lt $deadline)
            if (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue) {
                throw "Service $ServiceName was not deleted"
            }
        } catch {
            $cleanupErrors += "Confirming service deletion: $($_.Exception.Message)"
        }

        try {
            $leftoverProcesses = @(
                Get-MatchingRedisServiceProcesses $ServiceName $server)
            if ($leftoverProcesses.Count -ne 0) {
                throw "Found $($leftoverProcesses.Count) leftover process(es) for service $ServiceName"
            }
        } catch {
            $cleanupErrors += "Checking leftover processes: $($_.Exception.Message)"
        }
    }

    if ($service) {
        $service.Dispose()
    }
    if ($current) {
        $current.Dispose()
    }

    if ($dataDirCreated) {
        if ($testSucceeded) {
            try {
                Remove-Item -Path $dataDir -Recurse -Force -ErrorAction Stop
            } catch {
                $cleanupErrors += "Removing service test directory: $($_.Exception.Message)"
            }
            if (Test-Path $dataDir) {
                $cleanupErrors += "Service test directory still exists: $dataDir"
            }
        } else {
            Write-Host "Preserving failed service test directory for CI diagnostics: $dataDir"
        }
    }

    if ($cleanupErrors.Count -ne 0) {
        throw "Service cleanup failed: $($cleanupErrors -join '; ')"
    }
}
