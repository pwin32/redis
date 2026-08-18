param(
    [Parameter(Mandatory = $true)][string]$PackageDir,
    [Parameter(Mandatory = $true)][int]$MasterPort,
    [Parameter(Mandatory = $true)][int]$ReplicaPort,
    [Parameter(Mandatory = $true)][string]$OutputDir,
    [Parameter(Mandatory = $true)][string]$ExpectedVersion
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ($MasterPort -lt 1 -or $MasterPort -gt 65535 -or
    $ReplicaPort -lt 1 -or $ReplicaPort -gt 65535 -or
    $MasterPort -eq $ReplicaPort) {
    throw 'MasterPort and ReplicaPort must be distinct TCP ports from 1 through 65535.'
}
if ($ExpectedVersion -notmatch '^\d+\.\d+\.\d+$') {
    throw "Invalid expected version: $ExpectedVersion"
}

$package = (Resolve-Path -LiteralPath $PackageDir).Path
$server = Join-Path $package 'redis-server.exe'
$cli = Join-Path $package 'redis-cli.exe'
$benchmark = Join-Path $package 'redis-benchmark.exe'
foreach ($binary in @($server, $cli, $benchmark)) {
    if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) {
        throw "Required packaged binary not found: $binary"
    }
}

$output = if ([System.IO.Path]::IsPathRooted($OutputDir)) {
    [System.IO.Path]::GetFullPath($OutputDir)
} else {
    [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $OutputDir))
}
if (Test-Path -LiteralPath $output) {
    if ((Get-ChildItem -LiteralPath $output -Force | Measure-Object).Count -ne 0) {
        throw "Output directory must be empty: $output"
    }
} else {
    New-Item -ItemType Directory -Path $output | Out-Null
}

$masterDir = Join-Path $output 'master'
$replicaDir = Join-Path $output 'replica'
New-Item -ItemType Directory -Path $masterDir, $replicaDir | Out-Null
$masterConfig = Join-Path $masterDir 'redis.conf'
$replicaConfig = Join-Path $replicaDir 'redis.conf'

function Convert-ToConfigPath([string]$Path) {
    return $Path.Replace('\', '/')
}

@(
    'bind 127.0.0.1'
    "port $MasterPort"
    'protected-mode yes'
    'save ""'
    'appendonly no'
    'repl-diskless-sync no'
    'repl-compression 0'
    "dir `"$(Convert-ToConfigPath $masterDir)`""
    'dbfilename master.rdb'
    'logfile ""'
) | Set-Content -LiteralPath $masterConfig -Encoding UTF8

@(
    'bind 127.0.0.1'
    "port $ReplicaPort"
    'protected-mode yes'
    'save ""'
    'appendonly no'
    'repl-compression 0'
    "dir `"$(Convert-ToConfigPath $replicaDir)`""
    'dbfilename replica.rdb'
    'logfile ""'
) | Set-Content -LiteralPath $replicaConfig -Encoding UTF8

$master = $null
$replica = $null

function Invoke-RedisCli([int]$Port, [string[]]$Arguments) {
    $outputText = & $cli -h 127.0.0.1 -p $Port --raw @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "redis-cli failed on port $Port: $outputText"
    }
    return (($outputText | Out-String).Trim())
}

