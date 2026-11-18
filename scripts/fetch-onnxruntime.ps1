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
function Assert-Archive([string]$Path) {
    $actualBytes = (Get-Item -LiteralPath $Path).Length
    if ($actualBytes -ne $expectedBytes -or
        (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedHash) {
        throw "ONNX Runtime 归档字节数或 SHA256 不符（收到 $actualBytes 字节，应为 $expectedBytes）：$Path"
    }
}
if (Test-Path -LiteralPath $dest) {
    Assert-Runtime $dest
    Write-Host "ONNX Runtime $version 已验证：$dest"
    exit 0
}
New-Item -ItemType Directory -Path (Split-Path -Parent $archive) -Force | Out-Null
if (Test-Path -LiteralPath $archive -PathType Leaf) {
    Assert-Archive $archive
} else {
    if ($Offline) { throw '离线模式下未找到固定 ONNX Runtime 归档' }
    $urls = @(
        "https://github.com/microsoft/onnxruntime/releases/download/v$version/$name.zip",
        "https://downloads.sourceforge.net/project/onnx-runtime.mirror/v$version/$name.zip"
    )
    $pending = $archive + '.' + [Guid]::NewGuid().ToString('N') + '.part'
    for ($i = 0; $i -lt $urls.Count; $i++) {
        try {
            Write-Host "下载固定 CPU 归档：$($urls[$i])"
            if ($i -eq 0) {
                Invoke-WebRequest -Uri $urls[$i] -OutFile $pending
            } else {
                & curl.exe --fail --location --silent --show-error --retry 2 --output $pending $urls[$i]
                if ($LASTEXITCODE -ne 0) { throw "ONNX Runtime 镜像下载失败 (exit $LASTEXITCODE)" }
            }
            Assert-Archive $pending
            Move-Item -LiteralPath $pending -Destination $archive
            break
        } catch {
            if ($i -eq $urls.Count - 1) { throw }
            Write-Warning "微软原始地址不可用，尝试镜像并校验同一归档：$($_.Exception.Message)"
        } finally {
            if (Test-Path -LiteralPath $pending) { Remove-Item -LiteralPath $pending -Force }
        }
    }
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
