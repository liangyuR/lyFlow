#Requires -Version 7.0
param(
    [string]$BuildDir,
    [string]$InstallDir,
    [string]$ArchiveDir,
    [ValidateSet('Release', 'RelWithDebInfo')][string]$Configuration = 'RelWithDebInfo',
    [ValidateRange(1, 4)][int]$Parallel = 3,
    [switch]$Offline,
    [switch]$ReleaseBuild,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

function Resolve-BuildPath([string]$Value, [string]$Default) {
    if (-not $Value) { $Value = $Default }
    if (-not [IO.Path]::IsPathRooted($Value)) { $Value = Join-Path $root $Value }
    return [IO.Path]::GetFullPath($Value)
}

function Invoke-LoggedTool([string]$Executable, [string[]]$Arguments, [string]$Log, [hashtable]$Environment = @{}) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Executable
    $start.WorkingDirectory = $root
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
    foreach ($entry in $Environment.GetEnumerator()) { $start.Environment[$entry.Key] = $entry.Value }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    $writer = [IO.StreamWriter]::new($Log, $false, [Text.UTF8Encoding]::new($false))
    $writer.AutoFlush = $true
    try {
        $writer.WriteLine(($Executable, ($Arguments | ConvertTo-Json -Compress) -join ' '))
        if (-not $process.Start()) { throw "无法启动 $Executable" }
        $readers = @{ out = $process.StandardOutput; err = $process.StandardError }
        $pending = @{ out = $readers.out.ReadLineAsync(); err = $readers.err.ReadLineAsync() }
        while ($pending.Count) {
            $progressed = $false
            foreach ($stream in @($pending.Keys)) {
                if (-not $pending[$stream].IsCompleted) { continue }
                $line = $pending[$stream].GetAwaiter().GetResult()
                if ($null -eq $line) { $pending.Remove($stream) }
                else {
                    $writer.WriteLine($line)
                    Write-Host $line
                    $pending[$stream] = $readers[$stream].ReadLineAsync()
                }
                $progressed = $true
            }
            if (-not $progressed) { Start-Sleep -Milliseconds 20 }
        }
        $process.WaitForExit()
        if ($process.ExitCode -ne 0) { throw "$Executable 失败 (exit $($process.ExitCode))；日志：$Log" }
    } finally {
        $writer.Dispose()
        $process.Dispose()
    }
}

function Find-Executable([string]$Name, [string]$Preferred = '') {
    if ($Preferred -and (Test-Path -LiteralPath $Preferred -PathType Leaf)) { return $Preferred }
    $command = Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $command) { throw "找不到 $Name" }
    return $command.Source
}

