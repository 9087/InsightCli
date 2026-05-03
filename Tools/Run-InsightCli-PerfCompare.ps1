param(
    [string]$ExePath = "c:\PROJECTS\UnrealEngine\Engine\Binaries\Win64\InsightCli.exe",
    [string]$TracePath = "C:\Users\吴志伟\AppData\Local\UnrealEngine\Common\UnrealTrace\Store\001\20260502_160828.utrace",
    [int]$Rounds = 3,
    [int]$SamplesPerRound = 30,
    [string]$BaselineFile = "",
    [string]$OutputDir = ""
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$PSNativeCommandUseErrorActionPreference = $false

function Get-Stats {
    param([double[]]$Values)

    if ($Values.Count -le 0) {
        throw "Get-Stats requires at least one value"
    }

    $sorted = $Values | Sort-Object
    $count = $sorted.Count
    $p50Index = [Math]::Floor(0.5 * ($count - 1))
    $p95Index = [Math]::Floor(0.95 * ($count - 1))

    return [pscustomobject]@{
        min = [Math]::Round($sorted[0], 3)
        p50 = [Math]::Round($sorted[$p50Index], 3)
        p95 = [Math]::Round($sorted[$p95Index], 3)
        avg = [Math]::Round((($sorted | Measure-Object -Average).Average), 3)
        max = [Math]::Round($sorted[$count - 1], 3)
    }
}

function Invoke-Case {
    param(
        [string]$Exe,
        [string[]]$CommandArgs,
        [int]$Samples,
        [string]$CaseName,
        [int]$Round
    )

    $times = New-Object System.Collections.Generic.List[double]

    for ($i = 0; $i -lt $Samples; $i++) {
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        & $Exe @CommandArgs *> $null
        $sw.Stop()

        if ($LASTEXITCODE -ne 0) {
            throw "Command failed (round=$Round, sample=$($i + 1), case=$CaseName, exit=$LASTEXITCODE)"
        }

        $times.Add($sw.Elapsed.TotalMilliseconds)
    }

    return Get-Stats -Values $times.ToArray()
}

function Read-Baseline {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not (Test-Path -Path $Path)) {
        throw "Baseline file not found: $Path"
    }

    try {
        $json = Get-Content -Path $Path -Raw | ConvertFrom-Json
    }
    catch {
        throw "Failed to parse baseline JSON: $Path"
    }

    if ($null -eq $json.commands) {
        throw "Baseline file missing 'commands' section: $Path"
    }

    $baseline = @{}
    foreach ($property in $json.commands.PSObject.Properties) {
        $entry = $property.Value
        if ($null -eq $entry.p50 -or $null -eq $entry.p95) {
            throw "Baseline command '$($property.Name)' must define p50 and p95"
        }

        $p50 = [double]$entry.p50
        $p95 = [double]$entry.p95
        if ($p50 -le 0 -or $p95 -le 0) {
            throw "Baseline command '$($property.Name)' must have positive p50/p95 values"
        }

        $baseline[$property.Name] = @{
            p50 = $p50
            p95 = $p95
        }
    }

    return $baseline
}

if (-not (Test-Path -Path $ExePath)) {
    throw "InsightCli executable not found: $ExePath"
}
if (-not (Test-Path -Path $TracePath)) {
    throw "Trace file not found: $TracePath"
}
if ($Rounds -le 0) {
    throw "Rounds must be > 0"
}
if ($SamplesPerRound -le 0) {
    throw "SamplesPerRound must be > 0"
}
if ([string]::IsNullOrWhiteSpace($BaselineFile)) {
    $BaselineFile = Join-Path $PSScriptRoot "InsightCli.PerfBaseline.json"
}

$cases = @(
    @{ Name = 'info summary'; Args = @($TracePath, 'info', 'summary') },
    @{ Name = 'frames summary'; Args = @($TracePath, 'frames', 'summary') },
    @{ Name = 'cpu top --thread GameThread --limit 3'; Args = @($TracePath, 'cpu', 'top', '--thread', 'GameThread', '--limit', '3') }
)

$baseline = Read-Baseline -Path $BaselineFile

foreach ($case in $cases) {
    if (-not $baseline.ContainsKey($case.Name)) {
        throw "Baseline file missing case '$($case.Name)'. Add it under commands in $BaselineFile"
    }
}

$rawRows = @()
for ($round = 1; $round -le $Rounds; $round++) {
    Write-Host "Running round $round / $Rounds ..."

    foreach ($case in $cases) {
        $stats = Invoke-Case -Exe $ExePath -CommandArgs $case.Args -Samples $SamplesPerRound -CaseName $case.Name -Round $round
        $rawRows += [pscustomobject]@{
            round = $round
            command = $case.Name
            min_ms = $stats.min
            p50_ms = $stats.p50
            p95_ms = $stats.p95
            avg_ms = $stats.avg
            max_ms = $stats.max
        }
    }
}

$cmpRows = foreach ($row in $rawRows) {
    $base = $baseline[$row.command]
    if ($null -eq $base) {
        throw "Baseline missing for command '$($row.command)'"
    }

    [pscustomobject]@{
        round = $row.round
        command = $row.command
        p50_ms = $row.p50_ms
        p95_ms = $row.p95_ms
        p50_improve_pct = [Math]::Round((($base.p50 - $row.p50_ms) / $base.p50) * 100, 2)
        p95_improve_pct = [Math]::Round((($base.p95 - $row.p95_ms) / $base.p95) * 100, 2)
    }
}

$varianceRows = foreach ($name in ($cases | ForEach-Object { $_.Name })) {
    $rows = $rawRows | Where-Object { $_.command -eq $name }
    [pscustomobject]@{
        command = $name
        p50_min_ms = ($rows | Measure-Object p50_ms -Minimum).Minimum
        p50_max_ms = ($rows | Measure-Object p50_ms -Maximum).Maximum
        p95_min_ms = ($rows | Measure-Object p95_ms -Minimum).Minimum
        p95_max_ms = ($rows | Measure-Object p95_ms -Maximum).Maximum
    }
}

Write-Host ""
Write-Host "=== Raw stats (Rounds=$Rounds, SamplesPerRound=$SamplesPerRound) ==="
$rawRows | Format-Table -AutoSize

Write-Host ""
Write-Host "=== Compared to baseline: $BaselineFile ==="
$cmpRows | Format-Table -AutoSize

Write-Host ""
Write-Host "=== Round-to-round p50/p95 variance summary ==="
$varianceRows | Format-Table -AutoSize

if (-not [string]::IsNullOrWhiteSpace($OutputDir)) {
    if (-not (Test-Path -Path $OutputDir)) {
        New-Item -ItemType Directory -Path $OutputDir | Out-Null
    }

    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $rawPath = Join-Path $OutputDir "perf-raw-$stamp.csv"
    $cmpPath = Join-Path $OutputDir "perf-compare-$stamp.csv"
    $varPath = Join-Path $OutputDir "perf-variance-$stamp.csv"

    $rawRows | Export-Csv -Path $rawPath -NoTypeInformation -Encoding UTF8
    $cmpRows | Export-Csv -Path $cmpPath -NoTypeInformation -Encoding UTF8
    $varianceRows | Export-Csv -Path $varPath -NoTypeInformation -Encoding UTF8

    Write-Host ""
    Write-Host "Saved CSV outputs:" 
    Write-Host "  $rawPath"
    Write-Host "  $cmpPath"
    Write-Host "  $varPath"
}
