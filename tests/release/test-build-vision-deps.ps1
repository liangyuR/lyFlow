#Requires -Version 7.0
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Split-Path -Parent (Split-Path -Parent $PSScriptRoot)))
$work = Join-Path $repository ('build/vision-profile-test-' + [Guid]::NewGuid().ToString('N'))
$parseErrors = $null; $tokens = $null
$ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $repository 'scripts/build-vision-deps.ps1'), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw 'Production build script does not parse.' }
foreach ($function in $ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] }, $false)) {
    . ([ScriptBlock]::Create($function.Extent.Text))
}
$root = $work
$source = Join-Path $root 'build/vision-deps/opencv'
$build = Join-Path $root 'build/producer'
$install = Join-Path $root 'build/install'
$Configuration = 'RelWithDebInfo'
$ReleaseBuild = $true
$comparison = if ($IsWindows) { [StringComparison]::OrdinalIgnoreCase } else { [StringComparison]::Ordinal }
$count = 0
function Put-File([string]$Path, [string]$Text) {
    New-Item -ItemType Directory -Path (Split-Path -Parent $Path) -Force | Out-Null
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}
function Reject([ScriptBlock]$Action, [string]$Name) {
    $failed = $false
    try { & $Action } catch { $failed = $true }
    if (-not $failed) { throw "Expected rejection: $Name" }
    $script:count++
}
try {
    $release = Get-Content -LiteralPath (Join-Path $repository 'packaging/release.json') -Raw
    Put-File (Join-Path $root 'packaging/release.json') $release
    $vision = Get-Content -LiteralPath (Join-Path $repository 'packaging/vision-deps.json') -Raw | ConvertFrom-Json
    $opencvAsset = @($vision.assets | Where-Object name -EQ 'opencv')[0]
    $expectedProfile = [ordered]@{
        CMAKE_BUILD_TYPE = $Configuration; CMAKE_CXX_STANDARD = '17'; CMAKE_CXX_STANDARD_REQUIRED = 'ON'
        BUILD_SHARED_LIBS = 'ON'; CPU_BASELINE = 'SSE2'; CPU_BASELINE_REQUIRE = 'SSE2'; CPU_DISPATCH = ''
        WITH_IPP = 'OFF'; WITH_OPENCL = 'OFF'; WITH_CUDA = 'OFF'; WITH_TBB = 'OFF'; WITH_OPENMP = 'OFF'
        OPENCV_ENABLE_NONFREE = 'OFF'
    }
    if ($IsWindows) {
        $releaseSettings = $release | ConvertFrom-Json
        $toolset = $releaseSettings.dependencyToolchain.msvcToolset
        $sdk = $releaseSettings.dependencyToolchain.windowsSdk
        $compiler = "C:/fixture/VC/Tools/MSVC/$toolset/bin/Hostx64/x64/cl.exe"
        $compilerId = 'MSVC'; $compilerVersion = '19.44.35207.0'
        $expectedProfile.CMAKE_MSVC_RUNTIME_LIBRARY = 'MultiThreadedDLL'
        $expectedProfile.CMAKE_SYSTEM_VERSION = $sdk
        $tools = @{ Compiler = $compiler; CCompiler = $compiler }
        $configDir = Join-Path $install 'x64/vc17/lib'
        $binDir = Join-Path $install 'x64/vc17/bin'
        $includeDir = Join-Path $install 'include'
    } else {
        $compiler = (Get-Command c++ -CommandType Application | Select-Object -First 1).Source
        $compilerId = 'GNU'; $compilerVersion = '13.3.0'
        $tools = @{ Compiler = $compiler; CCompiler = (Get-Command cc -CommandType Application | Select-Object -First 1).Source }
        $configDir = Join-Path $install 'lib/cmake/opencv5'
        $binDir = Join-Path $install 'lib'
        $includeDir = Join-Path $install 'include/opencv5'
    }
    $cache = [ordered]@{} + $expectedProfile
    foreach ($entry in (Get-CompilerFlagsProfile).GetEnumerator()) {
        $expectedProfile[$entry.Key] = $entry.Value
        $cache[$entry.Key] = $entry.Value
    }
    $cache.CMAKE_HOME_DIRECTORY = $source.Replace('\', '/')
    $cache.CMAKE_INSTALL_PREFIX = $install.Replace('\', '/')
    $cache.CMAKE_CXX_COMPILER = $compiler
    $cache.CMAKE_C_COMPILER = $tools.CCompiler
    $cache.OPENCV_TIMESTAMP = '2026-11-15T00:00:00Z'
    $cacheFile = Join-Path $build 'CMakeCache.txt'
    $compilerFile = Join-Path $build 'CMakeFiles/fixture/CMakeCXXCompiler.cmake'
    function Write-ProducerFixture {
        $lines = foreach ($key in $cache.Keys) { "$key`:STRING=$($cache[$key])" }
        Put-File $cacheFile (($lines -join "`n") + "`n")
        Put-File $compilerFile @"
set(CMAKE_CXX_COMPILER "$compiler")
set(CMAKE_CXX_COMPILER_ID "$compilerId")
set(CMAKE_CXX_COMPILER_VERSION "$compilerVersion")
set(CMAKE_CXX_SIZEOF_DATA_PTR "8")
"@
    }
    Write-ProducerFixture
    Put-File (Join-Path $includeDir 'opencv2/core/version.hpp') @'
#define CV_VERSION_MAJOR 5
#define CV_VERSION_MINOR 1
#define CV_VERSION_REVISION 0
#define CV_VERSION_STATUS ""
'@
    $modules = 'core', 'features', 'flann', 'geometry', 'imgcodecs', 'imgproc', 'objdetect'
    Put-File (Join-Path $includeDir 'opencv2/opencv_modules.hpp') (($modules | ForEach-Object { "#define HAVE_OPENCV_$($_.ToUpperInvariant())" }) -join "`n")
    Put-File (Join-Path $configDir 'OpenCVModules.cmake') (($modules | ForEach-Object { "add_library(opencv_$_ SHARED IMPORTED)" }) -join "`n")
    Put-File (Join-Path $configDir 'OpenCVConfig.cmake') 'fixture'
    Put-File (Join-Path $configDir 'OpenCVConfig-version.cmake') 'fixture'
    foreach ($module in $modules) {
        if ($IsWindows) {
            Put-File (Join-Path $configDir "opencv_${module}510.lib") 'fixture, not an executable library'
            Put-File (Join-Path $binDir "opencv_${module}510.dll") 'fixture, not an executable library'
        } else { Put-File (Join-Path $binDir "libopencv_${module}.so.5.1.0") 'fixture, not an executable library' }
    }
    $licenses = @{
        'LICENSE' = 'LICENSE'; 'zlib-LICENSE' = '3rdparty/zlib/LICENSE'; 'libpng-LICENSE' = '3rdparty/libpng/LICENSE'
        'libpng-README' = '3rdparty/libpng/README'; 'libjpeg-turbo-LICENSE.md' = '3rdparty/libjpeg-turbo/LICENSE.md'
        'libjpeg-turbo-README.md' = '3rdparty/libjpeg-turbo/README.md'; 'libjpeg-turbo-README.ijg' = '3rdparty/libjpeg-turbo/README.ijg'
        'SoftFloat-COPYING.txt' = 'modules/core/3rdparty/SoftFloat/COPYING.txt'
        'mscr-chi_table_LICENSE.txt' = 'modules/features/3rdparty/mscr/chi_table_LICENSE.txt'
        'annoylib-LICENSE' = 'modules/features/3rdparty/annoy/LICENSE'; 'dlpack-LICENSE' = '3rdparty/dlpack/LICENSE'
        'flatbuffers-LICENSE.txt' = '3rdparty/flatbuffers/LICENSE.txt'
    }
    foreach ($entry in $licenses.GetEnumerator()) {
        Put-File (Join-Path $source $entry.Value) "notice fixture $($entry.Key)"
        $target = if ($entry.Key -eq 'LICENSE') { Join-Path $install 'LICENSE' }
                  elseif ($IsWindows) { Join-Path $install "etc/licenses/$($entry.Key)" }
                  else { Join-Path $install "share/licenses/opencv5/$($entry.Key)" }
        Put-File $target "notice fixture $($entry.Key)"
    }
    Write-InstallRecord
    Assert-InstallRecord
    $null = Assert-InstalledOpenCV
    $count++
    $recordPath = Join-Path $install 'share/lyflow-opencv/build-record.json'
    $original = Get-Content -LiteralPath $recordPath -Raw
    Put-File $recordPath (($original | ConvertFrom-Json -AsHashtable) | ConvertTo-Json -Depth 8)
    Assert-InstallRecord
    $count++
    Put-File $recordPath $original
    function Change-Record([ScriptBlock]$Change, [string]$Name) {
        $record = $original | ConvertFrom-Json -AsHashtable
        & $Change $record
        Put-File $recordPath ($record | ConvertTo-Json -Depth 8)
        try { Reject { Assert-InstallRecord } $Name } finally { Put-File $recordPath $original }
    }
    $backup = "$recordPath.backup"
    Move-Item -LiteralPath $recordPath -Destination $backup
    try { Reject { Assert-InstallRecord } 'missing installation record' } finally { Move-Item -LiteralPath $backup -Destination $recordPath }
    Change-Record { param($record) $record.configuration = 'Release' } 'different build configuration'
    Change-Record { param($record) $record.sourceRevision = '0' * 40 } 'different source revision'
    Change-Record { param($record) $record.sourceArchiveSha256 = '0' * 64 } 'different source archive'
    Change-Record { param($record) $record.cacheFile = '../outside.txt' } 'unsafe snapshot path'
    Change-Record { param($record) $record.actualProfile.CPU_BASELINE = 'AVX2' } 'record profile differs from actual cache'
    Change-Record { param($record) $record.producer.compilerVersion = '0.0.0' } 'producer differs from actual compiler snapshot'
    $snapshot = $original | ConvertFrom-Json
    $snapshotCache = Join-Path $install "share/lyflow-opencv/$($snapshot.cacheFile)"
    foreach ($key in @('CMAKE_CXX_FLAGS', 'CMAKE_C_FLAGS_RELWITHDEBINFO', 'CMAKE_SHARED_LINKER_FLAGS', 'CMAKE_INTERPROCEDURAL_OPTIMIZATION')) {
        $savedValue = $cache[$key]
        $cache[$key] = if ($key -eq 'CMAKE_INTERPROCEDURAL_OPTIMIZATION') { 'ON' } elseif ($IsWindows) { '/arch:AVX2 /GL /MT /LTCG' } else { '-march=native -flto -fsanitize=address' }
        Write-ProducerFixture
        try { Reject { Write-InstallRecord } "hidden producer override: $key" } finally { $cache[$key] = $savedValue; Write-ProducerFixture }
    }
    $saved = Get-Content -LiteralPath $snapshotCache -Raw
    Put-File $snapshotCache ($saved.Replace('WITH_IPP:STRING=OFF', 'WITH_IPP:STRING=ON'))
    try { Reject { Assert-InstallRecord } 'mutated actual CMake profile' } finally { Put-File $snapshotCache $saved }
    $wrongRelease = $release | ConvertFrom-Json -AsHashtable
    $wrongRelease.opencvSourceRevision = '0' * 40
    Put-File (Join-Path $root 'packaging/release.json') ($wrongRelease | ConvertTo-Json -Depth 20)
    try { Reject { Assert-InstallRecord } 'release and vision source disagree' } finally { Put-File (Join-Path $root 'packaging/release.json') $release }
    $header = Join-Path $includeDir 'opencv2/core/version.hpp'
    $saved = Get-Content -LiteralPath $header -Raw
    Put-File $header ($saved.Replace('CV_VERSION_MINOR 1', 'CV_VERSION_MINOR 0'))
    try { Reject { Assert-InstalledOpenCV } 'installed version header disagrees' } finally { Put-File $header $saved }
    $notice = if ($IsWindows) { Join-Path $install 'etc/licenses/zlib-LICENSE' } else { Join-Path $install 'share/licenses/opencv5/zlib-LICENSE' }
    $saved = Get-Content -LiteralPath $notice -Raw
    Put-File $notice 'wrong notice'
    try { Reject { Assert-InstalledOpenCV } 'bundled notice differs from fixed source' } finally { Put-File $notice $saved }
    if ($IsWindows) {
        $ReleaseBuild = $false
        $compiler = $compiler.Replace($toolset, '14.51.36231'); $compilerVersion = '19.51.36260.0'
        $cache.CMAKE_CXX_COMPILER = $compiler
        $cache.CMAKE_C_COMPILER = $compiler
        Write-ProducerFixture
        Write-InstallRecord
        Assert-InstallRecord
        $count++
        $ReleaseBuild = $true
        Reject { Assert-InstallRecord } '14.51 producer cannot satisfy fixed 14.44 release'
    }
    Write-Host "OpenCV managed installation contract: $count scenarios passed ($([Runtime.InteropServices.RuntimeInformation]::OSDescription))."
} finally {
    $prefix = [IO.Path]::GetFullPath((Join-Path $repository 'build')).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    $target = [IO.Path]::GetFullPath($work)
    if (-not $target.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Refusing to remove fixture outside repository build directory.' }
    if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Recurse -Force }
}
