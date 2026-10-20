[CmdletBinding()]
param(
    [string]$WorkspaceRoot = "",
    [string]$RepoRoot = "",
    [string]$FexSource = "",
    [string]$AdapterSource = "",
    [string]$AndroidSdkRoot = "",
    [string]$NdkVersion = "29.0.14206865",
    [string]$CmakeVersion = "3.22.1",
    [int]$AndroidApi = 26,
    [string]$ExpectedFexCommit = "3ab2f24c7a5371dcef2f950b3342a3c37b0d1627",
    [int]$Jobs = [Environment]::ProcessorCount,
    [switch]$VerifyExisting,
    [switch]$DiagnosticSkipFreshness
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$Program,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code $LASTEXITCODE`: $Program $($Arguments -join ' ')"
    }
}

function Resolve-ExistingPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return (Resolve-Path -LiteralPath $Path).Path
}

function Assert-SafeBuildPath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Workspace,
        [Parameter(Mandatory = $true)][string]$ExpectedLeaf
    )
    $absolute = [IO.Path]::GetFullPath($Path)
    $workspaceAbsolute = [IO.Path]::GetFullPath($Workspace).TrimEnd([IO.Path]::DirectorySeparatorChar)
    $workspacePrefix = $workspaceAbsolute + [IO.Path]::DirectorySeparatorChar
    if (-not $absolute.StartsWith($workspacePrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove a build path outside the workspace: $absolute"
    }
    if ([IO.Path]::GetFileName($absolute) -ne $ExpectedLeaf) {
        throw "Refusing to remove unexpected build path leaf: $absolute"
    }
    return $absolute
}

function Assert-CMakeExecutableTarget {
    param(
        [Parameter(Mandatory = $true)][string]$CmakeListsPath,
        [Parameter(Mandatory = $true)][string]$Target
    )
    $cmakeContents = [IO.File]::ReadAllText($CmakeListsPath)
    $escapedTarget = [Text.RegularExpressions.Regex]::Escape($Target)
    $targetPattern = "(?m)^\s*add_executable\(\s*$escapedTarget(?:\s|\))"
    if (-not [Text.RegularExpressions.Regex]::IsMatch($cmakeContents, $targetPattern)) {
        throw "Required Android smoke target '$Target' is not declared in $CmakeListsPath"
    }
}

if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = Resolve-ExistingPath (Join-Path $PSScriptRoot "..\..")
} else {
    $RepoRoot = Resolve-ExistingPath $RepoRoot
}
if ([string]::IsNullOrWhiteSpace($WorkspaceRoot)) {
    $WorkspaceRoot = Resolve-ExistingPath (Join-Path $RepoRoot "..")
} else {
    $WorkspaceRoot = Resolve-ExistingPath $WorkspaceRoot
}

if ([string]::IsNullOrWhiteSpace($AndroidSdkRoot)) {
    if (-not [string]::IsNullOrWhiteSpace($env:ANDROID_SDK_ROOT)) {
        $AndroidSdkRoot = $env:ANDROID_SDK_ROOT
    } elseif (-not [string]::IsNullOrWhiteSpace($env:ANDROID_HOME)) {
        $AndroidSdkRoot = $env:ANDROID_HOME
    } else {
        $AndroidSdkRoot = Join-Path $env:LOCALAPPDATA "Android\Sdk"
    }
}
$AndroidSdkRoot = Resolve-ExistingPath $AndroidSdkRoot

$ndkRoot = Resolve-ExistingPath (Join-Path $AndroidSdkRoot "ndk\$NdkVersion")
$cmakeBin = Resolve-ExistingPath (Join-Path $AndroidSdkRoot "cmake\$CmakeVersion\bin\cmake.exe")
$ctestBin = Resolve-ExistingPath (Join-Path $AndroidSdkRoot "cmake\$CmakeVersion\bin\ctest.exe")
$ninjaBin = Resolve-ExistingPath (Join-Path $AndroidSdkRoot "cmake\$CmakeVersion\bin\ninja.exe")
$toolchainFile = Resolve-ExistingPath (Join-Path $ndkRoot "build\cmake\android.toolchain.cmake")
$libcxxShared = Resolve-ExistingPath (Join-Path $ndkRoot "toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\lib\aarch64-linux-android\libc++_shared.so")
$pythonBin = (Get-Command python -ErrorAction Stop).Source

if ([string]::IsNullOrWhiteSpace($FexSource)) {
    $FexSource = Join-Path $WorkspaceRoot "fex-emu-cpu-hacks-8gen5-wt"
}
if ([string]::IsNullOrWhiteSpace($AdapterSource)) {
    $AdapterSource = Join-Path $WorkspaceRoot "fex-adapter-cpu-hacks-8gen5-wt"
}
$fexSource = Resolve-ExistingPath $FexSource
$adapterSource = Resolve-ExistingPath $AdapterSource
$adapterCmakeLists = Resolve-ExistingPath (Join-Path $adapterSource "CMakeLists.txt")
$androidSmokeTargets = @(
    "lsx4_fex2608_runtime_smoke",
    "lsx4_fex2608_staged_jit_smoke",
    "lsx4_fex2608_staged_cpu_profile_probe",
    "lsx4_typed_hle_abi_smoke"
)
foreach ($androidSmokeTarget in $androidSmokeTargets) {
    Assert-CMakeExecutableTarget `
        -CmakeListsPath $adapterCmakeLists `
        -Target $androidSmokeTarget
}
$abiDirectory = Resolve-ExistingPath (Join-Path $RepoRoot "src\executor")
$abiHeader = Resolve-ExistingPath (Join-Path $abiDirectory "fex_backend_abi_v3.h")
$funnelSource = Resolve-ExistingPath (Join-Path $RepoRoot "funnel-arm\src")
$typedHleAbiHeader = Resolve-ExistingPath (Join-Path $funnelSource "core\aerolib\hle_abi.h")
$verifier = Resolve-ExistingPath (Join-Path $PSScriptRoot "verify_android_fex_artifacts.py")

$fexBuild = Assert-SafeBuildPath `
    -Path (Join-Path $WorkspaceRoot ".fex-2608-build\android-api26-arm64") `
    -Workspace $WorkspaceRoot `
    -ExpectedLeaf "android-api26-arm64"
$adapterBuild = Assert-SafeBuildPath `
    -Path (Join-Path $WorkspaceRoot ".fex-2608-adapter-v3-build\android-api26-arm64-clean") `
    -Workspace $WorkspaceRoot `
    -ExpectedLeaf "android-api26-arm64-clean"
$hostBuild = Assert-SafeBuildPath `
    -Path (Join-Path $WorkspaceRoot ".fex-2608-adapter-v3-build\host-tests-clean") `
    -Workspace $WorkspaceRoot `
    -ExpectedLeaf "host-tests-clean"
$manifestPath = Join-Path $WorkspaceRoot ".fex-2608-artifacts\android-api26-arm64\manifest.json"
$sourceSnapshotPath = Join-Path $WorkspaceRoot ".fex-2608-artifacts\android-api26-arm64\build-source-snapshot.json"
$sourceArguments = @(
    "--source", "fex=$fexSource=git",
    "--source", "adapter=$adapterSource=tree",
    "--source", "lsx4_fex_abi=$abiHeader=git",
    "--source", "lsx4_typed_hle_abi=$typedHleAbiHeader=git"
)

$actualFexCommit = (& git -C $fexSource rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) {
    throw "Unable to read the pinned FEX source revision"
}
if ($actualFexCommit -ne $ExpectedFexCommit) {
    throw "Pinned FEX revision mismatch: expected $ExpectedFexCommit, got $actualFexCommit"
}

$requiredFexSubmoduleFiles = @{
    "External/fmt" = "CMakeLists.txt"
    "External/jemalloc_glibc" = "CMakeLists.txt"
    "External/range-v3" = "CMakeLists.txt"
    "External/rpmalloc" = "CMakeLists.txt"
    "External/unordered_dense" = "CMakeLists.txt"
    "External/xxhash" = "xxhash.c"
}
foreach ($requiredFexSubmodule in $requiredFexSubmoduleFiles.Keys) {
    $submoduleProof = Join-Path $fexSource (
        "$requiredFexSubmodule/" + $requiredFexSubmoduleFiles[$requiredFexSubmodule])
    if (-not (Test-Path -LiteralPath $submoduleProof -PathType Leaf)) {
        throw "Pinned FEX submodule is not initialized: $requiredFexSubmodule"
    }
}

if (-not $VerifyExisting) {
    $snapshotArguments = @($verifier, "--write-source-snapshot", $sourceSnapshotPath) + $sourceArguments
    Invoke-Checked -Program $pythonBin -Arguments $snapshotArguments

    foreach ($buildPath in @($fexBuild, $adapterBuild, $hostBuild)) {
        if (Test-Path -LiteralPath $buildPath) {
            Remove-Item -LiteralPath $buildPath -Recurse -Force
        }
        New-Item -ItemType Directory -Path $buildPath -Force | Out-Null
    }

    Invoke-Checked -Program $cmakeBin -Arguments @(
        "-S", $fexSource,
        "-B", $fexBuild,
        "-G", "Ninja",
        "-DCMAKE_MAKE_PROGRAM=$ninjaBin",
        "-DCMAKE_TOOLCHAIN_FILE=$toolchainFile",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DANDROID_ABI=arm64-v8a",
        "-DANDROID_PLATFORM=android-$AndroidApi",
        "-DANDROID_STL=c++_shared",
        "-DFEXCORE_ONLY=ON",
        "-DTUNE_CPU=none",
        "-DTUNE_ARCH=generic",
        "-DENABLE_JEMALLOC_GLIBC_ALLOC=OFF",
        "-DBUILD_TESTING=OFF",
        "-DBUILD_FEXCONFIG=OFF",
        "-DBUILD_FEX_LINUX_TESTS=OFF",
        "-DBUILD_STEAM_SUPPORT=OFF",
        "-DBUILD_THUNKS=OFF",
        "-DENABLE_CCACHE=OFF",
        "-DENABLE_FEXCORE_PROFILER=OFF",
        "-DENABLE_GDB_SYMBOLS=OFF",
        "-DENABLE_LTO=OFF",
        "-DENABLE_STRICT_WERROR=OFF"
    )
    Invoke-Checked -Program $cmakeBin -Arguments @(
        "--build", $fexBuild,
        "--target", "libFEXCore.so",
        "--parallel", "$Jobs"
    )

    $fexCore = Resolve-ExistingPath (Join-Path $fexBuild "FEXCore\Source\libFEXCore.so")
    Invoke-Checked -Program $cmakeBin -Arguments @(
        "-S", $adapterSource,
        "-B", $adapterBuild,
        "-G", "Ninja",
        "-DCMAKE_MAKE_PROGRAM=$ninjaBin",
        "-DCMAKE_TOOLCHAIN_FILE=$toolchainFile",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DANDROID_ABI=arm64-v8a",
        "-DANDROID_PLATFORM=android-$AndroidApi",
        "-DANDROID_STL=c++_shared",
        "-DLSX4_FEX_ABI_DIR=$abiDirectory",
        "-DLSX4_FEX2608_SOURCE_DIR=$fexSource",
        "-DLSX4_FEX2608_LIBRARY=$fexCore",
        "-DLSX4_FUNNEL_SRC_DIR=$funnelSource"
    )
    $adapterAndroidTargets = @(
        "lsx4_fex_backend_v3",
        "lsx4_fex2608_context_bridge"
    ) + $androidSmokeTargets
    $adapterBuildArguments = @(
        "--build", $adapterBuild,
        "--target"
    ) + $adapterAndroidTargets + @(
        "--parallel", "$Jobs"
    )
    Invoke-Checked -Program $cmakeBin -Arguments $adapterBuildArguments

    Invoke-Checked -Program $cmakeBin -Arguments @(
        "-S", $adapterSource,
        "-B", $hostBuild,
        "-G", "Visual Studio 17 2022",
        "-A", "x64",
        "-DLSX4_FEX_ABI_DIR=$abiDirectory",
        "-DLSX4_FEX2608_SOURCE_DIR=$fexSource",
        "-DLSX4_FEX2608_LIBRARY=$fexCore",
        "-DLSX4_FUNNEL_SRC_DIR=$funnelSource"
    )
    Invoke-Checked -Program $cmakeBin -Arguments @(
        "--build", $hostBuild,
        "--config", "Release",
        "--parallel", "$Jobs"
    )
    Invoke-Checked -Program $ctestBin -Arguments @(
        "--test-dir", $hostBuild,
        "-C", "Release",
        "--output-on-failure"
    )
    Invoke-Checked -Program $pythonBin -Arguments @(
        $verifier,
        "--source-snapshot", $sourceSnapshotPath
    )
}

$fexCore = Resolve-ExistingPath (Join-Path $fexBuild "FEXCore\Source\libFEXCore.so")
$adapterLibrary = Resolve-ExistingPath (Join-Path $adapterBuild "liblsx4_fex_backend_v3.so")
$bridgeLibrary = Resolve-ExistingPath (Join-Path $adapterBuild "liblsx4_fex2608_context_bridge.so")
$runtimeSmoke = Resolve-ExistingPath (Join-Path $adapterBuild "lsx4_fex2608_runtime_smoke")
$stagedJitSmoke = Resolve-ExistingPath (Join-Path $adapterBuild "lsx4_fex2608_staged_jit_smoke")
$stagedCpuProfileProbe = Resolve-ExistingPath (Join-Path $adapterBuild "lsx4_fex2608_staged_cpu_profile_probe")
$typedHleSmoke = Resolve-ExistingPath (Join-Path $adapterBuild "lsx4_typed_hle_abi_smoke")

$verifyArguments = @(
    $verifier,
    "--write-manifest", $manifestPath,
    "--fexcore", $fexCore,
    "--adapter", $adapterLibrary,
    "--bridge", $bridgeLibrary,
    "--android-smoke", $runtimeSmoke,
    "--android-smoke", $stagedJitSmoke,
    "--android-smoke", $stagedCpuProfileProbe,
    "--android-smoke", $typedHleSmoke,
    "--ndk", $ndkRoot,
    "--cmake", $cmakeBin,
    "--ninja", $ninjaBin,
    "--abi-header", $abiHeader,
    "--api", "$AndroidApi"
) + $sourceArguments
if ($DiagnosticSkipFreshness) {
    $verifyArguments += "--skip-freshness"
}
Invoke-Checked -Program $pythonBin -Arguments $verifyArguments
$manifestVerifyArguments = @(
    "$verifier",
    "--manifest", "$manifestPath",
    "--packaged-libcxx", "$libcxxShared"
)
if ($DiagnosticSkipFreshness) {
    $manifestVerifyArguments += "--skip-freshness"
}
Invoke-Checked -Program $pythonBin -Arguments $manifestVerifyArguments

Write-Output "FEX Android runtime build verified."
Write-Output "Manifest: $manifestPath"
Write-Output "FEXCore: $fexCore"
Write-Output "Adapter: $adapterLibrary"
Write-Output "Bridge: $bridgeLibrary"
