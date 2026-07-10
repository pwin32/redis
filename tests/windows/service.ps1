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

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "The service test requires an elevated Windows token"
}

if ($ServiceName -ieq "Redis") {
    throw "Refusing to use the installed Redis service name"
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

$probe = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $Port)
try {
    $probe.Start()
} finally {
    $probe.Stop()
}

New-Item -ItemType Directory -Path $dataDir -Force | Out-Null
$networkService = ([Security.Principal.SecurityIdentifier]"S-1-5-20").Translate(
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

$binaryPath = (
    '"{0}" --service-run --service-name {1} --persistence-available no --port {2} --bind 127.0.0.1 --dir "{3}" --logfile "{4}" --syslog-enabled yes --syslog-ident {1}' -f
        $server, $ServiceName, $Port, $dataDir, $logFile
)
$service = $null

try {
    $serviceParameters = @{
        Name = $ServiceName
        DisplayName = $ServiceName
        BinaryPathName = $binaryPath
        StartupType = "Manual"
    }
    New-Service @serviceParameters | Out-Null
    & sc.exe config $ServiceName "obj=" "NT AUTHORITY\NetworkService" | Out-Null
    Assert-LastExitCode "Configuring the service account"
    $service = Get-Service -Name $ServiceName

    $eventStart = Get-Date
    & $server --service-start --service-name $ServiceName
    Assert-LastExitCode "Starting the Redis service"
    $service.WaitForStatus(
        [System.ServiceProcess.ServiceControllerStatus]::Running,
        [TimeSpan]::FromSeconds(30))
    Wait-ForRedis $cli $Port

    $setReply = & $cli -h 127.0.0.1 -p $Port SET service-test mingw
    Assert-LastExitCode "Writing through the Redis service"
    if ($setReply -ne "OK") {
        throw "Unexpected SET reply: $setReply"
    }

    $getReply = & $cli -h 127.0.0.1 -p $Port GET service-test
    Assert-LastExitCode "Reading through the Redis service"
    if ($getReply -ne "mingw") {
        throw "Unexpected GET reply: $getReply"
    }

    $process = Get-CimInstance Win32_Process -Filter "Name = 'redis-server.exe'" |
        Where-Object {
            $_.ExecutablePath -ieq $server -and
            $_.CommandLine -match [regex]::Escape($ServiceName)
        }
    if (-not $process) {
        throw "The running service is not using the repository executable"
    }

    $eventDeadline = [DateTime]::UtcNow.AddSeconds(15)
    $event = $null
    do {
        $event = Get-WinEvent -FilterHashtable @{
            LogName = "Application"
            ProviderName = "redis"
            StartTime = $eventStart
        } -ErrorAction SilentlyContinue | Where-Object {
            $_.Message -match [regex]::Escape($ServiceName)
        } | Select-Object -First 1
        if (-not $event) {
            Start-Sleep -Milliseconds 250
        }
    } while (-not $event -and [DateTime]::UtcNow -lt $eventDeadline)
    if (-not $event) {
        throw "The Redis service did not emit a tagged Application event"
    }

    & $server --service-stop --service-name $ServiceName
    Assert-LastExitCode "Stopping the Redis service"
    $service.WaitForStatus(
        [System.ServiceProcess.ServiceControllerStatus]::Stopped,
        [TimeSpan]::FromSeconds(30))

    Write-Host "ALL SERVICE TESTS PASSED"
} finally {
    $current = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if ($current -and $current.Status -ne [System.ServiceProcess.ServiceControllerStatus]::Stopped) {
        Stop-Service -Name $ServiceName -Force -ErrorAction SilentlyContinue
        $current.WaitForStatus(
            [System.ServiceProcess.ServiceControllerStatus]::Stopped,
            [TimeSpan]::FromSeconds(30))
    }
    if ($service) {
        $service.Dispose()
    }
    if ($current) {
        $current.Dispose()
    }

    if (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue) {
        & sc.exe delete $ServiceName | Out-Null
        Assert-LastExitCode "Deleting the test service"
    }

    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while ((Get-Service -Name $ServiceName -ErrorAction SilentlyContinue) -and
           [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 250
    }
    if (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue) {
        throw "Service $ServiceName was not deleted"
    }

    Remove-Item -Path $dataDir -Recurse -Force -ErrorAction SilentlyContinue
}
