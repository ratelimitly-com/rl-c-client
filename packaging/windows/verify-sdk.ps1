[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("amd64")]
    [string]$Architecture,

    [Parameter(Mandatory = $true)]
    [string]$SdkRoot,

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
$resolvedSdkRoot = (Resolve-Path $SdkRoot).Path
$config = Get-Content (
    Join-Path $sourceDirectory "manifest/windows-release.json"
) -Raw | ConvertFrom-Json
$architectureConfig = (
    $config.architectures.PSObject.Properties[$Architecture].Value
)
$dll = Join-Path $resolvedSdkRoot "bin/rclient.dll"
if (-not (Test-Path $dll -PathType Leaf)) {
    throw "Windows SDK is missing bin/rclient.dll"
}
if (-not (Test-Path (
    Join-Path $resolvedSdkRoot "_manifest/spdx_2.2/manifest.spdx.json"
) -PathType Leaf)) {
    throw "Windows SDK is missing the WDK-generated SPDX SBOM"
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} `
    "Microsoft Visual Studio/Installer/vswhere.exe"
$visualStudioRoot = & $vswhere -latest -products * -property installationPath
Assert-NativeSuccess "Visual Studio discovery"
$targetToolDirectory = if ($Architecture -eq "amd64") { "x64" } else { "arm64" }
$dumpbin = Get-ChildItem (
    Join-Path $visualStudioRoot "VC/Tools/MSVC"
) -Recurse -Filter "dumpbin.exe" |
    Where-Object {
        $_.FullName -match "\\$targetToolDirectory\\dumpbin[.]exe$"
    } |
    Select-Object -First 1
if ($null -eq $dumpbin) {
    throw "Could not locate dumpbin.exe for $targetToolDirectory"
}

$headers = (& $dumpbin.FullName /nologo /headers $dll | Out-String)
Assert-NativeSuccess "PE header audit"
$expectedMachine = if ($Architecture -eq "amd64") {
    "machine (x64)"
} else {
    "machine (ARM64)"
}
if ($headers -notmatch [regex]::Escape($expectedMachine)) {
    throw "rclient.dll does not report $expectedMachine"
}

$dependents = (& $dumpbin.FullName /nologo /dependents $dll | Out-String)
Assert-NativeSuccess "PE import audit"
$imports = [regex]::Matches(
    $dependents,
    "(?im)^\s+([A-Za-z0-9._-]+[.]dll)\s*$"
) | ForEach-Object { $_.Groups[1].Value.ToLowerInvariant() } |
    Sort-Object -Unique
foreach ($import in $imports) {
    foreach ($pattern in $config.forbidden_import_patterns) {
        if ($import -match $pattern) {
            throw "rclient.dll imports forbidden runtime dependency: $import"
        }
    }
}

$exportOutput = (& $dumpbin.FullName /nologo /exports $dll | Out-String)
Assert-NativeSuccess "PE export audit"
$actualExports = [regex]::Matches(
    $exportOutput,
    "(?im)^\s+[0-9]+\s+[0-9A-F]+\s+[0-9A-F]+\s+([A-Za-z_][A-Za-z0-9_]*)\s*$"
) | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
$expectedExports = Get-Content (
    Join-Path $sourceDirectory "manifest/public-api.symbols"
) | Where-Object { $_ -ne "" } | Sort-Object -Unique
$exportDifference = Compare-Object `
    -ReferenceObject $expectedExports `
    -DifferenceObject $actualExports `
    -CaseSensitive
if ($null -ne $exportDifference) {
    $exportDifference | Format-Table | Out-String | Write-Error
    throw "rclient.dll export table differs from public-api.symbols"
}

$consumerBuild = Join-Path (
    [System.IO.Path]::GetTempPath()
) ("rl-windows-consumer-" + [guid]::NewGuid().ToString("N"))
try {
    $consumerSource = Join-Path $sourceDirectory `
        "tests/fixtures/installed_consumer"
    $toolchainFile = Join-Path $VcpkgRoot "scripts/buildsystems/vcpkg.cmake"
    $configureArguments = @(
        "-S", $consumerSource,
        "-B", $consumerBuild,
        "-G", "Visual Studio 17 2022",
        "-A", $architectureConfig.cmake_architecture,
        "-DCMAKE_PREFIX_PATH=$resolvedSdkRoot",
        "-DCMAKE_TOOLCHAIN_FILE=$toolchainFile",
        "-DVCPKG_TARGET_TRIPLET=$($architectureConfig.vcpkg_triplet)"
    )
    & cmake @configureArguments
    Assert-NativeSuccess "Packaged Windows consumer configure"
    & cmake --build $consumerBuild --config Release --parallel
    Assert-NativeSuccess "Packaged Windows consumer build"
    $savedPath = $env:PATH
    try {
        $env:PATH = (Join-Path $resolvedSdkRoot "bin") + ";" + $env:PATH
        & (Join-Path $consumerBuild "Release/rclient-installed-consumer.exe")
        Assert-NativeSuccess "Packaged Windows consumer execution"
    } finally {
        $env:PATH = $savedPath
    }
} finally {
    if (Test-Path $consumerBuild) {
        Remove-Item -Recurse -Force $consumerBuild
    }
}
