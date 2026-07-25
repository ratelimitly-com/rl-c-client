[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("amd64", "aarch64")]
    [string]$Architecture,

    [Parameter(Mandatory = $true)]
    [ValidatePattern("^[0-9]+\.[0-9]+\.[0-9]+$")]
    [string]$Version,

    [Parameter(Mandatory = $true)]
    [ValidatePattern("^[0-9a-f]{40}$")]
    [string]$Commit,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,

    [Parameter(Mandatory = $true)]
    [string]$VcpkgRoot
)

$ErrorActionPreference = "Stop"
if (Test-Path variable:PSNativeCommandUseErrorActionPreference) {
    $PSNativeCommandUseErrorActionPreference = $true
}

function Assert-NativeSuccess {
    param([Parameter(Mandatory = $true)][string]$Operation)
    if ($LASTEXITCODE -ne 0) {
        throw "$Operation failed with exit code $LASTEXITCODE"
    }
}

$sourceDirectory = (
    Resolve-Path (Join-Path $PSScriptRoot "../..")
).Path
$config = Get-Content (
    Join-Path $sourceDirectory "manifest/windows-release.json"
) -Raw | ConvertFrom-Json
$architectureConfig = (
    $config.architectures.PSObject.Properties[$Architecture].Value
)
if ($null -eq $architectureConfig) {
    throw "Unsupported Windows release architecture: $Architecture"
}
$expectedToolset = (
    "v143,host=x64,version=$($config.msvc_toolset_version)"
)

