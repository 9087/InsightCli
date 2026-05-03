param(
    [string]$ExePath = "c:\PROJECTS\UnrealEngine\Engine\Binaries\Win64\InsightCli.exe",
    [string[]]$TracePaths = @()
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$script:StepCounter = 0
$script:TotalSteps = 0

function Write-Step {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    $script:StepCounter++
    Write-Host ("[{0}/{1}] {2}" -f $script:StepCounter, $script:TotalSteps, $Message)
}

function Parse-JsonOutput {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Text,
        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    $normalized = $Text
    if ($null -eq $normalized) {
        throw "Expected valid JSON for $Context. Output was null"
    }

    $normalized = [string]$normalized
    $normalized = $normalized.Trim()

    if ($normalized.Length -gt 0 -and [int][char]$normalized[0] -eq 0xFEFF) {
        $normalized = $normalized.Substring(1).Trim()
    }

    $firstBrace = $normalized.IndexOf('{')
    $lastBrace = $normalized.LastIndexOf('}')
    if ($firstBrace -ge 0 -and $lastBrace -gt $firstBrace) {
        $normalized = $normalized.Substring($firstBrace, $lastBrace - $firstBrace + 1)
    }

    try {
        return ($normalized | ConvertFrom-Json)
    }
    catch {
        throw "Expected valid JSON for $Context. Output: $Text"
    }
}

function Invoke-InsightCli {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Args
    )

    $previousEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $output = & $ExePath @Args 2>&1
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = $previousEap

    return [pscustomobject]@{
        ExitCode = $exitCode
        Text = ($output | Out-String)
    }
}

function Invoke-SmokeCase {
    param(
        [Parameter(Mandatory = $true)]
        [pscustomobject]$Case
    )

    if (-not $Case.NoStep) {
        Write-Step $Case.Message
    }
    $result = Invoke-InsightCli -Args $Case.Args

    if ($Case.AllowExitCodes.Count -gt 0) {
        if ($Case.AllowExitCodes -notcontains $result.ExitCode) {
            throw "Expected exit code in [$($Case.AllowExitCodes -join ', ')] for $($Case.Context), got $($result.ExitCode)"
        }
    }
    elseif ($Case.ExpectNonZero) {
        if ($result.ExitCode -eq 0) {
            throw "Expected non-zero exit code for $($Case.Context)"
        }
    }
    elseif ($result.ExitCode -ne 0) {
        throw "Expected exit code 0 for $($Case.Context), got $($result.ExitCode)"
    }

    foreach ($pattern in $Case.MustContain) {
        if ($result.Text -notmatch $pattern) {
            throw "Expected pattern $pattern in $($Case.Context). Output: $($result.Text)"
        }
    }

    foreach ($pattern in $Case.MustNotContain) {
        if ($result.Text -match $pattern) {
            throw "Did not expect pattern $pattern in $($Case.Context). Output: $($result.Text)"
        }
    }

    if ($null -ne $Case.Validate) {
        & $Case.Validate $result
    }
}

function Invoke-SmokeCaseList {
    param(
        [Parameter(Mandatory = $true)]
        [array]$Cases
    )

    foreach ($case in $Cases) {
        Invoke-SmokeCase -Case $case
    }
}

function New-SmokeCase {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Message,
        [Parameter(Mandatory = $true)]
        [string]$Context,
        [Parameter(Mandatory = $true)]
        [string[]]$Args,
        [string[]]$MustContain = @(),
        [string[]]$MustNotContain = @(),
        [int[]]$AllowExitCodes = @(),
        [switch]$ExpectNonZero,
        [switch]$NoStep,
        [scriptblock]$Validate
    )

    return [pscustomobject]@{
        Message = $Message
        Context = $Context
        Args = $Args
        MustContain = $MustContain
        MustNotContain = $MustNotContain
        AllowExitCodes = $AllowExitCodes
        ExpectNonZero = $ExpectNonZero.IsPresent
        NoStep = $NoStep.IsPresent
        Validate = $Validate
    }
}

