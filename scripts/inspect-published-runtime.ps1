#Requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$RuntimeDir,
    [Parameter(Mandatory)][string]$ReportPath,
    [Parameter(Mandatory)][string]$ExpectedVersion,
    [Parameter(Mandatory)][string]$ExpectedRevision,
    [switch]$ProbeProcess
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'runtime-nuget-common.ps1')
if (-not $IsWindows -or -not [Environment]::Is64BitProcess) { throw 'Runtime inspection requires Windows x64 PowerShell.' }
$RuntimeDir = (Resolve-Path -LiteralPath $RuntimeDir).Path
$ReportPath = [IO.Path]::GetFullPath($ReportPath)
New-Item -ItemType Directory -Path (Split-Path -Parent $ReportPath) -Force | Out-Null
if (-not $ProbeProcess) {
    $start = [Diagnostics.ProcessStartInfo]::new((Join-Path $PSHOME 'pwsh.exe'))
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($argument in @('-NoLogo', '-NoProfile', '-File', $PSCommandPath, '-RuntimeDir', $RuntimeDir, '-ReportPath', $ReportPath, '-ExpectedVersion', $ExpectedVersion, '-ExpectedRevision', $ExpectedRevision, '-ProbeProcess')) { $start.ArgumentList.Add($argument) }
    $start.Environment['PATH'] = "$env:SystemRoot\System32;$env:SystemRoot"
    $process = [Diagnostics.Process]::Start($start)
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(120000)) { $process.Kill($true); $process.WaitForExit(); throw 'Published runtime inspection timed out.' }
        $log = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
        $log | Set-Content -LiteralPath "$ReportPath.log" -Encoding utf8NoBOM
        if ($process.ExitCode -ne 0) { throw "Published runtime inspection failed: $log" }
    } finally { $process.Dispose() }
    return
}
$metadata = Assert-RuntimeNuGetMetadata -RuntimeDir $RuntimeDir -ExpectedVersion $ExpectedVersion -ExpectedRevision $ExpectedRevision
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Linq;
using System.Diagnostics;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class LyFlowPublishedProbe {
    [DllImport("kernel32", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr LoadLibraryExW(string path, IntPtr file, uint flags);
    [DllImport("kernel32", CharSet=CharSet.Ansi, ExactSpelling=true, SetLastError=true)] static extern IntPtr GetProcAddress(IntPtr module, string name);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate IntPtr GetString();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void FreeString(IntPtr value);
    static T Function<T>(IntPtr module, string name) where T : Delegate {
        var pointer = GetProcAddress(module, name);
        if (pointer == IntPtr.Zero) throw new Exception("Missing runtime export: " + name);
        return Marshal.GetDelegateForFunctionPointer<T>(pointer);
    }
    public static IntPtr Load(string path) {
        var handle = LoadLibraryExW(path, IntPtr.Zero, 0x900);
        if (handle == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), path);
        return handle;
    }
    public static string Read(IntPtr module, string name, bool owned) {
        var value = Function<GetString>(module, name)();
        if (value == IntPtr.Zero) throw new Exception("Null runtime export result: " + name);
        try { return Marshal.PtrToStringUTF8(value); }
        finally { if (owned) Function<FreeString>(module, "lyflow_string_free")(value); }
    }
    public static string[] Modules(string root) {
        var packaged = Directory.GetFiles(root, "*.dll").Select(Path.GetFileName).ToHashSet(StringComparer.OrdinalIgnoreCase);
        var prefix = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        var loaded = Process.GetCurrentProcess().Modules.Cast<ProcessModule>().Where(m => packaged.Contains(Path.GetFileName(m.FileName))).Select(m => m.FileName).ToArray();
        foreach (var path in loaded) if (!Path.GetFullPath(path).StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) throw new Exception("Runtime module loaded outside package: " + path);
        return loaded;
    }
}
'@
$core = [LyFlowPublishedProbe]::Load((Join-Path $RuntimeDir 'lyflow_core.dll'))
$version = [LyFlowPublishedProbe]::Read($core, 'lyflow_version', $false)
$abi = [LyFlowPublishedProbe]::Read($core, 'lyflow_abi_version_string', $false)
$revision = [LyFlowPublishedProbe]::Read($core, 'lyflow_source_revision', $false)
if ($version -cne $ExpectedVersion -or $abi -cne $ExpectedVersion -or $revision -cne $ExpectedRevision) { throw 'Actual DLL version, ABI or revision differs from the selected formal release.' }
$problems = [LyFlowPublishedProbe]::Read($core, 'lyflow_manifest_problems', $true)
if ($problems.Trim()) { throw "Runtime manifest self-check failed: $problems" }
$manifest = [LyFlowPublishedProbe]::Read($core, 'lyflow_manifest_json', $true) | ConvertFrom-Json
$report = @{ status = 'passed'; engineVersion = $version; abiVersion = $abi; sourceRevision = $revision; manifest = $manifest; loadedModules = @([LyFlowPublishedProbe]::Modules($RuntimeDir)) }
Write-ReleaseJson -Value $report -Path $ReportPath
