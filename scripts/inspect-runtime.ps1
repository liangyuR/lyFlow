[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$RuntimeDir,
    [Parameter(Mandatory)][string]$ReportPath
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'release-common.ps1')
$root = Split-Path -Parent $PSScriptRoot
if (-not $IsWindows -or -not [Environment]::Is64BitProcess) { throw 'Runtime inspection requires Windows x64 PowerShell.' }
$RuntimeDir = (Resolve-Path -LiteralPath $RuntimeDir).Path
$ReportPath = [IO.Path]::GetFullPath($ReportPath)
New-Item -ItemType Directory -Path (Split-Path -Parent $ReportPath) -Force | Out-Null
$targetDir = Join-Path $root 'build/release-probes/probe'
& cargo build --locked --release --manifest-path (Join-Path $root 'tests/release/probe/Cargo.toml') --target-dir $targetDir
Assert-ReleaseExit 'Build runtime inspection probe'
$target = Join-Path $RuntimeDir '__lyflow_release_probe.exe'
if (Test-Path -LiteralPath $target) { throw 'Reserved inspection probe filename already exists in runtime.' }
Copy-Item -LiteralPath (Join-Path $targetDir 'release/lyflow-release-probe.exe') -Destination $target
try {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $target
    $info.WorkingDirectory = Split-Path -Parent $ReportPath
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.ArgumentList.Add($RuntimeDir)
    $info.ArgumentList.Add($ReportPath)
    $info.Environment['PATH'] = "$env:SystemRoot\System32;$env:SystemRoot"
    foreach ($key in @($info.Environment.Keys)) { if ($key -like 'LYFLOW_*') { $info.Environment.Remove($key) | Out-Null } }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    try {
        if (-not $process.Start()) { throw 'Could not start runtime inspection' }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(120000)) { $process.Kill($true); $process.WaitForExit(); throw 'Runtime inspection timed out' }
        $log = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
        $log | Set-Content -LiteralPath "$ReportPath.log" -Encoding utf8
        if ($process.ExitCode -ne 0) { throw "Runtime inspection failed ($($process.ExitCode)): $log" }
    } finally { $process.Dispose() }
    $inspection = Get-Content -LiteralPath $ReportPath -Raw | ConvertFrom-Json
    if ($inspection.status -cne 'passed') { throw 'Runtime inspection did not report passed.' }
} finally { Remove-Item -LiteralPath $target -Force }