$runtimeArchitecture = (
    [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
)
if ($runtimeArchitecture -ne $architectureConfig.runtime_architecture) {
    throw (
        "Windows SDK must build natively: expected " +
        "$($architectureConfig.runtime_architecture), got $runtimeArchitecture"
    )
}

$resolvedVcpkgRoot = (Resolve-Path $VcpkgRoot).Path
$actualVcpkgCommit = (
    & git -C $resolvedVcpkgRoot rev-parse HEAD
).Trim()
Assert-NativeSuccess "vcpkg commit query"
if ($actualVcpkgCommit -notmatch "^[0-9a-f]{40}$") {
    throw "vcpkg checkout commit is invalid: $actualVcpkgCommit"
}
if ($actualVcpkgCommit -ne $config.vcpkg_commit) {
    throw (
        "vcpkg checkout $actualVcpkgCommit does not match pinned commit " +
        $config.vcpkg_commit
    )
}
$toolchainFile = Join-Path $resolvedVcpkgRoot "scripts/buildsystems/vcpkg.cmake"
if (-not (Test-Path $toolchainFile -PathType Leaf)) {
    throw "vcpkg toolchain does not exist: $toolchainFile"
}

$programFilesX86 = ${env:ProgramFiles(x86)}
if ([string]::IsNullOrWhiteSpace($programFilesX86)) {
    throw "ProgramFiles(x86) is not available"
}
$vswhere = Join-Path $programFilesX86 (
    "Microsoft Visual Studio/Installer/vswhere.exe"
)
if (-not (Test-Path $vswhere -PathType Leaf)) {
    throw "Visual Studio locator does not exist: $vswhere"
}
$vsInstallation = (
    & $vswhere -latest -products * -property installationPath
).Trim()
Assert-NativeSuccess "Visual Studio discovery"
if ([string]::IsNullOrWhiteSpace($vsInstallation)) {
    throw "Visual Studio 2022 installation was not found"
}
$msvcDirectory = Join-Path $vsInstallation (
    "VC/Tools/MSVC/$($config.msvc_toolset_version)"
)
$compilerPath = Join-Path $msvcDirectory (
    "bin/Hostx64/$($architectureConfig.msvc_target)/cl.exe"
)
$linkerPath = Join-Path $msvcDirectory (
    "bin/Hostx64/$($architectureConfig.msvc_target)/link.exe"
)
if (-not (Test-Path $compilerPath -PathType Leaf)) {
    throw "Pinned MSVC compiler does not exist: $compilerPath"
}
if (-not (Test-Path $linkerPath -PathType Leaf)) {
    throw "Pinned MSVC linker does not exist: $linkerPath"
}
$compilerVersion = (Get-Item $compilerPath).VersionInfo.FileVersion
$linkerVersion = (Get-Item $linkerPath).VersionInfo.FileVersion

$workDirectory = Join-Path (
    [System.IO.Path]::GetTempPath()
) ("rl-windows-build-" + [guid]::NewGuid().ToString("N"))
$buildDirectory = Join-Path $workDirectory "build"
$stageDirectory = Join-Path $workDirectory "stage"
$wdkDirectory = Join-Path $workDirectory "wdk"
$sbomWdkDirectory = $wdkDirectory
$verifyDirectory = Join-Path $workDirectory "verify"

try {
    New-Item -ItemType Directory -Force -Path $workDirectory | Out-Null
    nuget install $architectureConfig.nuget_package `
        -Version $config.wdk_version `
        -OutputDirectory $wdkDirectory `
        -ExcludeVersion `
        -DirectDownload `
        -NonInteractive
    Assert-NativeSuccess "WDK NuGet restore"

    $wdkProps = Get-ChildItem $wdkDirectory -Recurse `
        -Filter "$($architectureConfig.nuget_package).props" |
        Select-Object -First 1
    if ($null -eq $wdkProps) {
        throw "Pinned WDK package did not contain its native MSBuild props"
    }
    $wdkPackageRoot = $wdkProps.Directory.Parent.Parent.FullName
    $wdkContentRoot = Join-Path $wdkPackageRoot "c"
    if (-not (Test-Path (
        Join-Path $wdkContentRoot "Include/10.0.26100.0/um"
    ) -PathType Container)) {
        throw "Pinned WDK package did not contain user-mode SDK headers"
    }
    $env:WindowsSdkDir = $wdkContentRoot + "\"
    $env:WindowsSDKVersion = "10.0.26100.0\"

    if (
        $architectureConfig.sbom_nuget_package -ne
        $architectureConfig.nuget_package
    ) {
        $sbomWdkDirectory = Join-Path $workDirectory "wdk-sbom"
        nuget install $architectureConfig.sbom_nuget_package `
            -Version $config.wdk_version `
            -OutputDirectory $sbomWdkDirectory `
            -ExcludeVersion `
            -DirectDownload `
            -NonInteractive
        Assert-NativeSuccess "WDK SBOM-tool NuGet restore"
    }

    $configureArguments = @(
        "-S", $sourceDirectory,
        "-B", $buildDirectory,
        "-G", "Visual Studio 17 2022",
        "-A", $architectureConfig.cmake_architecture,
        "-T", $expectedToolset,
        "-DCMAKE_INSTALL_PREFIX=$stageDirectory",
        "-DCMAKE_SYSTEM_VERSION=10.0.26100.0",
        "-DCMAKE_TOOLCHAIN_FILE=$toolchainFile",
        "-DVCPKG_TARGET_TRIPLET=$($architectureConfig.vcpkg_triplet)",
        "-DRCLIENT_BUILD_TESTS=ON",
        "-DRCLIENT_BUNDLE_OPENSSL=ON",
        "-DRCLIENT_RELOCATABLE_PKGCONFIG=ON",
        "-DRCLIENT_USE_STATIC_MSVC_RUNTIME=ON",
        "-DRCLIENT_VERSION=$Version",
        "-DRCLIENT_WARNINGS_AS_ERRORS=ON"
    )
    & cmake @configureArguments
    Assert-NativeSuccess "Windows CMake configure"
    $selectedToolset = Select-String `
        -Path (Join-Path $buildDirectory "CMakeCache.txt") `
        -Pattern "^CMAKE_GENERATOR_TOOLSET:INTERNAL=(.+)$"
    if (
        $null -eq $selectedToolset -or
        $selectedToolset.Matches[0].Groups[1].Value -ne
        $expectedToolset
    ) {
        throw "CMake did not select the pinned MSVC toolset"
    }
    & cmake --build $buildDirectory --config Release --parallel
    Assert-NativeSuccess "Windows CMake build"
    & ctest --test-dir $buildDirectory -C Release --output-on-failure
    Assert-NativeSuccess "Windows CTest"
    & cmake --install $buildDirectory --config Release
    Assert-NativeSuccess "Windows CMake install"

    $toolchainMetadataDirectory = Join-Path $stageDirectory "share/rl-c-client"
    New-Item -ItemType Directory -Force `
        -Path $toolchainMetadataDirectory | Out-Null
    $opensslCopyright = Join-Path $resolvedVcpkgRoot (
        "installed/$($architectureConfig.vcpkg_triplet)/share/openssl/copyright"
    )
    if (-not (Test-Path $opensslCopyright -PathType Leaf)) {
        throw "Installed OpenSSL license does not exist: $opensslCopyright"
    }
    $opensslPackage = & (Join-Path $resolvedVcpkgRoot "vcpkg.exe") list (
        "openssl:$($architectureConfig.vcpkg_triplet)"
    )
    Assert-NativeSuccess "OpenSSL version query"
    $opensslLine = $opensslPackage |
        Where-Object { $_ -match "^openssl:" } |
        Select-Object -First 1
    if ($null -eq $opensslLine -or $opensslLine -notmatch "\s([0-9]+\.[0-9]+\.[0-9]+)") {
        throw "Could not determine the installed OpenSSL version"
    }
    $opensslVersion = $Matches[1]
    $thirdPartyLicenseDirectory = Join-Path $stageDirectory (
        "share/doc/rl-c-client/third-party"
    )
    New-Item -ItemType Directory -Force `
        -Path $thirdPartyLicenseDirectory | Out-Null
    Copy-Item $opensslCopyright (
        Join-Path $thirdPartyLicenseDirectory "OpenSSL-LICENSE.txt"
    )
    & python (Join-Path $sourceDirectory "tools/write_dependency_sbom.py") `
        --output (
            Join-Path $toolchainMetadataDirectory "dependencies.spdx.json"
        ) `
        --project-version $Version `
        --openssl-version $opensslVersion
    Assert-NativeSuccess "Dependency SPDX SBOM generation"

    [ordered]@{
        architecture = $Architecture
        compiler_version = $compilerVersion
        linker_version = $linkerVersion
        msvc_runtime = "MultiThreaded"
        msvc_toolset_version = $config.msvc_toolset_version
        vcpkg_commit = $actualVcpkgCommit
        vcpkg_triplet = $architectureConfig.vcpkg_triplet
        wdk_package = $architectureConfig.nuget_package
        wdk_sbom_package = $architectureConfig.sbom_nuget_package
        wdk_version = $config.wdk_version
    } | ConvertTo-Json | Set-Content -Encoding utf8NoBOM (
        Join-Path $toolchainMetadataDirectory "toolchain.json"
    )

    $sbomTool = Get-ChildItem $sbomWdkDirectory -Recurse `
        -Filter $architectureConfig.sbom_tool |
        Select-Object -First 1
    if ($null -eq $sbomTool) {
        throw "Pinned WDK package did not contain its SBOM tool"
    }
    & $sbomTool.FullName generate `
        -b $stageDirectory `
        -bc $sourceDirectory `
        -pn "rl-c-client" `
        -pv $Version `
        -ps "Ratelimitly" `
        -nsb "https://github.com/ratelimitly-com/rl-c-client"
    Assert-NativeSuccess "WDK SBOM generation"

    New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
    & python (Join-Path $sourceDirectory "tools/package_sdk.py") `
        --stage $stageDirectory `
        --output $OutputDirectory `
        --version $Version `
        --commit $Commit `
        --platform windows `
        --architecture $Architecture `
        --archive-format zip
    Assert-NativeSuccess "Windows SDK archive creation"

    $archive = Join-Path $OutputDirectory (
        "rl-c-client-v$Version-windows-$Architecture-sdk.zip"
    )
    Expand-Archive -Path $archive -DestinationPath $verifyDirectory
    $verifiedRoot = Join-Path $verifyDirectory (
        "rl-c-client-$Version-windows-$Architecture"
    )
    & (Join-Path $PSScriptRoot "verify-sdk.ps1") `
        -Architecture $Architecture `
        -SdkRoot $verifiedRoot `
        -VcpkgRoot $resolvedVcpkgRoot
} finally {
    if (Test-Path $workDirectory) {
        Remove-Item -Recurse -Force $workDirectory
    }
}