function Assert-Descending {
    param(
        [Parameter(Mandatory = $true)]
        [array]$Items,
        [Parameter(Mandatory = $true)]
        [string]$Property,
        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    for ($i = 1; $i -lt $Items.Count; $i++) {
        if ([double]$Items[$i].$Property -gt [double]$Items[$i - 1].$Property) {
            throw "Expected descending order by $Property in $Context"
        }
    }
}

function Assert-Ascending {
    param(
        [Parameter(Mandatory = $true)]
        [array]$Items,
        [Parameter(Mandatory = $true)]
        [string]$Property,
        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    for ($i = 1; $i -lt $Items.Count; $i++) {
        if ([double]$Items[$i].$Property -lt [double]$Items[$i - 1].$Property) {
            throw "Expected ascending order by $Property in $Context"
        }
    }
}

function Invoke-NormalTraceSmoke {
    param(
        [Parameter(Mandatory = $true)]
        [string]$TracePath
    )

    $preDetailCases = @(
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify unknown compatibility option is rejected..." -Context 'info summary with extra format arg' -Args @($TracePath, 'info', 'summary', '--format', 'csv') -MustContain @('"E1003"', 'unknown_options') -ExpectNonZero),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify normal JSON path still works..." -Context 'info summary' -Args @($TracePath, 'info', 'summary') -MustContain @('"data"', 'trace_name') -Validate {
            param($result)
            $json = Parse-JsonOutput -Text $result.Text -Context 'info summary'
            if ($null -eq $json.data) {
                throw 'Expected info summary response to include data object'
            }
            if ($null -eq $json.data.start_timestamp -or [string]::IsNullOrWhiteSpace([string]$json.data.start_timestamp)) {
                throw 'Expected info summary to include start_timestamp'
            }
            if ($null -eq $json.data.duration_ms) {
                throw 'Expected info summary to include duration_ms'
            }

            $durationMs = [double]$json.data.duration_ms
            if ($durationMs -gt 0.0) {
                if ($null -eq $json.data.end_timestamp -or [string]::IsNullOrWhiteSpace([string]$json.data.end_timestamp)) {
                    throw 'Expected info summary to include non-empty end_timestamp when duration_ms > 0'
                }
                if ([string]$json.data.end_timestamp -eq [string]$json.data.start_timestamp) {
                    throw 'Expected end_timestamp to differ from start_timestamp when duration_ms > 0'
                }
            }
            elseif ([string]$json.data.end_timestamp -ne 'unavailable') {
                throw 'Expected end_timestamp to be unavailable when duration_ms == 0'
            }
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify info channels returns channel catalog..." -Context 'info channels' -Args @($TracePath, 'info', 'channels') -MustContain @('"data"', '"channels"') -Validate {
            param($result)
            $json = Parse-JsonOutput -Text $result.Text -Context 'info channels'
            if ($null -eq $json.data -or $null -eq $json.data.channels) {
                throw 'Expected info channels response to include data.channels array'
            }
            if ($null -eq $json.meta -or $null -eq $json.meta.channel_count) {
                throw 'Expected info channels response to include meta.channel_count'
            }

            foreach ($channel in $json.data.channels) {
                if ([string]::IsNullOrWhiteSpace([string]$channel.name)) {
                    throw 'Expected each info channels row to include non-empty name'
                }
            }
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify batch mode returns NDJSON in request order..." -Context 'batch mode ndjson' -Args @($TracePath, 'info', 'summary') -NoStep -Validate {
            param($result)

            $batchPath = Join-Path ([System.IO.Path]::GetTempPath()) ("insightcli-batch-" + [guid]::NewGuid().ToString() + ".json")
            $batchJson = @'
[
  { "group": "info", "action": "summary" },
  { "group": "frames", "action": "summary", "options": { "frame-range": "1:3" } },
  { "group": "counters", "action": "stats", "options": { "name": "UnknownCounter" } }
]
'@

            try {
                Set-Content -Path $batchPath -Value $batchJson -Encoding utf8
                $batchResult = Invoke-InsightCli -Args @($TracePath, '--batch', $batchPath)
                if ($batchResult.ExitCode -eq 0) {
                    throw 'Expected non-zero exit code for mixed batch with one invalid sub-command'
                }

                $lines = @($batchResult.Text -split "`r?`n" | Where-Object {
                    $line = $_.Trim()
                    return -not [string]::IsNullOrWhiteSpace($line) -and $line.StartsWith('{') -and $line.EndsWith('}')
                })
                if ($lines.Count -ne 3) {
                    throw "Expected 3 NDJSON lines from batch, got $($lines.Count)"
                }

                $first = Parse-JsonOutput -Text $lines[0] -Context 'batch line 1'
                if ($null -eq $first.data -or [string]::IsNullOrWhiteSpace([string]$first.data.trace_name)) {
                    throw 'Expected batch first line to be info summary envelope'
                }

                $second = Parse-JsonOutput -Text $lines[1] -Context 'batch line 2'
                if ($second.meta.time_window_source -ne 'frame-range') {
                    throw 'Expected batch second line to include frame-range window metadata'
                }

                $third = Parse-JsonOutput -Text $lines[2] -Context 'batch line 3'
                if ($third.code -ne 'E2001') {
                    throw 'Expected batch third line to be error envelope with E2001'
                }
            }
            finally {
                if (Test-Path -Path $batchPath) {
                    Remove-Item -Path $batchPath -Force -ErrorAction SilentlyContinue
                }
            }
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify frames summary returns timeline metrics..." -Context 'frames summary' -Args @($TracePath, 'frames', 'summary') -MustContain @('"data"', '"frame_count"')),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify frames summary supports frame-range window..." -Context 'frames summary frame-range' -Args @($TracePath, 'frames', 'summary', '--frame-range', '1:3') -MustContain @('"data"', '"time_window_source"') -Validate {
            param($result)
            $json = Parse-JsonOutput -Text $result.Text -Context 'frames summary frame-range'
            if ($json.meta.time_window_source -ne 'frame-range') {
                throw 'Expected frames summary frame-range meta.time_window_source=frame-range'
            }
            if ($json.meta.frame_range -ne '1:3') {
                throw 'Expected frames summary frame-range metadata echo'
            }
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify frame-range cannot be combined with explicit time window..." -Context 'frames summary frame-range conflict' -Args @($TracePath, 'frames', 'summary', '--frame-range', '1:3', '--time-start', '0') -MustContain @('"E1003"') -ExpectNonZero),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify frames slowest returns frame list..." -Context 'frames slowest' -Args @($TracePath, 'frames', 'slowest', '--limit', '3') -MustContain @('"data"', '"frame_index"'))
    )

    $postDetailCases = @(
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify cpu top returns scope list..." -Context 'cpu top' -Args @($TracePath, 'cpu', 'top', '--thread', 'GameThread', '--limit', '3') -MustContain @('"data"', '"scope_name"')),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify cpu stack returns trace-backed stack data..." -Context 'cpu stack' -Args @($TracePath, 'cpu', 'stack', '--frame-index', '1', '--thread', 'GameThread', '--limit', '10') -MustContain @('"data"', '"stack"', '"data_source"')),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify gpu top returns valid list (possibly empty)..." -Context 'gpu top' -Args @($TracePath, 'gpu', 'top', '--limit', '3') -MustContain @('"data"') -Validate {
            param($result)
            $json = Parse-JsonOutput -Text $result.Text -Context 'gpu top'
            if ($null -eq $json.data) {
                throw 'Expected gpu top response to include data array'
            }
            if ($json.data.Count -gt 3) {
                throw 'Expected gpu top result count <= limit'
            }
            foreach ($item in $json.data) {
                if ($null -eq $item.gpu_scope_name -or [string]::IsNullOrWhiteSpace([string]$item.gpu_scope_name)) {
                    throw 'Expected gpu_scope_name for each gpu top row when data is non-empty'
                }
            }
            if ($json.data.Count -gt 0) {
                Assert-Descending -Items $json.data -Property 'total_ms' -Context 'gpu top'
            }
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify threads waits returns trace-backed wait diagnostics..." -Context 'threads waits' -Args @($TracePath, 'threads', 'waits', '--frame-index', '1', '--limit', '3') -MustContain @('"data"', '"data_source"') -Validate {
            param($result)
            $json = Parse-JsonOutput -Text $result.Text -Context 'threads waits'
            if ($null -eq $json.data) {
                throw 'Expected threads waits response to include data array'
            }
            if ($json.data.Count -gt 3) {
                throw 'Expected threads waits result count <= limit'
            }
            foreach ($item in $json.data) {
                if ($null -eq $item.thread_id) {
                    throw 'Expected thread_id for each threads waits row when data is non-empty'
                }
                if ($null -eq $item.wait_ms) {
                    throw 'Expected wait_ms for each threads waits row when data is non-empty'
                }
            }
            if ($json.data.Count -gt 0) {
                Assert-Descending -Items $json.data -Property 'wait_ms' -Context 'threads waits'
            }
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify threads waits works without frame filter..." -Context 'threads waits no-frame' -Args @($TracePath, 'threads', 'waits', '--limit', '3') -MustContain @('"data"', '"data_source"')),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify tasks top returns trace-backed task diagnostics..." -Context 'tasks top' -Args @($TracePath, 'tasks', 'top', '--frame-index', '1', '--limit', '3') -MustContain @('"data"', '"data_source"') -Validate {
            param($result)
            $json = Parse-JsonOutput -Text $result.Text -Context 'tasks top'
            if ($null -eq $json.data) {
                throw 'Expected tasks top response to include data array'
            }
            if ($json.data.Count -gt 3) {
                throw 'Expected tasks top result count <= limit'
            }
            foreach ($item in $json.data) {
                if ($null -eq $item.task_id) {
                    throw 'Expected task_id for each tasks top row when data is non-empty'
                }
                if ($null -eq $item.queue_wait_ms) {
                    throw 'Expected queue_wait_ms for each tasks top row when data is non-empty'
                }
            }
            if ($json.data.Count -gt 0) {
                Assert-Descending -Items $json.data -Property 'queue_wait_ms' -Context 'tasks top'
            }
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify symbols resolve returns symbol mapping..." -Context 'symbols resolve' -Args @($TracePath, 'symbols', 'resolve', '--name', 'MoveActors') -Validate {
            param($result)
            $cpuTopResult = Invoke-InsightCli -Args @($TracePath, 'cpu', 'top', '--thread', 'GameThread', '--limit', '1')
            if ($cpuTopResult.ExitCode -ne 0) {
                throw 'Expected cpu top to succeed before symbols resolve validation'
            }
            $cpuTopJson = Parse-JsonOutput -Text $cpuTopResult.Text -Context 'cpu top for symbols resolve'
            if ($null -eq $cpuTopJson.data -or $cpuTopJson.data.Count -eq 0) {
                throw 'Expected cpu top to provide at least one scope for symbols resolve validation'
            }
            $scopeName = [string]$cpuTopJson.data[0].scope_name
            if ([string]::IsNullOrWhiteSpace($scopeName)) {
                throw 'Expected cpu top first row to include scope_name'
            }

            $resolveResult = Invoke-InsightCli -Args @($TracePath, 'symbols', 'resolve', '--name', $scopeName)
            if ($resolveResult.ExitCode -ne 0) {
                throw 'Expected symbols resolve to succeed for scope sampled from cpu top'
            }
            $resolveJson = Parse-JsonOutput -Text $resolveResult.Text -Context 'symbols resolve sampled scope'
            if ($null -eq $resolveJson.data) {
                throw 'Expected symbols resolve sampled response to contain data object'
            }
            if ([string]::IsNullOrWhiteSpace([string]$resolveJson.data.symbol)) {
                throw 'Expected symbols resolve sampled response to include symbol field'
            }
            if ([string]::IsNullOrWhiteSpace([string]$resolveJson.data.function)) {
                throw 'Expected symbols resolve sampled response to include function field'
            }
            if ($resolveJson.meta.data_source -ne 'trace') {
                throw 'Expected symbols resolve sampled response meta.data_source=trace'
            }

            $missingResult = Invoke-InsightCli -Args @($TracePath, 'symbols', 'resolve', '--name', 'DefinitelyMissingScope_123')
            if ($missingResult.ExitCode -ne 0) {
                throw 'Expected exit code 0 for missing symbols resolve'
            }
            $missingJson = Parse-JsonOutput -Text $missingResult.Text -Context 'symbols resolve missing'
            if ($null -eq $missingJson.data) {
                throw 'Expected symbols resolve missing response to contain data object'
            }
            if ($missingJson.meta.query_key -ne 'name' -or $missingJson.meta.query_value -ne 'DefinitelyMissingScope_123') {
                throw 'Expected symbols resolve missing response to include query_key/query_value metadata'
            }
            if ($missingResult.Text -match '"symbol"') {
                throw 'Expected missing symbols resolve response to omit symbol field'
            }
            if ($missingJson.meta.data_source -ne 'trace') {
                throw 'Expected symbols resolve missing response meta.data_source=trace'
            }
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify counters list returns discovered metadata..." -Context 'counters list' -Args @($TracePath, 'counters', 'list') -MustContain @('"data"') -Validate {
            param($result)
            $json = Parse-JsonOutput -Text $result.Text -Context 'counters list'
            if ($json.meta.data_source -ne 'trace') {
                throw 'Expected counters list meta.data_source=trace'
            }
            foreach ($counter in $json.data) {
                if ([string]::IsNullOrWhiteSpace([string]$counter.name)) {
                    throw 'Expected counter name in counters list rows'
                }
                if ([string]::IsNullOrWhiteSpace([string]$counter.type)) {
                    throw 'Expected counter type in counters list rows'
                }
                if ([string]::IsNullOrWhiteSpace([string]$counter.unit)) {
                    throw 'Expected counter unit in counters list rows'
                }
            }
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify counters series returns data array..." -Context 'counters series' -Args @($TracePath, 'counters', 'list') -MustContain @('"data"') -Validate {
            param($result)
            $catalog = Parse-JsonOutput -Text $result.Text -Context 'counters list for series'
            if ($null -eq $catalog.data -or $catalog.data.Count -eq 0) {
                return
            }

            $counterCandidate = $catalog.data | Where-Object { [int]$_.sample_count -gt 0 -and [string]$_.name -notmatch '[\(\)]' } | Select-Object -First 1
            if ($null -eq $counterCandidate) {
                $counterCandidate = $catalog.data | Where-Object { [int]$_.sample_count -gt 0 } | Select-Object -First 1
            }
            if ($null -eq $counterCandidate) {
                return
            }
            $counterName = [string]$counterCandidate.name
            $seriesResult = Invoke-InsightCli -Args @($TracePath, 'counters', 'series', '--name', $counterName, '--time-start', '0', '--time-end', '5000')
            if ($seriesResult.ExitCode -ne 0) {
                throw 'Expected counters series to succeed for discovered counter name'
            }
            $seriesJson = Parse-JsonOutput -Text $seriesResult.Text -Context 'counters series discovered counter'
            if ($seriesJson.meta.data_source -ne 'trace') {
                throw 'Expected counters series meta.data_source=trace'
            }
            if ($seriesJson.meta.counter_name -ne $counterName) {
                throw 'Expected counters series meta.counter_name to echo query name'
            }
            if ($seriesJson.data.Count -gt 1) {
                Assert-Ascending -Items $seriesJson.data -Property 'timestamp_ms' -Context 'counters series'
            }

            $seriesRangeResult = Invoke-InsightCli -Args @($TracePath, 'counters', 'series', '--name', $counterName, '--frame-range', '1:3')
            if ($seriesRangeResult.ExitCode -ne 0) {
                throw 'Expected counters series to succeed with frame-range'
            }
            $seriesRangeJson = Parse-JsonOutput -Text $seriesRangeResult.Text -Context 'counters series frame-range'
            if ($seriesRangeJson.meta.time_window_source -ne 'frame-range') {
                throw 'Expected counters series frame-range meta.time_window_source=frame-range'
            }
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify counters stats returns aggregate stats..." -Context 'counters stats' -Args @($TracePath, 'counters', 'list') -MustContain @('"data"') -Validate {
            param($result)
            $catalog = Parse-JsonOutput -Text $result.Text -Context 'counters list for stats'
            if ($null -eq $catalog.data -or $catalog.data.Count -eq 0) {
                return
            }

            $counterCandidate = $catalog.data | Where-Object { [int]$_.sample_count -gt 0 -and [string]$_.name -notmatch '[\(\)]' } | Select-Object -First 1
            if ($null -eq $counterCandidate) {
                $counterCandidate = $catalog.data | Where-Object { [int]$_.sample_count -gt 0 } | Select-Object -First 1
            }
            if ($null -eq $counterCandidate) {
                return
            }
            $counterName = [string]$counterCandidate.name
            $statsResult = Invoke-InsightCli -Args @($TracePath, 'counters', 'stats', '--name', $counterName)
            if ($statsResult.ExitCode -ne 0) {
                throw 'Expected counters stats to succeed for discovered counter name'
            }
            $statsJson = Parse-JsonOutput -Text $statsResult.Text -Context 'counters stats discovered counter'
            if ($statsJson.meta.data_source -ne 'trace') {
                throw 'Expected counters stats meta.data_source=trace'
            }
            if ($statsJson.data.counter_name -ne $counterName) {
                throw 'Expected counters stats to echo counter_name'
            }
            if ([double]$statsJson.data.sample_count -gt 0) {
                if ([double]$statsJson.data.min -gt [double]$statsJson.data.avg -or [double]$statsJson.data.avg -gt [double]$statsJson.data.max) {
                    throw 'Expected counters stats min <= avg <= max when sample_count > 0'
                }
            }
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify counters unknown name returns E2001..." -Context 'counters stats unknown name' -Args @($TracePath, 'counters', 'stats', '--name', 'UnknownCounter') -MustContain @('"E2001"', '"available_counters"') -ExpectNonZero)
    )

    $postMarksCases = @(
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify unknown command group/action returns E2001..." -Context 'unknown command group' -Args @($TracePath, 'unknown-group', 'summary') -MustContain @('"E2001"', '"group"', '"action"') -ExpectNonZero),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify known group unknown action returns E2001..." -Context 'known group unknown action' -Args @($TracePath, 'cpu', 'unknown-action') -MustContain @('"E2001"', '"group"', '"action"') -ExpectNonZero)
    )

    $detailCases = @(
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify frames detail behavior..." -Context 'frames detail behavior' -Args @($TracePath, 'frames', 'detail', '--frame-index', '1') -MustContain @('"data"', '"frame_index"') -Validate {
            param($result)
            $framesDetailInvalidResult = Invoke-InsightCli -Args @($TracePath, 'frames', 'detail', '--frame-index', 'abc')
            if ($framesDetailInvalidResult.ExitCode -eq 0) {
                throw 'Expected non-zero exit code for frames detail --frame-index abc'
            }
            if ($framesDetailInvalidResult.Text -notmatch '"E1003"') {
                throw "Expected E1003 for invalid frame-index. Output: $($framesDetailInvalidResult.Text)"
            }

            $framesDetailMissingResult = Invoke-InsightCli -Args @($TracePath, 'frames', 'detail', '--frame-index', '9999')
            if ($framesDetailMissingResult.ExitCode -ne 0) {
                throw 'Expected zero exit code for frames detail missing frame-index'
            }

            $framesDetailMissingJson = Parse-JsonOutput -Text $framesDetailMissingResult.Text -Context 'frames detail missing frame-index'
            if ($framesDetailMissingJson.meta.found -ne 'false') {
                throw 'Expected frames detail missing response to set meta.found=false'
            }
            if ($framesDetailMissingJson.meta.query_key -ne 'frame-index' -or $framesDetailMissingJson.meta.query_value -ne '9999') {
                throw 'Expected frames detail missing response to include query_key/query_value metadata'
            }
        })
    )

    $aggregateCases = @(
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify memory commands return trace-backed data..." -Context 'memory summary/peak/series/tags' -Args @($TracePath, 'memory', 'summary') -MustContain @('"data"') -Validate {
            param($result)
            $summaryJson = Parse-JsonOutput -Text $result.Text -Context 'memory summary'
            if ($summaryJson.meta.data_source -ne 'trace') {
                throw 'Expected memory summary meta.data_source=trace'
            }
            if ($summaryJson.meta.time_window_source -ne 'full') {
                throw 'Expected memory summary default time_window_source=full'
            }
            foreach ($field in @('min_bytes','max_bytes','avg_bytes','end_bytes')) {
                if ($null -eq $summaryJson.data.$field) {
                    throw "Expected memory summary field: $field"
                }
            }

            $memorySummaryRangeResult = Invoke-InsightCli -Args @($TracePath, 'memory', 'summary', '--frame-range', '1:3')
            if ($memorySummaryRangeResult.ExitCode -ne 0) {
                throw 'Expected zero exit code for memory summary frame-range'
            }
            $summaryRangeJson = Parse-JsonOutput -Text $memorySummaryRangeResult.Text -Context 'memory summary frame-range'
            if ($summaryRangeJson.meta.time_window_source -ne 'frame-range') {
                throw 'Expected memory summary frame-range meta.time_window_source=frame-range'
            }

            $memoryPeakResult = Invoke-InsightCli -Args @($TracePath, 'memory', 'peak')
            if ($memoryPeakResult.ExitCode -ne 0) {
                throw 'Expected zero exit code for memory peak'
            }
            $peakJson = Parse-JsonOutput -Text $memoryPeakResult.Text -Context 'memory peak'
            if ($peakJson.meta.data_source -ne 'trace') {
                throw 'Expected memory peak meta.data_source=trace'
            }
            if ($null -eq $peakJson.data.peak_bytes -or $null -eq $peakJson.data.peak_timestamp_ms) {
                throw 'Expected peak_bytes and peak_timestamp_ms in memory peak output'
            }

            $memorySeriesResult = Invoke-InsightCli -Args @($TracePath, 'memory', 'series', '--time-start', '0', '--time-end', '5000', '--limit', '3')
            if ($memorySeriesResult.ExitCode -ne 0) {
                throw 'Expected zero exit code for memory series'
            }
            $seriesJson = Parse-JsonOutput -Text $memorySeriesResult.Text -Context 'memory series'
            if ($seriesJson.meta.data_source -ne 'trace') {
                throw 'Expected memory series meta.data_source=trace'
            }
            if ($seriesJson.data.Count -gt 3) {
                throw 'Expected memory series count <= limit'
            }
            foreach ($item in $seriesJson.data) {
                if ($null -eq $item.timestamp_ms -or $null -eq $item.bytes) {
                    throw 'Expected timestamp_ms and bytes in memory series rows'
                }
            }

            $memoryTagsResult = Invoke-InsightCli -Args @($TracePath, 'memory', 'tags', '--limit', '3')
            if ($memoryTagsResult.ExitCode -ne 0) {
                throw 'Expected zero exit code for memory tags'
            }
            $tagsJson = Parse-JsonOutput -Text $memoryTagsResult.Text -Context 'memory tags'
            if ($tagsJson.meta.data_source -ne 'trace') {
                throw 'Expected memory tags meta.data_source=trace'
            }
            if ($tagsJson.data.Count -gt 3) {
                throw 'Expected memory tags count <= limit'
            }
            foreach ($item in $tagsJson.data) {
                if ([string]::IsNullOrWhiteSpace([string]$item.tag_name)) {
                    throw 'Expected tag_name in memory tags rows'
                }
                if ($null -eq $item.bytes -or $null -eq $item.percent_ratio) {
                    throw 'Expected bytes and percent_ratio in memory tags rows'
                }
            }
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify marks search/around return mark data..." -Context 'marks search/around' -Args @($TracePath, 'marks', 'search', '--keyword', 'load') -MustContain @('"data"') -Validate {
            param($result)
            $marksAroundResult = Invoke-InsightCli -Args @($TracePath, 'marks', 'around', '--timestamp', '120', '--window', '500', '--channel', 'Streaming', '--thread-id', '42')
            if ($marksAroundResult.ExitCode -ne 0) {
                throw 'Expected zero exit code for marks around command'
            }
            if ($marksAroundResult.Text -notmatch '"data"') {
                throw "Expected data in marks around output. Output: $($marksAroundResult.Text)"
            }
            if ($marksAroundResult.Text -notmatch '"filter_channel"' -or $marksAroundResult.Text -notmatch '"filter_thread_id"') {
                throw "Expected applied filter metadata in marks around output. Output: $($marksAroundResult.Text)"
            }
            $marksAroundJson = Parse-JsonOutput -Text $marksAroundResult.Text -Context 'marks around'
            if ($marksAroundJson.meta.data_source -ne 'trace') {
                throw 'Expected marks around meta.data_source=trace'
            }

            $marksSearchCaseSensitiveResult = Invoke-InsightCli -Args @($TracePath, 'marks', 'search', '--keyword', 'load', '--case-sensitive', '--exact', '--category', 'log', '--channel', 'Streaming', '--thread-id', '42')
            if ($marksSearchCaseSensitiveResult.ExitCode -ne 0) {
                throw 'Expected zero exit code for marks search case-sensitive exact'
            }
            if ($marksSearchCaseSensitiveResult.Text -notmatch '"data"') {
                throw "Expected data for marks search case-sensitive exact. Output: $($marksSearchCaseSensitiveResult.Text)"
            }
            if ($marksSearchCaseSensitiveResult.Text -notmatch '"filter_category"' -or $marksSearchCaseSensitiveResult.Text -notmatch '"filter_channel"' -or $marksSearchCaseSensitiveResult.Text -notmatch '"filter_thread_id"') {
                throw "Expected applied filter metadata in marks search output. Output: $($marksSearchCaseSensitiveResult.Text)"
            }

            $windowedSearchResult = Invoke-InsightCli -Args @($TracePath, 'marks', 'search', '--keyword', 'a', '--time-start', '0', '--time-end', '5000')
            if ($windowedSearchResult.ExitCode -ne 0) {
                throw 'Expected zero exit code for marks search with time window'
            }
            $windowedSearchJson = Parse-JsonOutput -Text $windowedSearchResult.Text -Context 'marks search with time window'
            if ($windowedSearchJson.meta.time_window_source -ne 'explicit' -or $windowedSearchJson.meta.time_window_start_ms -ne '0.000' -or $windowedSearchJson.meta.time_window_end_ms -ne '5000.000') {
                throw 'Expected marks search explicit time window metadata'
            }
            foreach ($row in $windowedSearchJson.data) {
                if ([double]$row.timestamp_ms -lt 0 -or [double]$row.timestamp_ms -ge 5000) {
                    throw 'Expected marks search rows to respect [time-start, time-end) window'
                }
            }

            $frameRangeSearchResult = Invoke-InsightCli -Args @($TracePath, 'marks', 'search', '--keyword', 'a', '--frame-range', '1:3')
            if ($frameRangeSearchResult.ExitCode -ne 0) {
                throw 'Expected zero exit code for marks search with frame-range'
            }
            $frameRangeSearchJson = Parse-JsonOutput -Text $frameRangeSearchResult.Text -Context 'marks search with frame-range'
            if ($frameRangeSearchJson.meta.time_window_source -ne 'frame-range' -or $frameRangeSearchJson.meta.frame_range -ne '1:3') {
                throw 'Expected marks search frame-range metadata'
            }
        })
    )

    $tailCases = @(
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify frames summary rejects invalid time window..." -Context 'frames summary invalid time window' -Args @($TracePath, 'frames', 'summary', '--time-start', '200', '--time-end', '100') -MustContain @('"E1003"') -ExpectNonZero -NoStep)
    )

    $orderedStepCases = @($preDetailCases + $detailCases + $postDetailCases + $aggregateCases + $postMarksCases)
    $orderedSilentCases = @($tailCases)

    $script:StepCounter = 0
    $script:TotalSteps = $orderedStepCases.Count
    Write-Host "Running normal-trace smoke: $TracePath"

    Invoke-SmokeCaseList -Cases $orderedStepCases
    Invoke-SmokeCaseList -Cases $orderedSilentCases
}

if (-not (Test-Path -Path $ExePath)) {
    throw "InsightCli executable not found: $ExePath"
}

if ($TracePaths.Count -eq 0) {
    $defaultTraceDir = Join-Path $PSScriptRoot 'Traces'
    $TracePaths = @(
        (Join-Path $defaultTraceDir '20260502_160828.utrace')
    )
}

foreach ($trace in $TracePaths) {
    if (-not (Test-Path -Path $trace)) {
        throw "Trace file not found: $trace"
    }
}

foreach ($trace in $TracePaths) {
    Invoke-NormalTraceSmoke -TracePath $trace
}

Write-Host "InsightCli output smoke checks passed for all traces."
