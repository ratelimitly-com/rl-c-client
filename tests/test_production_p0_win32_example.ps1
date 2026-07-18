[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ExamplePath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$TestName = "test_production_p0_win32_example"
$AuthKey = [Environment]::GetEnvironmentVariable(
    "RATELIMITLY_AUTH_KEY",
    [EnvironmentVariableTarget]::Process
)
if ([string]::IsNullOrEmpty($AuthKey)) {
    throw "$TestName`: RATELIMITLY_AUTH_KEY is required"
}
[Environment]::SetEnvironmentVariable(
    "RATELIMITLY_AUTH_KEY",
    $null,
    [EnvironmentVariableTarget]::Process
)

# A production smoke test must prove key-derived P0 discovery. Reject an
# accidental override, then remove even an inherited empty value before the
# executable starts. The authentication key is added only to an explicit child
# environment block and removed from that block immediately after process
# creation. It is never a command argument or inherited by this runner's sleep,
# diagnostics, or cleanup helpers.
$DiscoveryVariables = @(
    "RATELIMITLY_TENANT",
    "RATELIMITLY_EXAMPLE_SERVER_HOST",
    "RATELIMITLY_EXAMPLE_SERVER_PORT"
)
$SavedDiscoveryEnvironment = @{}
foreach ($Name in $DiscoveryVariables) {
    $Value = [Environment]::GetEnvironmentVariable(
        $Name,
        [EnvironmentVariableTarget]::Process
    )
    $SavedDiscoveryEnvironment[$Name] = $Value
    if (-not [string]::IsNullOrEmpty($Value)) {
        throw "$TestName`: $Name must not override key-derived production discovery"
    }
    [Environment]::SetEnvironmentVariable(
        $Name,
        $null,
        [EnvironmentVariableTarget]::Process
    )
}

$ExamplePath = (Resolve-Path -LiteralPath $ExamplePath).Path
if ((Get-Item -LiteralPath $ExamplePath).PSIsContainer) {
    throw "$TestName`: ExamplePath must name an executable file"
}

$TempRoot = Join-Path ([IO.Path]::GetTempPath()) `
    ("r-production-p0-win32-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $TempRoot | Out-Null
$StdoutPath = Join-Path $TempRoot "example.out"
$StderrPath = Join-Path $TempRoot "example.err"
$Client = $null
$StdoutRead = $null
$StderrRead = $null
$Failure = $null

function Stop-ClientTree {
    param([System.Diagnostics.Process]$Process)

    if ($null -eq $Process) {
        return $true
    }
    try {
        if ($Process.HasExited) {
            return $true
        }
    }
    catch {
        # Fall through to both kill mechanisms when process state cannot be
        # inspected. A cleanup error must never be mistaken for termination.
    }

    # Process.Kill(true) is the primary native tree-kill mechanism on current
    # GitHub Windows runners. If it throws *or* misses its five-second deadline,
    # taskkill is a bounded compatibility fallback. Return false unless the
    # original client is observed exiting; callers turn that into a test failure.
    $Stopped = $false
    try {
        $Process.Kill($true)
        $Stopped = $Process.WaitForExit(5000)
    }
    catch {
        $Stopped = $false
    }

    if (-not $Stopped) {
        $Taskkill = $null
        try {
            $Taskkill = Start-Process `
                -FilePath "$env:SystemRoot\System32\taskkill.exe" `
                -ArgumentList @("/PID", "$($Process.Id)", "/T", "/F") `
                -WindowStyle Hidden `
                -PassThru
            if (-not $Taskkill.WaitForExit(5000)) {
                $Taskkill.Kill($true)
                [void]$Taskkill.WaitForExit(1000)
            }
            $Stopped = $Process.WaitForExit(5000)
        }
        catch {
            $Stopped = $false
        }
    }

    if (-not $Stopped) {
        try {
            $Stopped = $Process.HasExited
        }
        catch {
            $Stopped = $false
        }
    }
    return $Stopped
}

function Protect-DiagnosticText {
    param([AllowEmptyString()][string]$Text)

    if ([string]::IsNullOrEmpty($Text)) {
        return ""
    }
    $Protected = $Text.Replace($script:AuthKey, "[REDACTED_AUTH_KEY]")
    $Protected = [regex]::Replace(
        $Protected,
        "rl-(?:aes|hmac)[A-Za-z0-9_-]+",
        "[REDACTED_AUTH_KEY]",
        [Text.RegularExpressions.RegexOptions]::IgnoreCase
    )
    return $Protected
}

function Write-BoundedDiagnosticFile {
    param(
        [Parameter(Mandatory = $true)][string]$Label,
        [Parameter(Mandatory = $true)][string]$Path
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    $Text = [IO.File]::ReadAllText($Path)
    if ([string]::IsNullOrEmpty($Text)) {
        return
    }

    [Console]::Error.WriteLine("--- $Label (sanitized)")
    $Lines = [regex]::Split((Protect-DiagnosticText $Text), "\r?\n")
    $Limit = [Math]::Min($Lines.Count, 80)
    for ($Index = 0; $Index -lt $Limit; $Index++) {
        $Line = $Lines[$Index]
        if ($Line.Length -gt 1000) {
            $Line = $Line.Substring(0, 1000) + " [line truncated]"
        }
        [Console]::Error.WriteLine($Line)
    }
    if ($Lines.Count -gt $Limit) {
        [Console]::Error.WriteLine("[diagnostic output truncated after 80 lines]")
    }
}

try {
    # The example's tracker retains samples for ten seconds. CI serializes the
    # fixed Win32 identities; one bounded drain prevents a cancelled prior run
    # from influencing this fresh production admission.
    Write-Host "$TestName`: draining stale production state (11 seconds)"
    Start-Sleep -Seconds 11

    # Construct ProcessStartInfo directly. This makes the exact environment
    # passed to CreateProcess auditable and avoids Start-Process's environment
    # overlay, which was not visible to the native executable in CI.
    $StartInfo = [Diagnostics.ProcessStartInfo]::new()
    $StartInfo.FileName = $ExamplePath
    $StartInfo.UseShellExecute = $false
    $StartInfo.CreateNoWindow = $true
    $StartInfo.RedirectStandardOutput = $true
    $StartInfo.RedirectStandardError = $true
    $StartInfo.Environment["RATELIMITLY_AUTH_KEY"] = $AuthKey
    foreach ($Name in $DiscoveryVariables) {
        [void]$StartInfo.Environment.Remove($Name)
    }

    $Client = [Diagnostics.Process]::new()
    $Client.StartInfo = $StartInfo
    try {
        if (-not $Client.Start()) {
            throw "CreateProcess did not start the example"
        }
        # Drain both pipes concurrently. Waiting before starting these reads
        # can deadlock when a child fills either redirected pipe.
        $StdoutRead = $Client.StandardOutput.ReadToEndAsync()
        $StderrRead = $Client.StandardError.ReadToEndAsync()
    }
    finally {
        # Drop the credential as soon as CreateProcess has copied the block.
        [void]$StartInfo.Environment.Remove("RATELIMITLY_AUTH_KEY")
    }

    # Keep the network smoke bounded even if DNS, UDP, or shutdown wedges.
    if (-not $Client.WaitForExit(60000)) {
        $Stopped = Stop-ClientTree $Client
        if (-not $Stopped) {
            throw "example exceeded the 60-second hard deadline and could not be stopped"
        }
        throw "example exceeded the 60-second hard deadline"
    }
    $Client.WaitForExit()

    $Stdout = $StdoutRead.GetAwaiter().GetResult()
    $Stderr = $StderrRead.GetAwaiter().GetResult()
    [IO.File]::WriteAllText($StdoutPath, $Stdout)
    [IO.File]::WriteAllText($StderrPath, $Stderr)

    if ($Client.ExitCode -ne 0) {
        throw "example exited $($Client.ExitCode); expected 0"
    }

    $Expected = `
        '\Aallowed: inventory response prepared by Win32; latency=[0-9]+ ms(?:\r\n|\n)?\z'
    if (-not [regex]::IsMatch($Stdout, $Expected)) {
        throw "stdout was not the single documented allowed/latency line"
    }
    if ($Stderr.Length -ne 0) {
        throw "example wrote unexpected stderr"
    }

    Write-Host "$TestName`: PASS (production P0 admission and latency report)"
}
catch {
    $Failure = Protect-DiagnosticText $_.Exception.Message
    Write-BoundedDiagnosticFile "example stdout" $StdoutPath
    Write-BoundedDiagnosticFile "example stderr" $StderrPath
}
finally {
    $Stopped = Stop-ClientTree $Client
    if (-not $Stopped -and $null -eq $Failure) {
        $Failure = "example process tree survived both bounded cleanup mechanisms"
    }
    foreach ($Name in $DiscoveryVariables) {
        [Environment]::SetEnvironmentVariable(
            $Name,
            $SavedDiscoveryEnvironment[$Name],
            [EnvironmentVariableTarget]::Process
        )
    }
    Remove-Item -LiteralPath $TempRoot -Recurse -Force -ErrorAction SilentlyContinue
    [Environment]::SetEnvironmentVariable(
        "RATELIMITLY_AUTH_KEY",
        $AuthKey,
        [EnvironmentVariableTarget]::Process
    )
}

if ($null -ne $Failure) {
    [Console]::Error.WriteLine("$TestName`: $Failure")
    exit 1
}
