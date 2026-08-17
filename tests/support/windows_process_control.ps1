param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("FindQForkChild", "CheckIdentity", "Suspend", "Resume")]
    [string]$Action,

    [Parameter(Mandatory = $true)]
    [int]$TargetProcessId,

    [Parameter(Mandatory = $true)]
    [string]$ExpectedExecutable,

    [Parameter(Mandatory = $false)]
    [string]$ExpectedArgument = ""
)

$ErrorActionPreference = "Stop"

if ($Action -eq "FindQForkChild") {
    $expected = [System.IO.Path]::GetFullPath($ExpectedExecutable)
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    $children = @()
    $matches = @()

    do {
        $children = @(
            Get-CimInstance Win32_Process -Filter "ParentProcessId = $TargetProcessId"
        )
        $matches = @(
            $children | Where-Object {
                $_.ExecutablePath -and
                [String]::Equals(
                    [System.IO.Path]::GetFullPath($_.ExecutablePath),
                    $expected,
                    [StringComparison]::OrdinalIgnoreCase)
            }
        )

        if ($matches.Count -eq 1) {
            Write-Output ([int]$matches[0].ProcessId)
            return
        }

        # More than one exact-image direct child is ambiguous and must never
        # be resolved by inspecting the mutable command-line text.
        if ($matches.Count -gt 1) {
            break
        }

        Start-Sleep -Milliseconds 25
    } while ([DateTime]::UtcNow -lt $deadline)

    $details = @(
        $children | ForEach-Object {
            "PID=$($_.ProcessId) ExecutablePath=$($_.ExecutablePath) CommandLine=$($_.CommandLine)"
        }
    ) -join [Environment]::NewLine
    if (-not $details) {
        $details = "<none>"
    }
    throw "Expected exactly one QFork child of process $TargetProcessId, found $($matches.Count). Direct children: $details"
}

if ($Action -eq "CheckIdentity") {
    $processes = @(
        Get-CimInstance Win32_Process -Filter "ProcessId = $TargetProcessId"
    )
    if ($processes.Count -ne 1 -or -not $processes[0].ExecutablePath) {
        exit 1
    }

    $process = $processes[0]
    $expected = [System.IO.Path]::GetFullPath($ExpectedExecutable)
    $actual = [System.IO.Path]::GetFullPath($process.ExecutablePath)
    if (-not [String]::Equals($actual, $expected, [StringComparison]::OrdinalIgnoreCase)) {
        exit 1
    }

    if ($ExpectedArgument -ne "" -and
        ($null -eq $process.CommandLine -or
         $process.CommandLine.IndexOf($ExpectedArgument, [StringComparison]::OrdinalIgnoreCase) -lt 0)) {
        exit 1
    }
    exit 0
}

Add-Type -TypeDefinition @"
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

public static class RedisTestProcessControl
{
    private const uint PROCESS_SUSPEND_RESUME = 0x0800;
    private const uint PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(
        uint processAccess,
        bool inheritHandle,
        int processId);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool QueryFullProcessImageName(
        IntPtr process,
        int flags,
        StringBuilder path,
        ref int size);

    [DllImport("kernel32.dll")]
    private static extern bool CloseHandle(IntPtr handle);

    [DllImport("ntdll.dll")]
    private static extern int NtSuspendProcess(IntPtr process);

    [DllImport("ntdll.dll")]
    private static extern int NtResumeProcess(IntPtr process);

    public static void Control(int processId, string expectedExecutable, bool suspend)
    {
        IntPtr process = OpenProcess(
            PROCESS_SUSPEND_RESUME | PROCESS_QUERY_LIMITED_INFORMATION,
            false,
            processId);
        if (process == IntPtr.Zero)
            throw new Win32Exception(Marshal.GetLastWin32Error(), "OpenProcess failed");

        try
        {
            var path = new StringBuilder(32768);
            int size = path.Capacity;
            if (!QueryFullProcessImageName(process, 0, path, ref size))
                throw new Win32Exception(
                    Marshal.GetLastWin32Error(),
                    "QueryFullProcessImageName failed");

            string actual = System.IO.Path.GetFullPath(path.ToString());
            string expected = System.IO.Path.GetFullPath(expectedExecutable);
            if (!String.Equals(actual, expected, StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException(
                    "Refusing to control unexpected process: " + actual);

            int status = suspend ? NtSuspendProcess(process) : NtResumeProcess(process);
            if (status != 0)
                throw new InvalidOperationException(
                    String.Format("Native process control failed with NTSTATUS 0x{0:X8}", status));
        }
        finally
        {
            CloseHandle(process);
        }
    }
}
"@

[RedisTestProcessControl]::Control(
    $TargetProcessId,
    $ExpectedExecutable,
    $Action -eq "Suspend")
