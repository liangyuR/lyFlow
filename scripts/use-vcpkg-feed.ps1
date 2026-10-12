#Requires -Version 7.0
# 让 vcpkg 先从 GitHub Packages 的 NuGet 源下载编好的二进制包，没有才编（docs/deployment/packaging.md「依赖二进制缓存」）。
# 只改当前进程的环境变量（VCPKG_BINARY_SOURCES 等），直接调用或点源都行：
#
#   $env:LYFLOW_VCPKG_FEED_TOKEN = '<带 read:packages 的 PAT>'
#   . scripts/use-vcpkg-feed.ps1 -VcpkgRoot C:\vcpkg
#
# 令牌只写进构建目录里的临时 nuget.config（不进仓库、不进 URL、不打印）。没有令牌时什么都不做，
# vcpkg 照旧只用本地缓存。命中的前提是三元组、vcpkg 基线与 MSVC 工具集都与写入方相同 ——
# vcpkg 按包的 ABI 哈希取缓存，任何一项不同就是另一个包，只会退回编译，不会用错二进制。
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$VcpkgRoot,
    [ValidateSet('read', 'readwrite')][string]$Mode = 'read',
    [string]$Feed = $(if ($env:LYFLOW_VCPKG_FEED) { $env:LYFLOW_VCPKG_FEED } else { 'https://nuget.pkg.github.com/liangyuR/index.json' }),
    [string]$User = $(if ($env:LYFLOW_VCPKG_FEED_USER) { $env:LYFLOW_VCPKG_FEED_USER } else { 'liangyuR' }),
    [string]$Repository = $(if ($env:GITHUB_REPOSITORY) { "https://github.com/$($env:GITHUB_REPOSITORY)" } else { 'https://github.com/liangyuR/lyFlow' }),
    [string]$ConfigDir
)
$ErrorActionPreference = 'Stop'

$token = $env:LYFLOW_VCPKG_FEED_TOKEN
if (-not $token) {
    Write-Host 'vcpkg 二进制源：未设置 LYFLOW_VCPKG_FEED_TOKEN，只用本地缓存'
    return
}
if ($Feed -notmatch '^https://') { throw "vcpkg 二进制源必须是 https：$Feed" }
$vcpkg = Join-Path $VcpkgRoot 'vcpkg.exe'
if (-not (Test-Path -LiteralPath $vcpkg -PathType Leaf)) { throw "找不到 vcpkg：$vcpkg" }
if (-not $ConfigDir) { $ConfigDir = Join-Path (Split-Path -Parent $PSScriptRoot) 'build/vcpkg-feed' }
New-Item -ItemType Directory -Path $ConfigDir -Force | Out-Null
$config = Join-Path ([IO.Path]::GetFullPath($ConfigDir)) 'nuget.config'

$nuget = (& $vcpkg fetch nuget | Select-Object -Last 1).Trim()
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $nuget -PathType Leaf)) { throw 'vcpkg fetch nuget 失败' }

# 每次重写：令牌可能换过，旧文件里的凭据不能留着。
Set-Content -LiteralPath $config -Encoding utf8NoBOM -Value @'
<?xml version="1.0" encoding="utf-8"?>
<configuration>
  <packageSources><clear /></packageSources>
</configuration>
'@
function Invoke-NuGet([string[]]$Arguments) {
    $output = & $nuget @Arguments -ConfigFile $config -NonInteractive 2>&1
    if ($LASTEXITCODE -ne 0) {
        # nuget 的报错里不会回显密码，但保险起见把令牌抹掉再抛。
        throw ("nuget $($Arguments[0]) 失败：" + (($output | Out-String) -replace [regex]::Escape($token), '***'))
    }
}
Invoke-NuGet @('sources', 'add', '-Name', 'lyflow-vcpkg', '-Source', $Feed, '-UserName', $User, '-Password', $token, '-StorePasswordInClearText')
if ($Mode -eq 'readwrite') {
    Invoke-NuGet @('setapikey', $token, '-Source', $Feed)
    Invoke-NuGet @('config', '-Set', "defaultPushSource=$Feed")
}

$sources = @('clear')
if ($env:VCPKG_DEFAULT_BINARY_CACHE) { $sources += "files,$($env:VCPKG_DEFAULT_BINARY_CACHE),readwrite" }
else { $sources += 'default,readwrite' }
$sources += "nugetconfig,$config,$Mode"
$sources += 'nugettimeout,1800'   # GitHub Packages 上传几百 MB 的包慢，默认 100 s 不够
$env:VCPKG_BINARY_SOURCES = $sources -join ';'
# 写进 nuspec 的 <repository>：GitHub Packages 靠它把包挂到仓库下，工作流的 GITHUB_TOKEN 才有权推送。
$env:VCPKG_NUGET_REPOSITORY = $Repository
Write-Host "vcpkg 二进制源：本地缓存 + $Feed（$Mode）"
