#Requires -Version 7.0
param(
  [string]$SourceRoot = (Split-Path -Parent $PSScriptRoot),
  [string]$OutputDir,
  [ValidateRange(1, 67108864)][int]$Elements = 4194304,
  [ValidateRange(1, 10000)][int]$Iterations = 101
)
$ErrorActionPreference = 'Stop'
$source = [IO.Path]::GetFullPath($SourceRoot)
$workspace = Split-Path -Parent $PSScriptRoot
$output = if ($OutputDir) { [IO.Path]::GetFullPath($OutputDir) } else { Join-Path $workspace 'build/tensor-metadata-bench' }
$benchmark = Join-Path $workspace 'core/tools/bench_tensor_metadata.cpp'
$data = Join-Path $source 'core/src/data.cpp'
$writer = Join-Path $source 'core/src/json_writer.cpp'
$status = Join-Path $source 'core/src/status.cpp'
$header = Join-Path $source 'core/include/lyflow/data.h'
foreach ($file in @($benchmark, $data, $writer, $status, $header)) {
  if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing source: $file" }
}
New-Item -ItemType Directory -Path $output -Force | Out-Null
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'Visual Studio C++ tools not found' }
$vcvars = Join-Path $vsPath 'VC/Auxiliary/Build/vcvars64.bat'
$executable = Join-Path $output 'bench-tensor-metadata.exe'
$include = Join-Path $source 'core/include'
$thirdParty = Join-Path $source 'core/third_party'
$objects = $output.Replace('\', '/') + '/'
$line = "call `"$vcvars`" >nul 2>&1 && cl /nologo /O2 /MD /EHsc /std:c++17 /utf-8 /I`"$include`" /I`"$thirdParty`" /Fo`"$objects`" /Fe`"$executable`" `"$benchmark`" `"$data`" `"$writer`" `"$status`""
& cmd /c $line *> (Join-Path $output 'compile.log')
if ($LASTEXITCODE -ne 0) {
  Get-Content -LiteralPath (Join-Path $output 'compile.log')
  throw "Metadata benchmark compilation failed: $LASTEXITCODE"
}
$raw = & $executable $Elements $Iterations
if ($LASTEXITCODE -ne 0) { throw "Metadata benchmark failed: $LASTEXITCODE" }
$result = $raw | ConvertFrom-Json -AsHashtable
$result.sourceRoot = $source
$result.timestampUtc = [DateTime]::UtcNow.ToString('o')
$result.sourceHashes = [ordered]@{}
foreach ($file in @($benchmark, $data, $writer, $status, $header)) {
  $result.sourceHashes[$file] = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
}
$result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding utf8
$result | ConvertTo-Json -Depth 5
