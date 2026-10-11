[CmdletBinding()]
param(
    [string]$RuntimeDir,
    [string]$SdkDir,
    [string]$ArchiveDir,
    [Parameter(Mandatory)][string]$ReportPath,
    [switch]$InspectOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
$ReportPath = [IO.Path]::GetFullPath($ReportPath)
$reportParent = Split-Path -Parent $ReportPath
New-Item -ItemType Directory -Path $reportParent -Force | Out-Null
$work = Join-Path $root ('build/release-verification/发布 验收 ' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work -Force | Out-Null
$cwd = Join-Path $work 'unrelated working directory'
New-Item -ItemType Directory -Path $cwd -Force | Out-Null
$checks = [Collections.Generic.List[object]]::new()

function Invoke-Checked([string]$Exe, [string[]]$Arguments) {
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe failed with exit code $LASTEXITCODE" }
}
function Invoke-Isolated([string]$Exe, [string[]]$Arguments, [string]$Name, [switch]$ExpectFailure) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $Exe
    $info.WorkingDirectory = $cwd
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    $info.Environment['PATH'] = "$env:SystemRoot\System32;$env:SystemRoot"
    foreach ($key in @($info.Environment.Keys)) { if ($key -like 'LYFLOW_*') { $info.Environment.Remove($key) | Out-Null } }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    if (-not $process.Start()) { throw "Could not start $Name" }
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(120000)) { $process.Kill($true); throw "$Name timed out" }
    $log = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
    $log | Set-Content -LiteralPath (Join-Path $work "$Name.log") -Encoding utf8
    $code = $process.ExitCode
    $process.Dispose()
    if ($ExpectFailure) {
        if ($code -eq 0) { throw "$Name unexpectedly succeeded" }
    } elseif ($code -ne 0) { throw "$Name failed ($code): $log" }
    $checks.Add([ordered]@{ name = $Name; status = 'passed'; exitCode = $code })
}
function Build-Probe([string]$Kind) {
    $manifest = Join-Path $root "tests/release/$Kind/Cargo.toml"
    $target = Join-Path $root "build/release-probes/$Kind"
    Invoke-Checked cargo @('build', '--locked', '--release', '--manifest-path', $manifest, '--target-dir', $target)
    $name = if ($Kind -eq 'probe') { 'lyflow-release-probe.exe' } else { 'lyflow-gs-release-probe.exe' }
    return Join-Path $target "release/$name"
}
function Invoke-Probe([string]$Probe, [string]$Runtime, [string]$Name, [string]$Mode = 'full', [switch]$Sdk, [switch]$ExpectFailure) {
    $target = Join-Path $Runtime '__lyflow_release_probe.exe'
    if (Test-Path -LiteralPath $target) { throw 'Reserved release probe filename already exists in package.' }
    Copy-Item -LiteralPath $Probe -Destination $target
    try {
        $output = Join-Path $work "$Name.json"
        $arguments = @($Runtime, $output, $Mode, $fixtures)
        if ($Sdk) { $arguments += 'sdk' }
        Invoke-Isolated $target $arguments $Name -ExpectFailure:$ExpectFailure
        if (-not $ExpectFailure) { return Get-Content -LiteralPath $output -Raw | ConvertFrom-Json }
    } finally { Remove-Item -LiteralPath $target -Force }
}
try {
    if (-not $IsWindows -or -not [Environment]::Is64BitProcess) { throw 'Release verification requires Windows x64 PowerShell.' }
    if ($ArchiveDir) {
        if ($RuntimeDir -or $SdkDir -or $InspectOnly) { throw '-ArchiveDir cannot be combined with directory or inspection inputs.' }
        $archives = Get-ChildItem -LiteralPath $ArchiveDir -File
        $runtimeArchive = @($archives | Where-Object Name -Match '^lyflow-runtime-win-x64-v\d+\.\d+\.\d+\.zip$')
        $sdkArchive = @($archives | Where-Object Name -Match '^lyflow-sdk-win-x64-v\d+\.\d+\.\d+\.zip$')
        if ($runtimeArchive.Count -ne 1 -or $sdkArchive.Count -ne 1) { throw 'Expected exactly one runtime ZIP and one SDK ZIP.' }
        if (($runtimeArchive[0].Name -replace '^lyflow-runtime-', '') -ne ($sdkArchive[0].Name -replace '^lyflow-sdk-', '')) { throw 'Runtime/SDK ZIP versions differ.' }
        $RuntimeDir = Join-Path $work '运行 库'
        $SdkDir = Join-Path $work '开发 SDK'
        [IO.Compression.ZipFile]::ExtractToDirectory($runtimeArchive[0].FullName, $RuntimeDir)
        [IO.Compression.ZipFile]::ExtractToDirectory($sdkArchive[0].FullName, $SdkDir)
    }
    if (-not $RuntimeDir) { throw '-RuntimeDir or -ArchiveDir is required.' }
    $RuntimeDir = (Resolve-Path -LiteralPath $RuntimeDir).Path
    $fixtures = Join-Path $work 'synthetic fixtures'
    Invoke-Checked python @((Join-Path $root 'tests/release/make_fixtures.py'), '--output-dir', $fixtures)
    $probe = Build-Probe 'probe'
    if ($InspectOnly) {
        $inspection = Invoke-Probe $probe $RuntimeDir 'inspection' 'inspect'
        $inspection | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $ReportPath -Encoding utf8
        return
    }
    if (-not $SdkDir) { throw '-SdkDir is required for full verification.' }
    $SdkDir = (Resolve-Path -LiteralPath $SdkDir).Path
    $sdkBin = Join-Path $SdkDir 'bin'
    $runtime = Invoke-Probe $probe $RuntimeDir 'runtime'
    $sdk = Invoke-Probe $probe $sdkBin 'sdk-runtime' -Sdk
    $runtimeMetadata = Get-Content -LiteralPath (Join-Path $RuntimeDir 'lyflow-runtime.json') -Raw | ConvertFrom-Json
    $sdkMetadata = Get-Content -LiteralPath (Join-Path $sdkBin 'lyflow-runtime.json') -Raw | ConvertFrom-Json
    foreach ($field in @('engineVersion','abiVersion','sourceRevision','architecture','buildConfiguration')) {
        if ($runtimeMetadata.$field -ne $sdkMetadata.$field) { throw "SDK/runtime $field mismatch" }
    }
    Invoke-Isolated (Join-Path $sdkBin 'lyflow.exe') @('manifest','--check') 'cli-manifest'
    $gsProbe = Build-Probe 'gs-probe'
    $gs = Invoke-Probe $gsProbe $RuntimeDir 'gs-fixed-client'
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $vs) { throw 'MSVC x64 tools not available.' }
    $vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
    $cmake = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
    $ninja = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
    if (-not (Test-Path -LiteralPath $cmake)) { $cmake = (Get-Command cmake).Source }
    if (-not (Test-Path -LiteralPath $ninja)) { $ninja = (Get-Command ninja).Source }
    $consumer = Join-Path $work 'cpp consumer'
    $source = Join-Path $root 'examples/consumer'
    $configure = "call `"$vcvars`" >nul && `"$cmake`" -S `"$source`" -B `"$consumer`" -G Ninja -DCMAKE_MAKE_PROGRAM=`"$ninja`" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH=`"$SdkDir`" -DCMAKE_TOOLCHAIN_FILE= && `"$cmake`" --build `"$consumer`""
    Invoke-Checked cmd @('/d', '/s', '/c', $configure)
    $graph = Join-Path $work 'synthetic-cloud.json'
    '{"schemaVersion":1,"id":"release-cloud","name":"release cloud","nodes":[{"id":"src","op":"gen.synthetic","params":{"pointCount":4096}},{"id":"pipe","op":"util.reroute"}],"edges":[{"id":"e","from":{"node":"src","port":"cloud"},"to":{"node":"pipe","port":"in"}}],"outputs":{"cloud":{"node":"pipe","port":"out"}}}' | Set-Content -LiteralPath $graph -Encoding utf8
    $consumerDll = Join-Path $consumer 'lyflow_core.dll'
    Invoke-Isolated (Join-Path $consumer 'embed_minimal.exe') @($consumerDll) 'cpp-minimal'
    Invoke-Isolated (Join-Path $consumer 'embed_concurrent.exe') @('--dll',$consumerDll,'--threads','2',$graph,$graph) 'cpp-concurrent'
    Invoke-Isolated (Join-Path $consumer 'embed_inject.exe') @('--dll',$consumerDll,'--node','src','--ports','cloud',$graph) 'cpp-inject'
    Invoke-Isolated (Join-Path $sdkBin 'lyflow.exe') @('run',$graph) 'cli-run'
    $cases = @('missing-core','missing-onnx','wrong-abi','wrong-architecture','wrong-version','missing-listed-file')
    foreach ($case in $cases) {
        $negative = Join-Path $work $case
        Copy-Item -LiteralPath $RuntimeDir -Destination $negative -Recurse
        $metadataPath = Join-Path $negative 'lyflow-runtime.json'
        $metadata = Get-Content -LiteralPath $metadataPath -Raw | ConvertFrom-Json
        switch ($case) {
            'missing-core' { Remove-Item -LiteralPath (Join-Path $negative 'lyflow_core.dll') }
            'missing-onnx' { Remove-Item -LiteralPath (Join-Path $negative 'onnxruntime.dll') }
            'wrong-abi' { $metadata.abiVersion = [int]$metadata.abiVersion + 1 }
            'wrong-architecture' { $metadata.architecture = 'x86' }
            'wrong-version' { $metadata.engineVersion = '0.0.0' }
            'missing-listed-file' { $metadata.files += [pscustomobject]@{ path = 'missing-required-license.txt' } }
        }
        $metadata | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $metadataPath -Encoding utf8
        Invoke-Probe $probe $negative $case -ExpectFailure
    }
    [ordered]@{
        status = 'passed'; engineVersion = $runtime.engineVersion; abiVersion = $runtime.abiVersion
        sourceRevision = $runtimeMetadata.sourceRevision; checks = @($checks.ToArray())
        runtime = $runtime; sdk = $sdk; gsClient = $gs
        gsClientRevision = '5b796c35d6cafd441a24bc09e620aaae399ca5ac'
        evidenceDirectory = $work
        scope = 'Synthetic software verification; customer images, physical devices and field timing not covered.'
    } | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $ReportPath -Encoding utf8
} catch {
    [ordered]@{status='failed'; error=$_.Exception.Message; checks=@($checks.ToArray()); evidenceDirectory=$work} |
        ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $ReportPath -Encoding utf8
    throw
}