function Wait-ForRedis([System.Diagnostics.Process]$Process, [int]$Port) {
    $deadline = [DateTime]::UtcNow.AddSeconds(60)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($Process.HasExited) {
            throw "Redis PID $($Process.Id) exited before port $Port became ready."
        }
        try {
            if ((Invoke-RedisCli $Port @('PING')) -eq 'PONG') { return }
        } catch {
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Redis PID $($Process.Id) did not become ready on port $Port."
}

function Stop-ExactRedis([System.Diagnostics.Process]$Process, [int]$Port) {
    if ($null -eq $Process -or $Process.HasExited) { return }
    try { Invoke-RedisCli $Port @('SHUTDOWN', 'NOSAVE') | Out-Null } catch {}
    if (-not $Process.WaitForExit(30000)) {
        $Process.Kill($true)
        if (-not $Process.WaitForExit(10000)) {
            throw "Unable to stop exact Redis PID $($Process.Id)."
        }
    }
}

function Start-PackagedRedis([string]$Config, [string]$WorkingDir, [string]$Name) {
    $stdout = Join-Path $output "$Name.stdout.log"
    $stderr = Join-Path $output "$Name.stderr.log"
    $process = Start-Process -FilePath $server -ArgumentList @($Config) `
        -WorkingDirectory $WorkingDir -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr -PassThru
    $actualPath = (Get-Process -Id $process.Id -FileVersionInfo).FileName
    if (-not [string]::Equals(
        [System.IO.Path]::GetFullPath($actualPath),
        [System.IO.Path]::GetFullPath($server),
        [System.StringComparison]::OrdinalIgnoreCase)) {
        $process.Kill($true)
        throw "PID $($process.Id) does not belong to the packaged server."
    }
    return $process
}

try {
    $master = Start-PackagedRedis $masterConfig $masterDir 'master'
    Wait-ForRedis $master $MasterPort
    $serverInfo = Invoke-RedisCli $MasterPort @('INFO', 'server')
    if ($serverInfo -notmatch "(?m)^redis_version:$([regex]::Escape($ExpectedVersion))`r?$") {
        throw "Master does not report Redis $ExpectedVersion."
    }

    $seedLog = Join-Path $output 'seed-benchmark.log'
    & $benchmark -h 127.0.0.1 -p $MasterPort -c 20 -P 4 -n 10000 `
        -r 10000 -d 128 -t set -q *> $seedLog
    if ($LASTEXITCODE -ne 0) { throw 'Packaged seed benchmark failed.' }
    if ((Invoke-RedisCli $MasterPort @('SET', 'package:replication:marker', 'initial')) -ne 'OK') {
        throw 'Unable to write the initial replication marker.'
    }

    $replica = Start-PackagedRedis $replicaConfig $replicaDir 'replica'
    Wait-ForRedis $replica $ReplicaPort
    if ((Invoke-RedisCli $ReplicaPort @('REPLICAOF', '127.0.0.1', "$MasterPort")) -ne 'OK') {
        throw 'REPLICAOF was not accepted.'
    }

    $deadline = [DateTime]::UtcNow.AddSeconds(120)
    $replicationInfo = ''
    while ([DateTime]::UtcNow -lt $deadline) {
        $replicationInfo = Invoke-RedisCli $ReplicaPort @('INFO', 'replication')
        $marker = Invoke-RedisCli $ReplicaPort @('GET', 'package:replication:marker')
        if ($replicationInfo -match '(?m)^master_link_status:up\r?$' -and $marker -eq 'initial') {
            break
        }
        if ($replica.HasExited) { throw 'Replica exited during full synchronization.' }
        Start-Sleep -Milliseconds 250
    }
    if ($replicationInfo -notmatch '(?m)^master_link_status:up\r?$') {
        throw 'Replica did not establish an upstream link.'
    }
    if ((Invoke-RedisCli $ReplicaPort @('GET', 'package:replication:marker')) -ne 'initial') {
        throw 'Replica did not receive the initial full synchronization data.'
    }

    if ((Invoke-RedisCli $MasterPort @('SET', 'package:replication:marker', 'incremental')) -ne 'OK') {
        throw 'Unable to write the incremental replication marker.'
    }
    $waitResult = Invoke-RedisCli $MasterPort @('WAIT', '1', '30000')
    if ([int]$waitResult -lt 1) { throw 'WAIT did not observe the packaged replica.' }
    if ((Invoke-RedisCli $ReplicaPort @('GET', 'package:replication:marker')) -ne 'incremental') {
        throw 'Replica did not receive the incremental update.'
    }

    Invoke-RedisCli $MasterPort @('INFO', 'replication') |
        Set-Content -LiteralPath (Join-Path $output 'master-info-replication.txt') -Encoding UTF8
    Invoke-RedisCli $ReplicaPort @('INFO', 'replication') |
        Set-Content -LiteralPath (Join-Path $output 'replica-info-replication.txt') -Encoding UTF8
    "PACKAGE_REPLICATION_OK version=$ExpectedVersion master_port=$MasterPort replica_port=$ReplicaPort" |
        Tee-Object -FilePath (Join-Path $output 'summary.txt')
} finally {
    Stop-ExactRedis $replica $ReplicaPort
    Stop-ExactRedis $master $MasterPort
}
