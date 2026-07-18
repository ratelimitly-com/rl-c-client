[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ExamplePath,

    [Parameter(Mandatory = $true)]
    [string]$ResponderPath,

    [int]$BasePort = 39140
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ExamplePath = (Resolve-Path -LiteralPath $ExamplePath).Path
$ResponderPath = (Resolve-Path -LiteralPath $ResponderPath).Path
if ($BasePort -lt 1024 -or $BasePort -gt 65532) {
    throw "BasePort must be in the range 1024..65532"
}

$SyntheticKey = "rl-aes1qvqqqqqqqqqqqqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqqqqzqqqqsqqqqqsqqqyqqqqqqkqzqqqhmzd8l"
$ExpectedTracker = [ordered]@{
    ttl_ms               = 10000
    max_samples          = 100
    buffer_size          = 32
    min_sample_threshold = 5
}
$Scenarios = @(
    [pscustomobject]@{
        Name = "guard-pass"; PortOffset = 0; ExitCode = 0
        OutputPattern = "(?m)^allowed: .+; latency=[0-9]+ ms\r?$"
    },
    [pscustomobject]@{
        Name = "deny"; PortOffset = 1; ExitCode = 2
        OutputPattern = "(?m)^denied: resource rate limit\r?$"
    },
    [pscustomobject]@{
        Name = "guard-deny"; PortOffset = 2; ExitCode = 2
        OutputPattern = "(?m)^denied: latency guard\r?$"
    }
)

$TempRoot = Join-Path ([IO.Path]::GetTempPath()) `
    ("r-win32-native-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $TempRoot | Out-Null

function Stop-TestProcess {
    param([System.Diagnostics.Process]$Process)
    if ($null -eq $Process) {
        return
    }
    if (-not $Process.HasExited) {
        try {
            $Process.Kill($true)
        }
        catch {
            Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
        }
        [void]$Process.WaitForExit(5000)
    }
}

function Show-ScenarioDiagnostics {
    param([string]$Directory)
    foreach ($Name in @("example.out", "example.err", "responder.out", "responder.err")) {
        $Path = Join-Path $Directory $Name
        if (Test-Path -LiteralPath $Path) {
            $Text = Get-Content -Raw -LiteralPath $Path
            if (-not [string]::IsNullOrWhiteSpace($Text)) {
                Write-Host "--- $Name"
                Write-Host $Text
            }
        }
    }
}

function Assert-Tracker {
    param(
        [Parameter(Mandatory = $true)]$Tracker,
        [Parameter(Mandatory = $true)][string]$Context
    )
    foreach ($Entry in $ExpectedTracker.GetEnumerator()) {
        $Property = $Tracker.PSObject.Properties[$Entry.Key]
        if ($null -eq $Property) {
            throw "$Context tracker omitted $($Entry.Key)"
        }
        $Actual = $Property.Value
        if ([long]$Actual -ne [long]$Entry.Value) {
            throw "$Context tracker $($Entry.Key) was $Actual; expected $($Entry.Value)"
        }
    }
}

function Read-JsonLines {
    param([Parameter(Mandatory = $true)][string]$Path)
    return @(
        Get-Content -LiteralPath $Path |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
            ForEach-Object { $_ | ConvertFrom-Json }
    )
}

function Invoke-Scenario {
    param([Parameter(Mandatory = $true)]$Scenario)

    $Directory = Join-Path $TempRoot $Scenario.Name
    New-Item -ItemType Directory -Path $Directory | Out-Null
    $ResponderOut = Join-Path $Directory "responder.out"
    $ResponderErr = Join-Path $Directory "responder.err"
    $ExampleOut = Join-Path $Directory "example.out"
    $ExampleErr = Join-Path $Directory "example.err"
    $Port = $BasePort + $Scenario.PortOffset
    $Responder = $null
    $Client = $null

    try {
        $Responder = Start-Process `
            -FilePath $ResponderPath `
            -ArgumentList @(
                "--listen=127.0.0.1:$Port",
                "--scenario=$($Scenario.Name)",
                "--auth=aes"
            ) `
            -RedirectStandardOutput $ResponderOut `
            -RedirectStandardError $ResponderErr `
            -NoNewWindow `
            -PassThru

        $Ready = $false
        for ($Attempt = 0; $Attempt -lt 300; $Attempt++) {
            if ((Test-Path -LiteralPath $ResponderOut) -and
                (Select-String -Quiet -SimpleMatch '"event":"ready"' $ResponderOut)) {
                $Ready = $true
                break
            }
            if ($Responder.HasExited) {
                break
            }
            Start-Sleep -Milliseconds 20
        }
        if (-not $Ready) {
            throw "responder did not become ready"
        }

        # The fixed endpoint makes this test deterministic. Removing the tenant
        # override still exercises tenant/key consistency in the packet path;
        # DNS formatting and real P0 discovery have separate coverage.
        $env:RATELIMITLY_TENANT = $null
        $env:RATELIMITLY_AUTH_KEY = $SyntheticKey
        $env:RATELIMITLY_EXAMPLE_SERVER_HOST = "127.0.0.1"
        $env:RATELIMITLY_EXAMPLE_SERVER_PORT = "$Port"

        $Client = Start-Process `
            -FilePath $ExamplePath `
            -RedirectStandardOutput $ExampleOut `
            -RedirectStandardError $ExampleErr `
            -NoNewWindow `
            -PassThru
        if (-not $Client.WaitForExit(30000)) {
            Stop-TestProcess $Client
            throw "example timed out"
        }
        if ($Client.ExitCode -ne $Scenario.ExitCode) {
            throw "example exited $($Client.ExitCode); expected $($Scenario.ExitCode)"
        }

        $ExampleText = Get-Content -Raw -LiteralPath $ExampleOut
        if ($ExampleText -notmatch $Scenario.OutputPattern) {
            throw "example output did not match $($Scenario.OutputPattern)"
        }
        if ($Scenario.Name -ne "guard-pass" -and $ExampleText -match "(?m)^allowed:") {
            throw "denied path exposed protected work"
        }

        # Drain after process exit, then force-stop the fixture. Every event is
        # flushed as JSONL, so late and duplicate reports remain assertable.
        Start-Sleep -Milliseconds 200
        if ($Responder.HasExited) {
            throw "responder exited before the post-client drain"
        }
        Stop-TestProcess $Responder

        $Records = Read-JsonLines $ResponderOut
        $RateRecords = @($Records | Where-Object { $_.event -eq "rate_request" })
        $LatencyRecords = @($Records | Where-Object { $_.event -eq "latency_report" })
        $RejectedRecords = @($Records | Where-Object { $_.event -eq "input_rejected" })
        if ($RateRecords.Count -ne 1) {
            throw "observed $($RateRecords.Count) rate requests; expected 1"
        }
        if ($RejectedRecords.Count -ne 0) {
            throw "responder rejected $($RejectedRecords.Count) packets"
        }

        $Rate = $RateRecords[0]
        if ($Rate.guards -ne 1 -or $Rate.resources -ne 1) {
            throw "request did not contain one guard and one resource"
        }
        if ($Rate.label -ne "win32-example") {
            throw "metrics label was $($Rate.label); expected win32-example"
        }
        if ($Rate.guard_threshold_ms -ne 100) {
            throw "guard threshold was $($Rate.guard_threshold_ms); expected 100"
        }
        if ($Rate.disposition -ne $Scenario.Name) {
            throw "responder disposition was $($Rate.disposition)"
        }
        Assert-Tracker $Rate.tracker "$($Scenario.Name) guard"

        $ExpectedLatencyCount = if ($Scenario.Name -eq "guard-pass") { 1 } else { 0 }
        if ($LatencyRecords.Count -ne $ExpectedLatencyCount) {
            throw "observed $($LatencyRecords.Count) latency reports; expected $ExpectedLatencyCount"
        }
        if ($ExpectedLatencyCount -eq 1) {
            $Latency = $LatencyRecords[0]
            if ($Latency.reports -ne 1) {
                throw "latency packet contained $($Latency.reports) reports; expected 1"
            }
            if ($null -eq $Latency.PSObject.Properties["observed_latency_ms"] -or
                [long]$Latency.observed_latency_ms -lt 0) {
                throw "latency observation was negative"
            }
            if ($Latency.matches_previous_guard -ne $true) {
                throw "latency report targeted a different tracker"
            }
            Assert-Tracker $Latency.tracker "$($Scenario.Name) report"
        }
    }
    catch {
        Show-ScenarioDiagnostics $Directory
        throw "$($Scenario.Name): $($_.Exception.Message)"
    }
    finally {
        Stop-TestProcess $Client
        Stop-TestProcess $Responder
    }
}

$EnvironmentNames = @(
    "RATELIMITLY_TENANT",
    "RATELIMITLY_AUTH_KEY",
    "RATELIMITLY_EXAMPLE_SERVER_HOST",
    "RATELIMITLY_EXAMPLE_SERVER_PORT"
)
$SavedEnvironment = @{}
foreach ($Name in $EnvironmentNames) {
    $SavedEnvironment[$Name] = [Environment]::GetEnvironmentVariable($Name, "Process")
}

try {
    foreach ($Scenario in $Scenarios) {
        Invoke-Scenario $Scenario
        Write-Host "$($Scenario.Name): PASS"
    }
    Write-Host "test_windows_native_example: PASS"
}
finally {
    foreach ($Name in $EnvironmentNames) {
        [Environment]::SetEnvironmentVariable(
            $Name,
            $SavedEnvironment[$Name],
            "Process"
        )
    }
    Remove-Item -LiteralPath $TempRoot -Recurse -Force -ErrorAction SilentlyContinue
}
