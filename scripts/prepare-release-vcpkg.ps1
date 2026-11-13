#Requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateNotNullOrEmpty()][string]$ToolDir,
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-fA-F]{40}$')][string]$Baseline
)
$ErrorActionPreference = 'Stop'
$toolRoot = [IO.Path]::GetFullPath($ToolDir)
$expectedRevision = $Baseline.ToLowerInvariant()
$upstream = 'https://github.com/microsoft/vcpkg.git'

function Invoke-VcpkgToolCommand([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program 失败（$LASTEXITCODE）" }
}

function Get-VcpkgToolVersion([string]$Executable) {
    if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) { return $null }
    try {
        $output = & $Executable version 2>&1
        if ($LASTEXITCODE -ne 0) { return $null }
        $text = ($output | Out-String).Trim()
        $match = [regex]::Match($text, '(?m)^vcpkg package management program version (\d{4}-\d{2}-\d{2})(?:-|$)')
        if (-not $match.Success) { return $null }
        return [pscustomobject]@{ Tag = $match.Groups[1].Value; Text = $text }
    } catch { return $null }
}

if (-not $IsWindows -or -not [Environment]::Is64BitProcess) { throw 'vcpkg 发布工具只支持 Windows x64 进程' }
if (-not (Test-Path -LiteralPath (Join-Path $toolRoot '.git'))) {
    New-Item -ItemType Directory -Path $toolRoot -Force | Out-Null
    Invoke-VcpkgToolCommand git @('init', $toolRoot)
    Invoke-VcpkgToolCommand git @('-C', $toolRoot, 'remote', 'add', 'origin', $upstream)
}
$origin = & git -C $toolRoot remote get-url origin
if ($LASTEXITCODE -ne 0 -or $origin -ne $upstream) { throw 'vcpkg 工具目录必须来自固定的 Microsoft vcpkg 仓库' }
$revision = & git -C $toolRoot rev-parse --verify HEAD 2>$null
$baselineChanged = $LASTEXITCODE -ne 0 -or $revision -ne $expectedRevision
if ($baselineChanged) {
    Write-Host "vcpkg 工具基线准备：$expectedRevision"
    Invoke-VcpkgToolCommand git @('-C', $toolRoot, 'fetch', '--depth=1', 'origin', $expectedRevision)
    Invoke-VcpkgToolCommand git @('-C', $toolRoot, 'checkout', '--detach', $expectedRevision)
}
$dirty = @(& git -C $toolRoot status --porcelain --untracked-files=no)
if ($LASTEXITCODE -ne 0 -or $dirty.Count -gt 0) { throw 'vcpkg 工具源码存在未提交修改，拒绝使用或执行该缓存' }
$metadataPath = Join-Path $toolRoot 'scripts/vcpkg-tool-metadata.txt'
$metadata = ConvertFrom-StringData (Get-Content -LiteralPath $metadataPath -Raw)
$expectedTag = $metadata.VCPKG_TOOL_RELEASE_TAG
if ($expectedTag -notmatch '^\d{4}-\d{2}-\d{2}$') { throw 'vcpkg 工具发布版本声明缺失或无效' }
$executable = Join-Path $toolRoot 'vcpkg.exe'
$version = Get-VcpkgToolVersion $executable
if ($baselineChanged -or -not $version -or $version.Tag -ne $expectedTag) {
    $reason = if ($baselineChanged) { '基线更新' }
              elseif (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { '缺少可执行文件' }
              else { '缓存工具版本失配或不可运行' }
    Write-Host "vcpkg bootstrap：$reason"
    Invoke-VcpkgToolCommand (Join-Path $toolRoot 'bootstrap-vcpkg.bat') @('-disableMetrics')
} else {
    Write-Host 'vcpkg 工具缓存命中：保留已有可执行文件'
}
$revision = & git -C $toolRoot rev-parse --verify HEAD
if ($LASTEXITCODE -ne 0 -or $revision -ne $expectedRevision) { throw 'vcpkg 工具准备后的 Git 修订不符合固定基线' }
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw 'vcpkg bootstrap 未生成可执行文件' }
$version = Get-VcpkgToolVersion $executable
if (-not $version -or $version.Tag -ne $expectedTag) { throw "vcpkg 工具可执行版本不符合基线声明 $expectedTag" }
Write-Host $version.Text
Write-Host "vcpkg 工具已验证：$toolRoot，$revision"
