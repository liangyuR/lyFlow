#Requires -Version 7.0
param([switch]$Offline)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$config = Get-Content -LiteralPath (Join-Path $root 'packaging/release.json') -Raw | ConvertFrom-Json
$version = $config.onnxruntimeVersion
$expectedHash = $config.onnxruntimeSha256
$expectedBytes = $config.onnxruntimeBytes
if ($version -notmatch '^\d+\.\d+\.\d+$' -or $expectedHash -notmatch '^[0-9a-f]{64}$' -or $expectedBytes -le 0) {
    throw 'release.json 必须固定 ONNX Runtime 正式版本、归档字节数和 SHA256'
}
$name = "onnxruntime-win-x64-$version"
$parent = Join-Path $root 'third_party/onnxruntime'
$dest = Join-Path $parent $name
$archive = Join-Path $root "build/third-party/$name.zip"
$required = @('lib/onnxruntime.lib', 'lib/onnxruntime.dll', 'lib/onnxruntime_providers_shared.dll',
    'include/onnxruntime_c_api.h', 'include/onnxruntime_cxx_api.h', 'include/onnxruntime_cxx_inline.h',
    'include/onnxruntime_float16.h', 'VERSION_NUMBER', 'LICENSE', 'ThirdPartyNotices.txt')
function Assert-Runtime([string]$Directory) {
    foreach ($file in $required) {
        if (-not (Test-Path -LiteralPath (Join-Path $Directory $file) -PathType Leaf)) {
            throw "ONNX Runtime 缺少必需文件：$file"
        }
    }
    $actualVersion = (Get-Content -LiteralPath (Join-Path $Directory 'VERSION_NUMBER') -Raw).Trim()
    if ($actualVersion -ne $version) { throw "ONNX Runtime 版本不符：$actualVersion，预期 $version" }
}
if (Test-Path -LiteralPath $dest) {
    Assert-Runtime $dest
    Write-Host "ONNX Runtime $version 已验证：$dest"
    exit 0
}
New-Item -ItemType Directory -Path (Split-Path -Parent $archive) -Force | Out-Null
if (-not (Test-Path -LiteralPath $archive -PathType Leaf)) {
    if ($Offline) { throw '离线模式下未找到固定 ONNX Runtime 归档' }
    $url = "https://github.com/microsoft/onnxruntime/releases/download/v$version/$name.zip"
    Write-Host "下载官方 CPU 资产：$url"
    Invoke-WebRequest -Uri $url -OutFile $archive
}
if ((Get-Item -LiteralPath $archive).Length -ne $expectedBytes -or
    (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedHash) {
    throw "ONNX Runtime 官方归档字节数或 SHA256 不符：$archive"
}
$stage = [IO.Path]::GetFullPath((Join-Path $root ('build/onnxruntime-stage-' + [Guid]::NewGuid().ToString('N'))))
$workspacePrefix = $root.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
if (-not $stage.StartsWith($workspacePrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'ONNX Runtime 暂存路径越出功能工作区'
}
try {
    New-Item -ItemType Directory -Path $stage | Out-Null
    Expand-Archive -LiteralPath $archive -DestinationPath $stage
    $prepared = Join-Path $stage $name
    Assert-Runtime $prepared
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
    Move-Item -LiteralPath $prepared -Destination $dest
} finally {
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
}
Write-Host "ONNX Runtime $version 已验证并就位：$dest"