function Get-WindowsTools {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { throw '找不到 Visual Studio vswhere.exe' }
    $instances = @((& $vswhere -all -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json) | ConvertFrom-Json)
    if ($LASTEXITCODE -ne 0) { throw 'Visual Studio 查询失败' }
    $toolchain = $null
    if ($ReleaseBuild) {
        $release = Get-Content -LiteralPath (Join-Path $root 'packaging/release.json') -Raw | ConvertFrom-Json
        $toolchain = $release.dependencyToolchain
        if ($toolchain.msvcToolset -notmatch '^\d+\.\d+\.\d+$' -or $toolchain.windowsSdk -notmatch '^\d+\.\d+\.\d+\.\d+$') {
            throw '发布配置缺少固定 MSVC / Windows SDK 版本'
        }
        $instances = @($instances | Where-Object {
            Test-Path -LiteralPath (Join-Path $_.installationPath "VC/Tools/MSVC/$($toolchain.msvcToolset)/bin/Hostx64/x64/cl.exe") -PathType Leaf
        })
    }
    $instance = $instances | Select-Object -First 1
    if (-not $instance) { throw '找不到满足工具集要求的 Visual Studio 安装' }
    $vsPath = $instance.installationPath
    $vcvars = Join-Path $vsPath 'VC/Auxiliary/Build/vcvars64.bat'
    if (-not (Test-Path -LiteralPath $vcvars -PathType Leaf) -or $vcvars -match '["%\r\n]') { throw '无效的 vcvars64.bat 路径' }
    $vcvarsArguments = if ($toolchain) { " $($toolchain.windowsSdk) -vcvars_ver=$($toolchain.msvcToolset)" } else { '' }
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $env:ComSpec
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.Arguments = "/d /v:off /s /c `"call `"$vcvars`"$vcvarsArguments >nul && set`""
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    try {
        if (-not $process.Start()) { throw '无法初始化 MSVC' }
        $output = $process.StandardOutput.ReadToEndAsync()
        $errors = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        if ($process.ExitCode -ne 0) { throw "MSVC 初始化失败 (exit $($process.ExitCode))" }
        $environment = @{}
        foreach ($line in ($output.GetAwaiter().GetResult() -split '\r?\n')) {
            if ($line -match '^([^=]+)=(.*)$') { $environment[$Matches[1]] = $Matches[2] }
        }
        $null = $errors.GetAwaiter().GetResult()
    } finally { $process.Dispose() }
    if (-not $environment.VCToolsInstallDir) { throw 'MSVC 环境缺少 VCToolsInstallDir' }
    if ($toolchain -and ($environment.VCToolsVersion.TrimEnd('\', '/') -ne $toolchain.msvcToolset -or
            $environment.WindowsSDKVersion.TrimEnd('\', '/') -ne $toolchain.windowsSdk)) {
        throw 'MSVC / Windows SDK 初始化版本不符合发布锁定'
    }
    $compiler = Join-Path $environment.VCToolsInstallDir 'bin/Hostx64/x64/cl.exe'
    return @{
        CMake = Find-Executable 'cmake' (Join-Path $vsPath 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe')
        Ninja = Find-Executable 'ninja' (Join-Path $vsPath 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe')
        Environment = $environment
        Compiler = $compiler
        Sdk = $environment.WindowsSDKVersion.TrimEnd('\', '/')
    }
}

function Read-CMakeCache([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "缺少实际CMake配置：$Path" }
    $values = @{}
    foreach ($line in [IO.File]::ReadAllLines($Path)) {
        if ($line -match '^([^#/:][^:]*):([^=]+)=(.*)$') { $values[$Matches[1]] = $Matches[3] }
    }
    return $values
}

function Get-ObservedProfile([hashtable]$Cache) {
    $profile = [ordered]@{}
    foreach ($key in @($Cache.Keys | Sort-Object)) {
        if ($key -match '^(WITH_|BUILD_|CPU_|OPENCV_|CMAKE_(CXX|C)_FLAGS($|_)|CMAKE_(EXE|SHARED|MODULE|STATIC)_LINKER_FLAGS($|_)|CMAKE_INTERPROCEDURAL_OPTIMIZATION($|_)|CMAKE_CXX_STANDARD($|_)|CMAKE_MSVC_RUNTIME_LIBRARY$|CMAKE_BUILD_TYPE$)') {
            if ($key -notmatch '(-ADVANCED|-STRINGS)$' -and $key -ne 'OPENCV_DOWNLOAD_PATH' -and $key -ne 'OPENCV_DOWNLOAD_PATH_DIR') {
                $profile[$key] = $Cache[$key]
            }
        }
    }
    return $profile
}

function Get-CompilerFlagsProfile {
    $profile = [ordered]@{}
    $profile.CMAKE_C_FLAGS = if ($IsWindows) { '/DWIN32 /D_WINDOWS /W3' } else { '' }
    $profile.CMAKE_CXX_FLAGS = if ($IsWindows) { '/DWIN32 /D_WINDOWS /W3 /GR /EHsc' } else { '' }
    $flags = if ($IsWindows) {
        if ($Configuration -eq 'Release') { '/O2 /Ob2 /DNDEBUG' } else { '/Zi /O2 /Ob1 /DNDEBUG' }
    } else {
        if ($Configuration -eq 'Release') { '-O3 -DNDEBUG' } else { '-O2 -g -DNDEBUG' }
    }
    $configurationName = $Configuration.ToUpperInvariant()
    $profile["CMAKE_C_FLAGS_$configurationName"] = $flags
    $profile["CMAKE_CXX_FLAGS_$configurationName"] = $flags
    foreach ($kind in @('EXE', 'SHARED', 'MODULE', 'STATIC')) {
        $profile["CMAKE_${kind}_LINKER_FLAGS"] = ''
        $profile["CMAKE_${kind}_LINKER_FLAGS_$configurationName"] = ''
    }
    $profile.CMAKE_INTERPROCEDURAL_OPTIMIZATION = 'OFF'
    $profile["CMAKE_INTERPROCEDURAL_OPTIMIZATION_$configurationName"] = 'OFF'
    return $profile
}

function Assert-Profile([System.Collections.IDictionary]$Actual, [System.Collections.IDictionary]$Expected, [switch]$Exact) {
    if ($Exact -and $Actual.Count -ne $Expected.Count) { throw 'OpenCV实际配置项集合与构建记录不符' }
    foreach ($key in $Expected.Keys) {
        if (-not $Actual.Contains($key) -or $Actual[$key] -cne $Expected[$key]) { throw "OpenCV实际配置不符：$key" }
    }
}

function Read-Producer([string]$CachePath, [string]$CompilerPath) {
    $cache = Read-CMakeCache $CachePath
    $compiler = Get-Content -LiteralPath $CompilerPath -Raw
    $values = @{}
    foreach ($name in @('CMAKE_CXX_COMPILER', 'CMAKE_CXX_COMPILER_ID', 'CMAKE_CXX_COMPILER_VERSION', 'CMAKE_CXX_SIZEOF_DATA_PTR')) {
        $value = [regex]::Match($compiler, "(?m)^set\($name\s+`"([^`"]*)`"\)")
        if (-not $value.Success) { throw "实际编译器记录缺少$name" }
        $values[$name] = $value.Groups[1].Value
    }
    if ($values.CMAKE_CXX_SIZEOF_DATA_PTR -ne '8') { throw 'OpenCV生产编译器必须为64位' }
    $cacheCompiler = $cache['CMAKE_CXX_COMPILER'].Replace('\', '/')
    if ($cacheCompiler -cne $values.CMAKE_CXX_COMPILER.Replace('\', '/')) { throw 'CMake缓存与编译器记录路径不一致' }
    $producer = [ordered]@{
        platform = if ($IsWindows) { 'windows' } else { 'linux' }
        architecture = 'x64'; pointerBytes = 8
        compiler = $values.CMAKE_CXX_COMPILER; compilerId = $values.CMAKE_CXX_COMPILER_ID
        compilerVersion = $values.CMAKE_CXX_COMPILER_VERSION
        cCompiler = $cache['CMAKE_C_COMPILER']
        toolset = ''; sdk = ''; runtime = if ($IsWindows) { 'MultiThreadedDLL' } else { 'system-shared' }
    }
    if ($IsWindows) {
        $match = [regex]::Match($cacheCompiler, '/MSVC/([0-9.]+)/bin/Hostx64/x64/cl\.exe$', 'IgnoreCase')
        if (-not $match.Success -or $producer.compilerId -cne 'MSVC' -or $cache['CMAKE_MSVC_RUNTIME_LIBRARY'] -cne 'MultiThreadedDLL' -or
            $producer.cCompiler.Replace('\', '/') -cne $cacheCompiler) { throw 'OpenCV必须使用同一x64 MSVC C/C++编译器和动态CRT' }
        $producer.toolset = $match.Groups[1].Value
        $producer.sdk = $cache['CMAKE_SYSTEM_VERSION']
        if ($producer.sdk -notmatch '^\d+\.\d+\.\d+\.\d+$') { throw 'OpenCV实际CMake配置缺少固定Windows SDK' }
    }
    return $producer
}

function Assert-Producer([System.Collections.IDictionary]$Producer) {
    if ($Producer.platform -cne $(if ($IsWindows) { 'windows' } else { 'linux' }) -or
        $Producer.architecture -cne 'x64' -or $Producer.pointerBytes -ne 8) { throw 'OpenCV生产平台不符合当前平台/x64' }
    if ($IsWindows -and $ReleaseBuild) {
        $release = Get-Content -LiteralPath (Join-Path $root 'packaging/release.json') -Raw | ConvertFrom-Json
        if ($Producer.toolset -cne $release.dependencyToolchain.msvcToolset -or
            $Producer.sdk -cne $release.dependencyToolchain.windowsSdk) { throw 'OpenCV生产toolset/SDK不符合发布锁定' }
    }
    if ($IsLinux) {
        if ($Producer.compiler -cne $tools.Compiler -or $Producer.cCompiler -cne $tools.CCompiler) { throw 'OpenCV缓存生产C/C++编译器路径与当前配置不同' }
        $banner = (& $tools.Compiler --version 2>&1 | Out-String).Trim()
        if ($LASTEXITCODE -ne 0 -or $Producer.banner -cne $banner) { throw 'OpenCV缓存生产编译器版本与当前编译器不同' }
        $cBanner = (& $tools.CCompiler --version 2>&1 | Out-String).Trim()
        if ($LASTEXITCODE -ne 0 -or $Producer.cBanner -cne $cBanner) { throw 'OpenCV缓存生产C编译器版本与当前编译器不同' }
        foreach ($entry in @(@{ key = 'target'; compiler = $tools.Compiler }, @{ key = 'cTarget'; compiler = $tools.CCompiler })) {
            $target = (& $entry.compiler -dumpmachine 2>&1 | Out-String).Trim()
            if ($LASTEXITCODE -ne 0 -or $target -notmatch '^(x86_64|amd64)(-|$)' -or $Producer[$entry.key] -cne $target) { throw 'OpenCV生产编译器目标必须为当前Linux x64' }
        }
    }
}

function Assert-ReleaseSource {
    $release = Get-Content -LiteralPath (Join-Path $root 'packaging/release.json') -Raw | ConvertFrom-Json
    if ($release.opencvVersion -cne '5.1.0' -or $release.opencvVersion -cne $opencvAsset.tag -or
        $release.opencvSourceRevision -cne $opencvAsset.revision) { throw '发布OpenCV版本/源码修订与固定视觉清单不一致' }
}

function Assert-InstallRecord {
    Assert-ReleaseSource
    $directory = Join-Path $install 'share/lyflow-opencv'
    $path = Join-Path $directory 'build-record.json'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw 'OpenCV安装缺少受管构建记录；必须先按正式脚本构建，不能跳过编译' }
    $recordText = Get-Content -LiteralPath $path -Raw
    $record = $recordText | ConvertFrom-Json -AsHashtable
    $document = [Text.Json.JsonDocument]::Parse($recordText)
    try {
        foreach ($entry in $document.RootElement.GetProperty('actualProfile').EnumerateObject()) {
            if ($entry.Value.ValueKind -ne [Text.Json.JsonValueKind]::String) { throw 'OpenCV配置记录值必须为字符串' }
            $record.actualProfile[$entry.Name] = $entry.Value.GetString()
        }
    } finally { $document.Dispose() }
    if ($record.schemaVersion -ne 1 -or $record.sourceVersion -cne $opencvAsset.tag -or $record.sourceRevision -cne $opencvAsset.revision -or
        $record.sourceArchiveSha256 -cne $opencvAsset.sha256 -or $record.configuration -cne $Configuration -or
        $record.cacheFile -notmatch '^cache-[a-f0-9]{32}\.txt$' -or $record.compilerFile -notmatch '^compiler-[a-f0-9]{32}\.cmake$') {
        throw 'OpenCV受管构建记录与固定源码/配置不符'
    }
    $cachePath = Join-Path $directory $record.cacheFile
    $compilerPath = Join-Path $directory $record.compilerFile
    $cache = Read-CMakeCache $cachePath
    $producer = Read-Producer $cachePath $compilerPath
    if ($IsLinux) {
        foreach ($field in @('banner', 'cBanner', 'target', 'cTarget')) { $producer[$field] = $record.producer[$field] }
    }
    Assert-Profile $producer $record.producer -Exact
    Assert-Producer $producer
    Assert-Profile $cache $expectedProfile
    Assert-Profile (Get-ObservedProfile $cache) $record.actualProfile -Exact
    if ((Test-Path -LiteralPath (Join-Path $build 'CMakeCache.txt') -PathType Leaf)) {
        Assert-Profile (Read-CMakeCache (Join-Path $build 'CMakeCache.txt')) $expectedProfile
    }
}

function Write-InstallRecord {
    $cachePath = Join-Path $build 'CMakeCache.txt'
    $cache = Read-CMakeCache $cachePath
    Assert-Profile $cache $expectedProfile
    if (-not ([IO.Path]::GetFullPath($cache['CMAKE_HOME_DIRECTORY'])).Equals([IO.Path]::GetFullPath($source), $comparison)) { throw '实际CMake源码目录不是固定OpenCV源码' }
    $files = @(Get-ChildItem -LiteralPath (Join-Path $build 'CMakeFiles') -Recurse -File -Filter CMakeCXXCompiler.cmake | Where-Object {
        [IO.Path]::GetRelativePath((Join-Path $build 'CMakeFiles'), $_.FullName) -match '^[^/\\]+[/\\]CMakeCXXCompiler\.cmake$'
    })
    if ($files.Count -ne 1) { throw '无法确定唯一的实际CMake生产编译器记录' }
    $producer = Read-Producer $cachePath $files[0].FullName
    if ($IsLinux) {
        $producer['banner'] = (& $tools.Compiler --version 2>&1 | Out-String).Trim()
        if ($LASTEXITCODE -ne 0) { throw '无法读取实际Linux编译器版本' }
        $producer['cBanner'] = (& $tools.CCompiler --version 2>&1 | Out-String).Trim()
        if ($LASTEXITCODE -ne 0) { throw '无法读取实际Linux C编译器版本' }
        $producer['target'] = (& $tools.Compiler -dumpmachine 2>&1 | Out-String).Trim()
        if ($LASTEXITCODE -ne 0) { throw '无法读取实际Linux C++编译器目标' }
        $producer['cTarget'] = (& $tools.CCompiler -dumpmachine 2>&1 | Out-String).Trim()
        if ($LASTEXITCODE -ne 0) { throw '无法读取实际Linux C编译器目标' }
    }
    Assert-Producer $producer
    $directory = Join-Path $install 'share/lyflow-opencv'
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    $suffix = [Guid]::NewGuid().ToString('N')
    $cacheName = "cache-$suffix.txt"; $compilerName = "compiler-$suffix.cmake"
    Copy-Item -LiteralPath $cachePath -Destination (Join-Path $directory $cacheName)
    Copy-Item -LiteralPath $files[0].FullName -Destination (Join-Path $directory $compilerName)
    $record = [ordered]@{
        schemaVersion = 1; sourceVersion = $opencvAsset.tag; sourceRevision = $opencvAsset.revision; sourceArchiveSha256 = $opencvAsset.sha256
        configuration = $Configuration; producer = $producer; actualProfile = Get-ObservedProfile $cache
        cacheFile = $cacheName; compilerFile = $compilerName
    }
    $temporary = Join-Path $directory "record-$suffix.tmp"
    $record | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $temporary -Encoding utf8NoBOM
    [IO.File]::Move($temporary, (Join-Path $directory 'build-record.json'), $true)
}

function Assert-InstalledOpenCV {
    $includeDir = if ($IsWindows) { 'include' } else { 'include/opencv5' }
    $versionHeader = Join-Path $install "$includeDir/opencv2/core/version.hpp"
    $text = Get-Content -LiteralPath $versionHeader -Raw
    foreach ($component in @(@('MAJOR', '5'), @('MINOR', '1'), @('REVISION', '0'))) {
        if ($text -notmatch "(?m)^#define\s+CV_VERSION_$($component[0])\s+$($component[1])\s*$") { throw '已安装 OpenCV 版本不是 5.1.0' }
    }
    if ($text -notmatch '(?m)^#define\s+CV_VERSION_STATUS\s+""\s*$') { throw '拒绝非稳定版 OpenCV' }
    $moduleHeader = Get-Content -LiteralPath (Join-Path $install "$includeDir/opencv2/opencv_modules.hpp") -Raw
    $configs = @(Get-ChildItem -LiteralPath $install -Recurse -File -Filter OpenCVModules.cmake)
    if ($configs.Count -ne 1) { throw '安装前缀必须含唯一 OpenCVModules.cmake' }
    $configDir = Split-Path -Parent $configs[0].FullName
    foreach ($name in @('OpenCVConfig.cmake', 'OpenCVConfig-version.cmake')) {
        if (-not (Test-Path -LiteralPath (Join-Path $configDir $name) -PathType Leaf)) { throw "安装缺少 $name" }
    }
    $exports = Get-Content -LiteralPath $configs[0].FullName -Raw
    $binaryDir = $null
    foreach ($module in @('core', 'features', 'flann', 'geometry', 'imgcodecs', 'imgproc', 'objdetect')) {
        if ($moduleHeader -notmatch "(?m)^#define\s+HAVE_OPENCV_$($module.ToUpperInvariant())\s*$" -or
            $exports -notmatch "add_library\(opencv_$module\s+SHARED\s+IMPORTED\)") { throw "安装缺少 OpenCV 模块 $module" }
        if ($IsWindows) {
            $binaryDir = Join-Path (Split-Path -Parent $configDir) 'bin'
            foreach ($file in @((Join-Path $configDir "opencv_${module}510.lib"), (Join-Path $binaryDir "opencv_${module}510.dll"))) {
                if (-not (Test-Path -LiteralPath $file -PathType Leaf) -or (Get-Item -LiteralPath $file).Length -eq 0) { throw "安装缺少非空文件 $file" }
            }
        } else {
            $libraries = @(Get-ChildItem -LiteralPath $install -Recurse -File -Filter "libopencv_${module}.so.5.1.0")
            if ($libraries.Count -ne 1 -or $libraries[0].Length -eq 0) { throw "安装缺少唯一共享库 libopencv_${module}.so.5.1.0" }
            $directory = Split-Path -Parent $libraries[0].FullName
            if ($binaryDir -and $binaryDir -ne $directory) { throw 'OpenCV 共享库不在同一目录' }
            $binaryDir = $directory
        }
    }
    $licenses = @{
        'LICENSE' = 'LICENSE'
        'etc/licenses/zlib-LICENSE' = '3rdparty/zlib/LICENSE'
        'etc/licenses/libpng-LICENSE' = '3rdparty/libpng/LICENSE'
        'etc/licenses/libpng-README' = '3rdparty/libpng/README'
        'etc/licenses/libjpeg-turbo-LICENSE.md' = '3rdparty/libjpeg-turbo/LICENSE.md'
        'etc/licenses/libjpeg-turbo-README.md' = '3rdparty/libjpeg-turbo/README.md'
        'etc/licenses/libjpeg-turbo-README.ijg' = '3rdparty/libjpeg-turbo/README.ijg'
        'etc/licenses/SoftFloat-COPYING.txt' = 'modules/core/3rdparty/SoftFloat/COPYING.txt'
        'etc/licenses/mscr-chi_table_LICENSE.txt' = 'modules/features/3rdparty/mscr/chi_table_LICENSE.txt'
        'etc/licenses/annoylib-LICENSE' = 'modules/features/3rdparty/annoy/LICENSE'
        'etc/licenses/dlpack-LICENSE' = '3rdparty/dlpack/LICENSE'
        'etc/licenses/flatbuffers-LICENSE.txt' = '3rdparty/flatbuffers/LICENSE.txt'
    }
    foreach ($entry in $licenses.GetEnumerator()) {
        $relative = if ($IsLinux) { $entry.Key -replace '^etc/licenses/', 'share/licenses/opencv5/' } else { $entry.Key }
        $installed = Join-Path $install $relative
        if (-not (Test-Path -LiteralPath $installed -PathType Leaf) -or
            (Get-FileHash -LiteralPath $installed -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath (Join-Path $source $entry.Value) -Algorithm SHA256).Hash) {
            throw "安装许可证缺失或不符合固定源码：$($entry.Key)"
        }
    }
    return @{ Config = $configDir; Bin = $binaryDir }
}

if (-not ($IsWindows -or $IsLinux) -or [Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture -ne 'X64') {
    throw 'OpenCV 固定 SSE2 构建支持 Windows / Linux x64'
}
$build = Resolve-BuildPath $BuildDir 'build/vision-opencv'
$install = Resolve-BuildPath $InstallDir 'build/opencv5-install'
$sourceRoot = Join-Path $root 'build/vision-deps'
$source = Join-Path $sourceRoot 'opencv'
$download = Join-Path $root 'build/vision-downloads'
$comparison = if ($IsWindows) { [StringComparison]::OrdinalIgnoreCase } else { [StringComparison]::Ordinal }
$sourcePrefix = $source.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
foreach ($path in @($build, $install, $download)) {
    if ($path.Equals($source, $comparison) -or $path.StartsWith($sourcePrefix, $comparison)) { throw '构建、安装、下载目录不得位于固定源码内' }
}
if ($build.Equals($install, $comparison)) { throw '构建和安装目录不得相同' }
New-Item -ItemType Directory -Path $build -Force | Out-Null
$pythonCommand = Get-Command python -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
$python = if ($pythonCommand) { $pythonCommand.Source } else { Find-Executable 'python3' }
$fetch = @((Join-Path $PSScriptRoot 'fetch-vision-deps.py'), '--output-dir', $sourceRoot)
if ($ArchiveDir) { $fetch += @('--archive-dir', (Resolve-BuildPath $ArchiveDir $ArchiveDir)) }
if ($Offline) { $fetch += '--offline' }
Invoke-LoggedTool $python $fetch (Join-Path $build 'fetch.log')

$visionConfig = Get-Content -LiteralPath (Join-Path $root 'packaging/vision-deps.json') -Raw | ConvertFrom-Json
$opencvAssets = @($visionConfig.assets | Where-Object name -EQ 'opencv')
if ($opencvAssets.Count -ne 1) { throw '视觉清单必须固定唯一OpenCV源码' }
$opencvAsset = $opencvAssets[0]
Assert-ReleaseSource
if ($SkipBuild -and -not (Test-Path -LiteralPath (Join-Path $install 'share/lyflow-opencv/build-record.json') -PathType Leaf)) {
    throw 'OpenCV安装缺少受管构建记录；必须先构建，不能跳过编译'
}
if ($IsWindows -and $SkipBuild -and $ReleaseBuild) {
    $release = Get-Content -LiteralPath (Join-Path $root 'packaging/release.json') -Raw | ConvertFrom-Json
    $tools = @{ Compiler = ''; Ninja = ''; Sdk = $release.dependencyToolchain.windowsSdk; Environment = @{} }
} elseif ($IsWindows) { $tools = Get-WindowsTools }
else {
    $cc = Find-Executable $(if ($env:CC) { $env:CC } else { 'cc' })
    $cxx = Find-Executable $(if ($env:CXX) { $env:CXX } else { 'c++' })
    $tools = @{ CMake = Find-Executable 'cmake'; Ninja = Find-Executable 'ninja'; Environment = @{}; Compiler = $cxx; CCompiler = $cc }
}
$configure = @('-S', $source, '-B', $build, '-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$($tools.Ninja)",
    "-DCMAKE_BUILD_TYPE=$Configuration", '-DCMAKE_CXX_STANDARD=17', '-DCMAKE_CXX_STANDARD_REQUIRED=ON',
    "-DCMAKE_INSTALL_PREFIX=$install", '-DCMAKE_TOOLCHAIN_FILE=', '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF',
    '-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF', '-DCMAKE_DISABLE_FIND_PACKAGE_Git=ON', "-DOPENCV_DOWNLOAD_PATH=$download",
    '-DBUILD_SHARED_LIBS=ON', '-DBUILD_LIST=core,imgproc,imgcodecs,geometry,objdetect',
    '-DCPU_BASELINE=SSE2', '-DCPU_BASELINE_REQUIRE=SSE2', '-DCPU_DISPATCH=',
    '-DBUILD_TESTS=OFF', '-DBUILD_PERF_TESTS=OFF', '-DBUILD_EXAMPLES=OFF', '-DBUILD_opencv_apps=OFF',
    '-DBUILD_opencv_python2=OFF', '-DBUILD_opencv_python3=OFF', '-DBUILD_opencv_java=OFF', '-DBUILD_opencv_js=OFF',
    '-DBUILD_JAVA=OFF', '-DBUILD_DOCS=OFF', '-DBUILD_PACKAGE=OFF', '-DBUILD_opencv_world=OFF',
    '-DBUILD_opencv_highgui=OFF', '-DBUILD_opencv_dnn=OFF', '-DBUILD_opencv_videoio=OFF',
    '-DBUILD_opencv_calib=OFF', '-DBUILD_opencv_stereo=OFF', '-DOPENCV_EXTRA_MODULES_PATH=',
    '-DWITH_IPP=OFF', '-DWITH_ITT=OFF', '-DWITH_OPENCL=OFF', '-DWITH_CUDA=OFF', '-DWITH_CUBLAS=OFF',
    '-DWITH_CUDNN=OFF', '-DWITH_TBB=OFF', '-DBUILD_TBB=OFF', '-DWITH_OPENMP=OFF',
    '-DWITH_OPENVINO=OFF', '-DWITH_EIGEN=OFF', '-DWITH_LAPACK=OFF', '-DWITH_PROTOBUF=OFF',
    '-DWITH_HARFBUZZ=OFF', '-DWITH_UNIFONT=OFF', '-DWITH_ITALICFONT=OFF',
    '-DWITH_FFMPEG=OFF', '-DWITH_GSTREAMER=OFF', '-DWITH_MSMF=OFF', '-DWITH_DSHOW=OFF',
    '-DWITH_WIN32UI=OFF', '-DWITH_QT=OFF', '-DWITH_GTK=OFF', '-DWITH_VTK=OFF',
    '-DWITH_JPEG=ON', '-DBUILD_JPEG=ON', '-DWITH_PNG=ON', '-DBUILD_PNG=ON', '-DBUILD_ZLIB=ON',
    '-DWITH_ZLIB_NG=OFF', '-DWITH_TIFF=OFF', '-DWITH_WEBP=OFF', '-DWITH_OPENJPEG=OFF',
    '-DWITH_OPENEXR=OFF', '-DWITH_AVIF=OFF', '-DWITH_SPNG=OFF', '-DWITH_JASPER=OFF',
    '-DWITH_QUIRC=OFF', '-DWITH_OBSENSOR=OFF', '-DWITH_D3D11=OFF', '-DWITH_VULKAN=OFF',
    '-DOPENCV_ENABLE_NONFREE=OFF', '-DOPENCV_GENERATE_PKGCONFIG=OFF', '-DBUILD_WITH_STATIC_CRT=OFF')
if ($IsWindows) {
    $configure += @('-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL', "-DCMAKE_C_COMPILER=$($tools.Compiler)", "-DCMAKE_CXX_COMPILER=$($tools.Compiler)", "-DCMAKE_SYSTEM_VERSION=$($tools.Sdk)")
} else {
    $configure += @("-DCMAKE_C_COMPILER=$($tools.CCompiler)", "-DCMAKE_CXX_COMPILER=$($tools.Compiler)")
}
$compilerFlagsProfile = Get-CompilerFlagsProfile
foreach ($key in $compilerFlagsProfile.Keys) { $configure += "-D$key=$($compilerFlagsProfile[$key])" }
$expectedProfile = [ordered]@{}
foreach ($argument in $configure) {
    if ($argument -match '^-D([^=]+)=(.*)$' -and $Matches[1] -notin @('CMAKE_MAKE_PROGRAM', 'CMAKE_INSTALL_PREFIX', 'CMAKE_C_COMPILER', 'CMAKE_CXX_COMPILER', 'OPENCV_DOWNLOAD_PATH')) {
        $expectedProfile[$Matches[1]] = $Matches[2]
    }
}
if (-not $SkipBuild) {
    foreach ($name in @('CL', '_CL_', 'CFLAGS', 'CXXFLAGS', 'LDFLAGS')) {
        if ([Environment]::GetEnvironmentVariable($name, 'Process')) { throw "固定OpenCV构建拒绝环境编译选项：$name" }
    }
    New-Item -ItemType Directory -Path $download -Force | Out-Null
    if ($IsWindows -and $ReleaseBuild -and (Test-Path -LiteralPath (Join-Path $build 'CMakeCache.txt') -PathType Leaf)) {
        $old = Read-CMakeCache (Join-Path $build 'CMakeCache.txt')
        if ($old['CMAKE_CXX_COMPILER'] -and -not ([IO.Path]::GetFullPath($old['CMAKE_CXX_COMPILER'])).Equals([IO.Path]::GetFullPath($tools.Compiler), $comparison)) {
            throw '现有缓存编译器不符合发布锁定；请指定独立BuildDir'
        }
    }
    Invoke-LoggedTool $tools.CMake $configure (Join-Path $build 'configure.log') $tools.Environment
    Invoke-LoggedTool $tools.CMake @('--build', $build, '--parallel', "$Parallel") (Join-Path $build 'build.log') $tools.Environment
    Invoke-LoggedTool $tools.CMake @('--install', $build) (Join-Path $build 'install.log') $tools.Environment
    if ($IsLinux) { Copy-Item -LiteralPath (Join-Path $source 'LICENSE') -Destination (Join-Path $install 'LICENSE') -Force }
}
$installed = Assert-InstalledOpenCV
Invoke-LoggedTool $python ($fetch + '--verify-only') (Join-Path $build 'verify-source.log')
if (-not $SkipBuild) { Write-InstallRecord }
Assert-InstallRecord
$env:OpenCV_DIR = $installed.Config
$env:LYFLOW_OPENCV_ROOT = $install
$env:LYFLOW_OPENCV_BIN = $installed.Bin
$operation = if ($SkipBuild) { '已验证受管安装（跳过编译）' } else { '构建、安装及验证完成' }
Write-Host "OpenCV 5.1.0 $operation -> $install"
Write-Host "OpenCV_DIR=$env:OpenCV_DIR"
Write-Host "LYFLOW_OPENCV_BIN=$env:LYFLOW_OPENCV_BIN"
