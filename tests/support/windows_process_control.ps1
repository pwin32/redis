param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("Suspend", "Resume")]
    [string]$Action,

    [Parameter(Mandatory = $true)]
    [int]$TargetProcessId,

    [Parameter(Mandatory = $true)]
    [string]$ExpectedExecutable
)

$ErrorActionPreference = "Stop"

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
