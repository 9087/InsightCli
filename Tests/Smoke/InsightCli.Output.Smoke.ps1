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

function Assert-TraceUnavailableChannelDisabled {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Text,
        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    if ($Text -notmatch '"code"\s*:\s*"E3001"') {
        throw "Expected $Context output to contain error code E3001"
    }
    if ($Text -notmatch '"data_source"\s*:\s*"unavailable"') {
        throw "Expected $Context output to contain data_source=unavailable"
    }
    if ($Text -notmatch '"unavailable_reason"\s*:\s*"channel_disabled"') {
        throw "Expected $Context output to contain unavailable_reason=channel_disabled"
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

            if ($json.data.duration_ms -is [string]) {
                throw 'Expected info summary duration_ms to be numeric JSON type, not string'
            }
            if ($json.data.trace_size_bytes -is [string]) {
                throw 'Expected info summary trace_size_bytes to be numeric JSON type, not string'
            }
            if ($null -ne $json.data.thread_count -and $json.data.thread_count -is [string]) {
                throw 'Expected info summary thread_count to be numeric or null, not string'
            }

            if ($null -ne $json.data.event_count) {
                throw 'Expected info summary event_count to be null when unavailable'
            }
            if ($null -ne $json.data.build_version) {
                throw 'Expected info summary build_version to be null when unavailable'
            }

            if ($null -eq $json.meta -or $null -eq $json.meta.unavailable_fields) {
                throw 'Expected info summary response to include meta.unavailable_fields'
            }

            $unavailable = @($json.meta.unavailable_fields)
            if ($unavailable.Count -lt 2) {
                throw 'Expected info summary meta.unavailable_fields to contain unavailable field names'
            }

            if (-not ($unavailable -contains 'event_count')) {
                throw 'Expected info summary meta.unavailable_fields to include event_count'
            }
            if (-not ($unavailable -contains 'build_version')) {
                throw 'Expected info summary meta.unavailable_fields to include build_version'
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
            elseif ($null -ne $json.data.end_timestamp) {
                throw 'Expected end_timestamp to be null when duration_ms == 0'
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
        ,(New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify frames slowest supports --max-rows truncation..." -Context 'frames slowest max-rows' -Args @($TracePath, 'frames', 'slowest', '--limit', '100', '--max-rows', '5') -MustContain @('"data"', '"truncated"') -Validate {
            param($result)
            $json = Parse-JsonOutput -Text $result.Text -Context 'frames slowest max-rows'
            if ($null -eq $json.data) {
                throw 'Expected frames slowest max-rows response to include data array'
            }
            if ($json.data.Count -ne 5) {
                throw 'Expected frames slowest --max-rows 5 to return exactly 5 rows'
            }
            if ($json.meta.truncated -ne $true) {
                throw 'Expected frames slowest --max-rows response meta.truncated=true'
            }
            if ([int]$json.meta.row_count_actual -lt 5) {
                throw 'Expected frames slowest --max-rows to include row_count_actual >= 5'
            }
        })
    )

    $postDetailCases = @(
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify cpu top returns scope list..." -Context 'cpu top' -Args @($TracePath, 'cpu', 'top', '--thread', 'GameThread', '--limit', '3') -MustContain @('"data"', '"scope_name"')),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify cpu stat-groups and stat-group filter..." -Context 'cpu stat-groups' -Args @($TracePath, 'cpu', 'stat-groups') -MustContain @('"data"', '"scope_count"', '"total_self_ms"') -Validate {
            param($result)
            $groupsJson = Parse-JsonOutput -Text $result.Text -Context 'cpu stat-groups'
            if ($null -eq $groupsJson.data) {
                throw 'Expected cpu stat-groups response to include data array'
            }
            if ($groupsJson.data.Count -lt 3) {
                throw 'Expected cpu stat-groups to return at least 3 groups'
            }

            $groupName = [string]$groupsJson.data[0].name
            if ([string]::IsNullOrWhiteSpace($groupName)) {
                throw 'Expected cpu stat-groups rows to include non-empty name'
            }

            $fullTopResult = Invoke-InsightCli -Args @($TracePath, 'cpu', 'top', '--limit', '20')
            if ($fullTopResult.ExitCode -ne 0) {
                throw 'Expected zero exit code for cpu top baseline in stat-group smoke'
            }
            $fullTopJson = Parse-JsonOutput -Text $fullTopResult.Text -Context 'cpu top baseline for stat-group'

            $filteredTopResult = Invoke-InsightCli -Args @($TracePath, 'cpu', 'top', '--limit', '20', '--stat-group', $groupName, '--frame-index', '1')
            if ($filteredTopResult.ExitCode -ne 0) {
                throw 'Expected zero exit code for cpu top stat-group filter'
            }
            $filteredTopJson = Parse-JsonOutput -Text $filteredTopResult.Text -Context 'cpu top stat-group filter'

            if ($filteredTopJson.meta.stat_group -ne $groupName.ToLower()) {
                throw 'Expected cpu top stat-group metadata echo in lowercase'
            }
            if ($filteredTopJson.data.Count -gt $fullTopJson.data.Count) {
                throw 'Expected cpu top --stat-group row count <= baseline cpu top row count'
            }
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify cpu top supports --fields projection..." -Context 'cpu top fields projection' -Args @($TracePath, 'cpu', 'top', '--thread', 'GameThread', '--limit', '3', '--fields', 'scope_name,self_ms,missing_field') -MustContain @('"scope_name"', '"self_ms"', '"fields_missing"') -Validate {
            param($result)
            $json = Parse-JsonOutput -Text $result.Text -Context 'cpu top fields projection'
            if ($null -eq $json.data) {
                throw 'Expected cpu top fields projection response to include data array'
            }

            $allowed = @('scope_name', 'self_ms')
            foreach ($row in $json.data) {
                $names = @($row.PSObject.Properties.Name)
                $unexpected = @($names | Where-Object { $allowed -notcontains $_ })
                if ($unexpected.Count -gt 0) {
                    throw "Expected cpu top --fields rows to contain only projected fields. Unexpected: $($unexpected -join ',')"
                }
            }

            $missing = @($json.meta.fields_missing)
            if (-not ($missing -contains 'missing_field')) {
                throw 'Expected cpu top --fields response to include missing_field in meta.fields_missing'
            }
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify cpu stack top-down view returns trace-backed stack data..." -Context 'cpu stack top-down' -Args @($TracePath, 'cpu', 'stack', '--frame-index', '1', '--thread', 'GameThread', '--view', 'top-down', '--limit', '10') -MustContain @('"data"', '"stack"', '"data_source"') -Validate {
            param($result)
            $json = Parse-JsonOutput -Text $result.Text -Context 'cpu stack top-down'
            if ($null -eq $json.data -or $json.data.Count -lt 1) {
                throw 'Expected cpu stack top-down response to include at least one row'
            }
            if ($json.meta.view -ne 'top-down') {
                throw 'Expected cpu stack top-down meta.view=top-down'
            }
            if ($null -eq $json.data[0].stack) {
                throw 'Expected cpu stack top-down row to include stack array'
            }
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify cpu stack bottom-up view reverses stack order..." -Context 'cpu stack bottom-up' -Args @($TracePath, 'cpu', 'stack', '--frame-index', '1', '--thread', 'GameThread', '--view', 'bottom-up', '--limit', '10') -MustContain @('"data"', '"stack"', '"data_source"') -Validate {
            param($result)
            $json = Parse-JsonOutput -Text $result.Text -Context 'cpu stack bottom-up'
            if ($null -eq $json.data -or $json.data.Count -lt 1) {
                throw 'Expected cpu stack bottom-up response to include at least one row'
            }
            if ($json.meta.view -ne 'bottom-up') {
                throw 'Expected cpu stack bottom-up meta.view=bottom-up'
            }
            if ($null -eq $json.data[0].stack) {
                throw 'Expected cpu stack bottom-up row to include stack array'
            }
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify cpu stack leaf view returns self-time aggregation..." -Context 'cpu stack leaf' -Args @($TracePath, 'cpu', 'stack', '--frame-index', '1', '--thread', 'GameThread', '--view', 'leaf', '--limit', '10') -MustContain @('"data"', '"stack"', '"data_source"') -Validate {
            param($result)
            $json = Parse-JsonOutput -Text $result.Text -Context 'cpu stack leaf'
            if ($null -eq $json.data -or $json.data.Count -lt 1) {
                throw 'Expected cpu stack leaf response to include at least one row'
            }
            if ($json.meta.view -ne 'leaf') {
                throw 'Expected cpu stack leaf meta.view=leaf'
            }
            if ($null -eq $json.data[0].stack) {
                throw 'Expected cpu stack leaf row to include stack array'
            }
            foreach ($leaf in $json.data[0].stack) {
                if ($null -eq $leaf.self_ms -or $null -eq $leaf.call_count) {
                    throw 'Expected self_ms and call_count in cpu stack leaf rows'
                }
            }
            if ($json.data[0].stack.Count -gt 0) {
                Assert-Descending -Items $json.data[0].stack -Property 'self_ms' -Context 'cpu stack leaf'
            }
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify cpu hot-functions returns self-time ranking..." -Context 'cpu hot-functions' -Args @($TracePath, 'cpu', 'hot-functions', '--thread', 'GameThread', '--limit', '3') -MustContain @('"data"', '"self_ms"', '"sort_by"') -Validate {
            param($result)
            $json = Parse-JsonOutput -Text $result.Text -Context 'cpu hot-functions'
            if ($null -eq $json.data) {
                throw 'Expected cpu hot-functions response to include data array'
            }
            if ($json.data.Count -gt 3) {
                throw 'Expected cpu hot-functions result count <= limit'
            }
            if ($json.meta.sort_by -ne 'self_ms_desc') {
                throw 'Expected cpu hot-functions meta.sort_by=self_ms_desc'
            }
            if ($json.data.Count -gt 0) {
                foreach ($item in $json.data) {
                    if ($null -eq $item.scope_name -or [string]::IsNullOrWhiteSpace([string]$item.scope_name)) {
                        throw 'Expected scope_name in cpu hot-functions rows'
                    }
                    if ($null -eq $item.self_ms) {
                        throw 'Expected self_ms in cpu hot-functions rows'
                    }
                }
                Assert-Descending -Items $json.data -Property 'self_ms' -Context 'cpu hot-functions'
            }
        }),
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
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify gpu passes enumerates frame pass list..." -Context 'gpu passes' -Args @($TracePath, 'gpu', 'passes', '--frame-index', '120') -MustContain @('"data"', '"data_source"') -Validate {
            param($result)
            $json = Parse-JsonOutput -Text $result.Text -Context 'gpu passes'
            if ($null -eq $json.data) {
                throw 'Expected gpu passes response to include data array'
            }
            if ($json.meta.data_source -ne 'trace') {
                throw 'Expected gpu passes meta.data_source=trace'
            }
            if ($json.meta.frame_index -ne '120') {
                throw 'Expected gpu passes meta.frame_index=120'
            }

            if ($json.data.Count -gt 0) {
                foreach ($item in $json.data) {
                    if ($null -eq $item.pass -or [string]::IsNullOrWhiteSpace([string]$item.pass)) {
                        throw 'Expected pass in gpu passes rows'
                    }
                    if ($null -eq $item.gpu_ms) {
                        throw 'Expected gpu_ms in gpu passes rows'
                    }
                    if ($null -eq $item.draw_call_count) {
                        throw 'Expected draw_call_count in gpu passes rows'
                    }
                }

                Assert-Descending -Items $json.data -Property 'gpu_ms' -Context 'gpu passes'
            }
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify rhi summary returns draw-call metrics..." -Context 'rhi summary' -Args @($TracePath, 'rhi', 'summary', '--frame-index', '1') -MustContain @('"data"', '"draw_call_count"') -Validate {
            param($result)
            $json = Parse-JsonOutput -Text $result.Text -Context 'rhi summary'
            if ($null -eq $json.data) {
                throw 'Expected rhi summary response to include data object'
            }
            foreach ($field in @('draw_call_count', 'primitive_count', 'triangle_count', 'rhi_thread_ms')) {
                if ($null -eq $json.data.$field) {
                    throw "Expected rhi summary field: $field"
                }
            }
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify rhi drawcalls returns sorted rows..." -Context 'rhi drawcalls' -Args @($TracePath, 'rhi', 'drawcalls', '--limit', '3') -MustContain @('"data"') -Validate {
            param($result)
            $json = Parse-JsonOutput -Text $result.Text -Context 'rhi drawcalls'
            if ($null -eq $json.data) {
                throw 'Expected rhi drawcalls response to include data array'
            }
            if ($json.data.Count -gt 3) {
                throw 'Expected rhi drawcalls count <= limit'
            }
            foreach ($item in $json.data) {
                if ([string]::IsNullOrWhiteSpace([string]$item.render_target)) {
                    throw 'Expected render_target in rhi drawcalls rows'
                }
                if ($null -eq $item.draw_call_count -or $null -eq $item.gpu_ms) {
                    throw 'Expected draw_call_count and gpu_ms in rhi drawcalls rows'
                }
            }
            if ($json.data.Count -gt 0) {
                Assert-Descending -Items $json.data -Property 'draw_call_count' -Context 'rhi drawcalls'
            }
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify slate top-widgets reports channel-disabled trace unavailability..." -Context 'slate top-widgets' -Args @($TracePath, 'slate', 'top-widgets', '--by', 'paint', '--limit', '3') -AllowExitCodes @(10) -Validate {
            param($result)
            Assert-TraceUnavailableChannelDisabled -Text $result.Text -Context 'slate top-widgets'
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify slate paint-cost reports channel-disabled trace unavailability..." -Context 'slate paint-cost' -Args @($TracePath, 'slate', 'paint-cost', '--frame-index', '1') -AllowExitCodes @(10) -Validate {
            param($result)
            Assert-TraceUnavailableChannelDisabled -Text $result.Text -Context 'slate paint-cost'
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify slate invalidation-rate reports channel-disabled trace unavailability..." -Context 'slate invalidation-rate' -Args @($TracePath, 'slate', 'invalidation-rate', '--time-start', '0', '--time-end', '1000') -AllowExitCodes @(10) -Validate {
            param($result)
            Assert-TraceUnavailableChannelDisabled -Text $result.Text -Context 'slate invalidation-rate'
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify anim top-actors reports channel-disabled trace unavailability..." -Context 'anim top-actors' -Args @($TracePath, 'anim', 'top-actors', '--limit', '3') -AllowExitCodes @(10) -Validate {
            param($result)
            Assert-TraceUnavailableChannelDisabled -Text $result.Text -Context 'anim top-actors'
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify anim graph reports channel-disabled trace unavailability..." -Context 'anim graph' -Args @($TracePath, 'anim', 'graph', '--actor', 'Character') -AllowExitCodes @(10) -Validate {
            param($result)
            Assert-TraceUnavailableChannelDisabled -Text $result.Text -Context 'anim graph'
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify anim skinning reports channel-disabled trace unavailability..." -Context 'anim skinning' -Args @($TracePath, 'anim', 'skinning', '--limit', '3') -AllowExitCodes @(10) -Validate {
            param($result)
            Assert-TraceUnavailableChannelDisabled -Text $result.Text -Context 'anim skinning'
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify io summary returns read totals and sync/async split..." -Context 'io summary' -Args @($TracePath, 'io', 'summary') -MustContain @('"data"') -Validate {
            param($result)
            $json = Parse-JsonOutput -Text $result.Text -Context 'io summary'
            if ($null -eq $json.data) {
                throw 'Expected io summary response to include data object'
            }
            foreach ($field in @('read_count', 'total_read_bytes', 'sync_read_count', 'async_read_count', 'sync_read_ratio', 'async_read_ratio')) {
                if ($null -eq $json.data.$field) {
                    throw "Expected io summary field: $field"
                }
            }
            if ([string]::IsNullOrWhiteSpace([string]$json.meta.channel_state)) {
                throw 'Expected io summary meta.channel_state'
            }
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify io slowest-reads respects limit and sort order..." -Context 'io slowest-reads' -Args @($TracePath, 'io', 'slowest-reads', '--limit', '3') -MustContain @('"data"') -Validate {
            param($result)
            $json = Parse-JsonOutput -Text $result.Text -Context 'io slowest-reads'
            if ($null -eq $json.data) {
                throw 'Expected io slowest-reads response to include data array'
            }
            if ($json.data.Count -gt 3) {
                throw 'Expected io slowest-reads count <= limit'
            }
            foreach ($item in $json.data) {
                if ([string]::IsNullOrWhiteSpace([string]$item.file_path)) {
                    throw 'Expected file_path in io slowest-reads rows'
                }
                if ($null -eq $item.duration_ms -or $null -eq $item.actual_size_bytes) {
                    throw 'Expected duration_ms and actual_size_bytes in io slowest-reads rows'
                }
            }
            if ($json.data.Count -gt 0) {
                Assert-Descending -Items $json.data -Property 'duration_ms' -Context 'io slowest-reads'
            }
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify io top-files aggregates file hotspots..." -Context 'io top-files' -Args @($TracePath, 'io', 'top-files', '--limit', '3') -MustContain @('"data"') -Validate {
            param($result)
            $json = Parse-JsonOutput -Text $result.Text -Context 'io top-files'
            if ($null -eq $json.data) {
                throw 'Expected io top-files response to include data array'
            }
            if ($json.data.Count -gt 3) {
                throw 'Expected io top-files count <= limit'
            }
            foreach ($item in $json.data) {
                if ([string]::IsNullOrWhiteSpace([string]$item.file_path)) {
                    throw 'Expected file_path in io top-files rows'
                }
                if ($null -eq $item.read_count -or $null -eq $item.read_bytes) {
                    throw 'Expected read_count and read_bytes in io top-files rows'
                }
            }
            if ($json.data.Count -gt 0) {
                Assert-Descending -Items $json.data -Property 'read_bytes' -Context 'io top-files'
            }
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify net summary reports channel-disabled trace unavailability..." -Context 'net summary' -Args @($TracePath, 'net', 'summary') -AllowExitCodes @(10) -Validate {
            param($result)
            Assert-TraceUnavailableChannelDisabled -Text $result.Text -Context 'net summary'
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify net top-actors reports channel-disabled trace unavailability..." -Context 'net top-actors' -Args @($TracePath, 'net', 'top-actors', '--limit', '3') -AllowExitCodes @(10) -Validate {
            param($result)
            Assert-TraceUnavailableChannelDisabled -Text $result.Text -Context 'net top-actors'
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify net top-rpcs reports channel-disabled trace unavailability..." -Context 'net top-rpcs' -Args @($TracePath, 'net', 'top-rpcs', '--limit', '3') -AllowExitCodes @(10) -Validate {
            param($result)
            Assert-TraceUnavailableChannelDisabled -Text $result.Text -Context 'net top-rpcs'
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify net bandwidth-series reports channel-disabled trace unavailability..." -Context 'net bandwidth-series' -Args @($TracePath, 'net', 'bandwidth-series', '--time-start', '0', '--time-end', '1000') -AllowExitCodes @(10) -Validate {
            param($result)
            Assert-TraceUnavailableChannelDisabled -Text $result.Text -Context 'net bandwidth-series'
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify niagara top-systems reports channel-disabled trace unavailability..." -Context 'niagara top-systems' -Args @($TracePath, 'niagara', 'top-systems', '--limit', '3') -AllowExitCodes @(10) -Validate {
            param($result)
            Assert-TraceUnavailableChannelDisabled -Text $result.Text -Context 'niagara top-systems'
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify niagara emitter-cost reports channel-disabled trace unavailability..." -Context 'niagara emitter-cost' -Args @($TracePath, 'niagara', 'emitter-cost', '--system', 'Niagara') -AllowExitCodes @(10) -Validate {
            param($result)
            Assert-TraceUnavailableChannelDisabled -Text $result.Text -Context 'niagara emitter-cost'
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify physics summary reports channel-disabled trace unavailability..." -Context 'physics summary' -Args @($TracePath, 'physics', 'summary', '--frame-index', '1') -AllowExitCodes @(10) -Validate {
            param($result)
            Assert-TraceUnavailableChannelDisabled -Text $result.Text -Context 'physics summary'
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify physics solver-stages reports channel-disabled trace unavailability..." -Context 'physics solver-stages' -Args @($TracePath, 'physics', 'solver-stages') -AllowExitCodes @(10) -Validate {
            param($result)
            Assert-TraceUnavailableChannelDisabled -Text $result.Text -Context 'physics solver-stages'
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify physics top-bodies reports channel-disabled trace unavailability..." -Context 'physics top-bodies' -Args @($TracePath, 'physics', 'top-bodies', '--limit', '3') -AllowExitCodes @(10) -Validate {
            param($result)
            Assert-TraceUnavailableChannelDisabled -Text $result.Text -Context 'physics top-bodies'
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify shaders compile-events reports channel-disabled trace unavailability..." -Context 'shaders compile-events' -Args @($TracePath, 'shaders', 'compile-events', '--limit', '3') -AllowExitCodes @(10) -Validate {
            param($result)
            Assert-TraceUnavailableChannelDisabled -Text $result.Text -Context 'shaders compile-events'
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify shaders pso-cache-misses reports channel-disabled trace unavailability..." -Context 'shaders pso-cache-misses' -Args @($TracePath, 'shaders', 'pso-cache-misses') -AllowExitCodes @(10) -Validate {
            param($result)
            Assert-TraceUnavailableChannelDisabled -Text $result.Text -Context 'shaders pso-cache-misses'
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify threads waits returns trace-backed wait diagnostics..." -Context 'threads waits' -Args @($TracePath, 'threads', 'waits', '--frame-index', '1', '--limit', '3') -MustContain @('"data"', '"data_source"') -Validate {
            param($result)
            $json = Parse-JsonOutput -Text $result.Text -Context 'threads waits'
            if ($null -eq $json.data) {
                throw 'Expected threads waits response to include data array'
            }
            if ($json.meta.data_source -ne 'context_switch_heuristic') {
                throw 'Expected threads waits meta.data_source=context_switch_heuristic'
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
                if ($null -eq $item.blocker_overlap_ratio) {
                    throw 'Expected blocker_overlap_ratio for each threads waits row when data is non-empty'
                }
                if ([string]$item.wait_type -eq 'NotRunning') {
                    throw 'Expected wait_type to be classified, not NotRunning'
                }
            }
            if ($json.data.Count -gt 0) {
                Assert-Descending -Items $json.data -Property 'wait_ms' -Context 'threads waits'
            }
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify threads waits works without frame filter..." -Context 'threads waits no-frame' -Args @($TracePath, 'threads', 'waits', '--limit', '3') -MustContain @('"data"', '"data_source"')),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify threads wait-chain returns chain diagnostics..." -Context 'threads wait-chain' -Args @($TracePath, 'threads', 'wait-chain', '--thread', 'GameThread', '--depth', '4', '--time-start', '0', '--time-end', '5000') -MustContain @('"data"') -Validate {
            param($result)
            $json = Parse-JsonOutput -Text $result.Text -Context 'threads wait-chain'
            if ($null -eq $json.data) {
                throw 'Expected threads wait-chain response to include data array'
            }
            if ($json.meta.data_source -ne 'context_switch_heuristic') {
                throw 'Expected threads wait-chain meta.data_source=context_switch_heuristic'
            }
            if ($json.meta.time_window_source -ne 'explicit') {
                throw 'Expected threads wait-chain explicit time window metadata'
            }
            if ([string]::IsNullOrWhiteSpace([string]$json.meta.max_chain_depth_used)) {
                throw 'Expected threads wait-chain meta.max_chain_depth_used'
            }

            foreach ($item in $json.data) {
                if ($null -eq $item.chain -or $item.chain.Count -lt 1) {
                    throw 'Expected non-empty chain array in threads wait-chain rows'
                }
                if ($null -eq $item.wait_ms) {
                    throw 'Expected wait_ms in threads wait-chain rows'
                }
                if ($null -eq $item.blocker_overlap_ratio) {
                    throw 'Expected blocker_overlap_ratio in threads wait-chain rows'
                }
                if ([string]::IsNullOrWhiteSpace([string]$item.chain_status)) {
                    throw 'Expected chain_status in threads wait-chain rows'
                }
            }

            if ($json.data.Count -gt 0) {
                Assert-Descending -Items $json.data -Property 'wait_ms' -Context 'threads wait-chain'
            }
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify tasks top returns trace-backed task diagnostics..." -Context 'tasks top' -Args @($TracePath, 'tasks', 'top', '--frame-index', '1', '--limit', '3') -MustContain @('"data"', '"data_source"') -Validate {
            param($result)
            $json = Parse-JsonOutput -Text $result.Text -Context 'tasks top'
            if ($null -eq $json.data) {
                throw 'Expected tasks top response to include data array'
            }
            if ($json.data.Count -gt 3) {
                throw 'Expected tasks top result count <= limit'
            }
            if ($json.meta.algorithm -ne 'dag_longest_path') {
                throw 'Expected tasks top meta.algorithm=dag_longest_path'
            }
            foreach ($item in $json.data) {
                if ($null -eq $item.task_id) {
                    throw 'Expected task_id for each tasks top row when data is non-empty'
                }
                if ($null -eq $item.queue_wait_ms) {
                    throw 'Expected queue_wait_ms for each tasks top row when data is non-empty'
                }
                if ([double]$item.critical_path_ms -lt [double]$item.run_ms) {
                    throw 'Expected critical_path_ms >= run_ms for each tasks top row'
                }
            }
            if ($json.data.Count -gt 0 -and -not ($json.data | Where-Object { [int]$_.critical_path_depth -ge 1 })) {
                throw 'Expected at least one tasks top row with critical_path_depth >= 1'
            }
            if ($json.data.Count -gt 0) {
                Assert-Descending -Items $json.data -Property 'queue_wait_ms' -Context 'tasks top'
            }
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify tasks critical-path returns path diagnostics..." -Context 'tasks critical-path' -Args @($TracePath, 'tasks', 'critical-path', '--frame-index', '1', '--top', '2') -MustContain @('"data"', '"data_source"') -Validate {
            param($result)
            $json = Parse-JsonOutput -Text $result.Text -Context 'tasks critical-path'
            if ($null -eq $json.data) {
                throw 'Expected tasks critical-path response to include data array'
            }
            if ($json.meta.data_source -ne 'trace') {
                throw 'Expected tasks critical-path meta.data_source=trace'
            }
            if ($json.meta.algorithm -ne 'dag_longest_path') {
                throw 'Expected tasks critical-path meta.algorithm=dag_longest_path'
            }
            if ($json.meta.frame_index -ne '1') {
                throw 'Expected tasks critical-path meta.frame_index=1'
            }
            if ($json.data.Count -gt 2) {
                throw 'Expected tasks critical-path count <= top'
            }

            if ($json.data.Count -gt 0) {
                foreach ($item in $json.data) {
                    if ($null -eq $item.path -or $item.path.Count -lt 1) {
                        throw 'Expected non-empty path array in tasks critical-path rows'
                    }
                    if ($null -eq $item.total_duration_ms -or $null -eq $item.node_count) {
                        throw 'Expected total_duration_ms and node_count in tasks critical-path rows'
                    }

                    foreach ($node in $item.path) {
                        if ($null -eq $node.task_name -or [string]::IsNullOrWhiteSpace([string]$node.task_name)) {
                            throw 'Expected task_name in tasks critical-path nodes'
                        }
                        if ($null -eq $node.thread -or [string]::IsNullOrWhiteSpace([string]$node.thread)) {
                            throw 'Expected thread in tasks critical-path nodes'
                        }
                        if ($null -eq $node.start_ms -or $null -eq $node.duration_ms) {
                            throw 'Expected start_ms and duration_ms in tasks critical-path nodes'
                        }
                        if ($null -eq $node.waits_for) {
                            throw 'Expected waits_for array in tasks critical-path nodes'
                        }
                    }
                }

                Assert-Descending -Items $json.data -Property 'total_duration_ms' -Context 'tasks critical-path'
            }
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify loadtime summary returns aggregate metrics..." -Context 'loadtime summary' -Args @($TracePath, 'loadtime', 'summary') -MustContain @('"data"', '"package_count"', '"total_load_ms"') -Validate {
            param($result)
            $json = Parse-JsonOutput -Text $result.Text -Context 'loadtime summary'
            if ($null -eq $json.data.package_count) {
                throw 'Expected loadtime summary to include package_count'
            }
            if ($null -eq $json.meta -or [string]::IsNullOrWhiteSpace([string]$json.meta.channel_state)) {
                throw 'Expected loadtime summary meta.channel_state'
            }
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify loadtime packages supports limit and sort-by..." -Context 'loadtime packages' -Args @($TracePath, 'loadtime', 'packages', '--limit', '5', '--sort-by', 'serialize') -MustContain @('"data"', '"sort_by"') -Validate {
            param($result)
            $json = Parse-JsonOutput -Text $result.Text -Context 'loadtime packages'
            if ($json.meta.sort_by -ne 'serialize') {
                throw 'Expected loadtime packages meta.sort_by=serialize'
            }
            if ($json.data.Count -gt 5) {
                throw 'Expected loadtime packages count <= limit'
            }
            foreach ($item in $json.data) {
                if ([string]::IsNullOrWhiteSpace([string]$item.package_name)) {
                    throw 'Expected package_name for each loadtime packages row'
                }
            }
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify loadtime slowest returns sorted package rows..." -Context 'loadtime slowest' -Args @($TracePath, 'loadtime', 'slowest', '--limit', '3') -MustContain @('"data"') -Validate {
            param($result)
            $json = Parse-JsonOutput -Text $result.Text -Context 'loadtime slowest'
            if ($json.data.Count -gt 3) {
                throw 'Expected loadtime slowest count <= limit'
            }
            if ($json.data.Count -gt 0) {
                foreach ($item in $json.data) {
                    if ($null -eq $item.total_load_ms) {
                        throw 'Expected total_load_ms in loadtime slowest rows when data is non-empty'
                    }
                }
                Assert-Descending -Items $json.data -Property 'total_load_ms' -Context 'loadtime slowest'
            }
            elseif ([string]::IsNullOrWhiteSpace([string]$json.meta.channel_state)) {
                throw 'Expected channel_state metadata for empty loadtime slowest response'
            }
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify loadtime timeline supports explicit time window..." -Context 'loadtime timeline' -Args @($TracePath, 'loadtime', 'timeline', '--time-start', '0', '--time-end', '5000') -MustContain @('"data"', '"time_window_source"') -Validate {
            param($result)
            $json = Parse-JsonOutput -Text $result.Text -Context 'loadtime timeline'
            if ($json.meta.time_window_source -ne 'explicit') {
                throw 'Expected loadtime timeline meta.time_window_source=explicit'
            }
            foreach ($item in $json.data) {
                if ($null -eq $item.start_ms -or $null -eq $item.end_ms) {
                    throw 'Expected start_ms/end_ms in loadtime timeline rows'
                }
            }
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify gc summary returns structured aggregation..." -Context 'gc summary' -Args @($TracePath, 'gc', 'summary') -MustContain @('"data"', '"gc_count"', '"source"') -Validate {
            param($result)
            $json = Parse-JsonOutput -Text $result.Text -Context 'gc summary'
            if ($json.meta.source -ne 'cpu_scope_pattern') {
                throw 'Expected gc summary meta.source=cpu_scope_pattern'
            }
            if ($null -eq $json.data.avg_gc_ms -or $null -eq $json.data.max_gc_ms) {
                throw 'Expected gc summary to include avg_gc_ms and max_gc_ms'
            }
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify gc longest respects limit and ordering..." -Context 'gc longest' -Args @($TracePath, 'gc', 'longest', '--limit', '3') -MustContain @('"data"', '"duration_ms"', '"source"') -Validate {
            param($result)
            $json = Parse-JsonOutput -Text $result.Text -Context 'gc longest'
            if ($json.data.Count -gt 3) {
                throw 'Expected gc longest count <= limit'
            }
            if ($json.data.Count -gt 0) {
                Assert-Descending -Items $json.data -Property 'duration_ms' -Context 'gc longest'
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
            $framesDetailThreadBreakdownResult = Invoke-InsightCli -Args @($TracePath, 'frames', 'detail', '--frame-index', '1', '--breakdown', 'thread')
            if ($framesDetailThreadBreakdownResult.ExitCode -ne 0) {
                throw 'Expected zero exit code for frames detail thread breakdown'
            }
            $framesDetailThreadJson = Parse-JsonOutput -Text $framesDetailThreadBreakdownResult.Text -Context 'frames detail thread breakdown'
            if ($framesDetailThreadJson.meta.breakdown -ne 'thread') {
                throw 'Expected frames detail thread breakdown meta.breakdown=thread'
            }
            if ($null -eq $framesDetailThreadJson.data.breakdown) {
                throw 'Expected frames detail thread breakdown to include data.breakdown'
            }
            foreach ($item in $framesDetailThreadJson.data.breakdown) {
                if ([string]::IsNullOrWhiteSpace([string]$item.bucket)) {
                    throw 'Expected bucket in frames detail thread breakdown rows'
                }
                if ($null -eq $item.ms -or $null -eq $item.ratio) {
                    throw 'Expected ms and ratio in frames detail thread breakdown rows'
                }
            }

            $framesDetailStatgroupBreakdownResult = Invoke-InsightCli -Args @($TracePath, 'frames', 'detail', '--frame-index', '1', '--breakdown', 'statgroup')
            if ($framesDetailStatgroupBreakdownResult.ExitCode -ne 0) {
                throw 'Expected zero exit code for frames detail statgroup breakdown'
            }
            $framesDetailStatgroupJson = Parse-JsonOutput -Text $framesDetailStatgroupBreakdownResult.Text -Context 'frames detail statgroup breakdown'
            if ($framesDetailStatgroupJson.meta.breakdown -ne 'statgroup') {
                throw 'Expected frames detail statgroup breakdown meta.breakdown=statgroup'
            }
            if ($null -eq $framesDetailStatgroupJson.data.breakdown) {
                throw 'Expected frames detail statgroup breakdown to include data.breakdown'
            }
            foreach ($item in $framesDetailStatgroupJson.data.breakdown) {
                if ([string]::IsNullOrWhiteSpace([string]$item.bucket)) {
                    throw 'Expected bucket in frames detail statgroup breakdown rows'
                }
                if ($null -eq $item.ms -or $null -eq $item.ratio) {
                    throw 'Expected ms and ratio in frames detail statgroup breakdown rows'
                }
            }

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

            $memoryDiffResult = Invoke-InsightCli -Args @($TracePath, 'memory', 'diff', '--t1', '0', '--t2', '5', '--limit', '3')
            if ($memoryDiffResult.ExitCode -ne 0) {
                throw 'Expected zero exit code for memory diff'
            }
            $diffJson = Parse-JsonOutput -Text $memoryDiffResult.Text -Context 'memory diff'
            if ($diffJson.data.Count -gt 3) {
                throw 'Expected memory diff count <= limit'
            }
            if ([double]$diffJson.meta.t1_sec -ne 0 -or [double]$diffJson.meta.t2_sec -ne 5) {
                throw 'Expected memory diff metadata to echo t1_sec/t2_sec values'
            }
            if ($diffJson.meta.data_source -eq 'trace') {
                foreach ($item in $diffJson.data) {
                    if ([string]::IsNullOrWhiteSpace([string]$item.tag_name)) {
                        throw 'Expected tag_name in memory diff rows'
                    }
                    if ($null -eq $item.delta_bytes -or $null -eq $item.delta_alloc_count) {
                        throw 'Expected delta_bytes and delta_alloc_count in memory diff rows'
                    }
                }
                if ($diffJson.data.Count -gt 0) {
                    Assert-Descending -Items $diffJson.data -Property 'delta_bytes' -Context 'memory diff'
                }
            }
            elseif ([string]::IsNullOrWhiteSpace([string]$diffJson.meta.warning)) {
                throw 'Expected warning metadata when memory diff data_source is unavailable'
            }

            $memoryAllocTopResult = Invoke-InsightCli -Args @($TracePath, 'memory', 'alloc-top', '--limit', '3', '--by', 'tag')
            if ($memoryAllocTopResult.ExitCode -ne 0) {
                throw 'Expected zero exit code for memory alloc-top --by tag'
            }
            $allocTopJson = Parse-JsonOutput -Text $memoryAllocTopResult.Text -Context 'memory alloc-top'
            if ($allocTopJson.meta.by -ne 'tag') {
                throw 'Expected memory alloc-top meta.by=tag'
            }
            if ($allocTopJson.data.Count -gt 3) {
                throw 'Expected memory alloc-top count <= limit'
            }
            if ($allocTopJson.meta.data_source -eq 'trace') {
                foreach ($item in $allocTopJson.data) {
                    if ([string]::IsNullOrWhiteSpace([string]$item.tag_name)) {
                        throw 'Expected tag_name in memory alloc-top rows'
                    }
                    if ($null -eq $item.bytes -or $null -eq $item.sample_count) {
                        throw 'Expected bytes and sample_count in memory alloc-top rows'
                    }
                }
                if ($allocTopJson.data.Count -gt 0) {
                    Assert-Descending -Items $allocTopJson.data -Property 'bytes' -Context 'memory alloc-top'
                }
            }
            elseif ([string]::IsNullOrWhiteSpace([string]$allocTopJson.meta.warning)) {
                throw 'Expected warning metadata when memory alloc-top data_source is unavailable'
            }

            $memoryLeakResult = Invoke-InsightCli -Args @($TracePath, 'memory', 'leak-suspect', '--window', '5', '--limit', '3')
            if ($memoryLeakResult.ExitCode -ne 0) {
                throw 'Expected zero exit code for memory leak-suspect'
            }
            $leakJson = Parse-JsonOutput -Text $memoryLeakResult.Text -Context 'memory leak-suspect'
            if ($leakJson.data.Count -gt 3) {
                throw 'Expected memory leak-suspect count <= limit'
            }
            if ($leakJson.meta.data_source -eq 'trace') {
                foreach ($item in $leakJson.data) {
                    if ([string]::IsNullOrWhiteSpace([string]$item.tag_name)) {
                        throw 'Expected tag_name in memory leak-suspect rows'
                    }
                    if ($null -eq $item.growth_bytes -or $null -eq $item.sample_count) {
                        throw 'Expected growth_bytes and sample_count in memory leak-suspect rows'
                    }
                }
                if ($leakJson.data.Count -gt 0) {
                    Assert-Descending -Items $leakJson.data -Property 'growth_bytes' -Context 'memory leak-suspect'
                }
            }
            elseif ([string]::IsNullOrWhiteSpace([string]$leakJson.meta.warning)) {
                throw 'Expected warning metadata when memory leak-suspect data_source is unavailable'
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
        }),
        (New-SmokeCase -Message "[$([IO.Path]::GetFileName($TracePath))] Verify marks regions and region-slice return structured outputs..." -Context 'marks regions/region-slice' -Args @($TracePath, 'marks', 'regions') -MustContain @('"data"') -Validate {
            param($result)
            $regionsJson = Parse-JsonOutput -Text $result.Text -Context 'marks regions'
            if ($null -eq $regionsJson.data) {
                throw 'Expected marks regions response to include data array'
            }

            if ($regionsJson.data.Count -gt 0) {
                $targetRegion = $regionsJson.data[0]
                if ([string]::IsNullOrWhiteSpace([string]$targetRegion.name)) {
                    throw 'Expected marks regions rows to include non-empty name'
                }
                foreach ($field in @('start_ms', 'end_ms', 'duration_ms', 'incomplete')) {
                    if ($null -eq $targetRegion.$field) {
                        throw "Expected marks regions row field: $field"
                    }
                }

                $sliceResult = Invoke-InsightCli -Args @($TracePath, 'marks', 'region-slice', '--name', [string]$targetRegion.name)
                if ($sliceResult.ExitCode -ne 0) {
                    throw 'Expected zero exit code for marks region-slice'
                }
                $sliceJson = Parse-JsonOutput -Text $sliceResult.Text -Context 'marks region-slice'
                if ($null -eq $sliceJson.data) {
                    throw 'Expected marks region-slice response to include data object'
                }
                if ([string]::IsNullOrWhiteSpace([string]$sliceJson.data.name)) {
                    throw 'Expected marks region-slice data.name'
                }
                foreach ($field in @('frame_count', 'frame_time_avg_ms', 'frame_time_max_ms', 'cpu_total_self_ms', 'memory_delta_bytes')) {
                    if ($null -eq $sliceJson.data.$field) {
                        throw "Expected marks region-slice metric: $field"
                    }
                }
            }
            else {
                $missingSliceResult = Invoke-InsightCli -Args @($TracePath, 'marks', 'region-slice', '--name', '__missing_region__')
                if ($missingSliceResult.ExitCode -ne 0) {
                    throw 'Expected zero exit code for marks region-slice not_found path'
                }
                $missingSliceJson = Parse-JsonOutput -Text $missingSliceResult.Text -Context 'marks region-slice missing'
                if ([string]$missingSliceJson.meta.found -ne 'false') {
                    throw 'Expected marks region-slice missing response meta.found=false'
                }
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

function Invoke-TopLevelSmoke {
    $topLevelCases = @(
        (New-SmokeCase -Message "Verify top-level help command returns command catalog..." -Context 'top-level help' -Args @('help') -MustContain @('"data"', '"group"', '"action"', '"required_options"', '"optional_options"') -Validate {
            param($result)
            $json = Parse-JsonOutput -Text $result.Text -Context 'top-level help'
            if ($null -eq $json.data -or $json.data.Count -lt 1) {
                throw 'Expected top-level help to return non-empty command catalog array'
            }
            $first = $json.data[0]
            if ([string]::IsNullOrWhiteSpace([string]$first.group) -or [string]::IsNullOrWhiteSpace([string]$first.action)) {
                throw 'Expected top-level help rows to include group and action'
            }
            if ($null -eq $first.required_options -or $null -eq $first.optional_options) {
                throw 'Expected top-level help rows to include required_options and optional_options arrays'
            }

            $hasInfoSummary = $false
            foreach ($row in $json.data) {
                if ([string]$row.group -eq 'info' -and [string]$row.action -eq 'summary') {
                    $hasInfoSummary = $true
                    break
                }
            }

            if (-not $hasInfoSummary) {
                throw 'Expected top-level help catalog to include info summary command'
            }
        }),
        (New-SmokeCase -Message "Verify top-level --help alias works..." -Context 'top-level --help' -Args @('--help') -MustContain @('"data"', '"group"', '"action"') -Validate {
            param($result)
            $json = Parse-JsonOutput -Text $result.Text -Context 'top-level --help'
            if ($null -eq $json.data -or $json.data.Count -lt 1) {
                throw 'Expected top-level --help to return non-empty command catalog array'
            }
        }),
        (New-SmokeCase -Message "Verify top-level schema returns schema catalog..." -Context 'top-level schema' -Args @('schema') -MustContain @('"commands"', '"input_schema"', '"output_schema_summary"') -Validate {
            param($result)
            $json = Parse-JsonOutput -Text $result.Text -Context 'top-level schema'
            if ($null -eq $json.data -or [string]::IsNullOrWhiteSpace([string]$json.data.'$schema')) {
                throw 'Expected top-level schema to include data.$schema'
            }
            if ($null -eq $json.data.commands -or $json.data.commands.Count -lt 1) {
                throw 'Expected top-level schema to include non-empty data.commands'
            }
        }),
        (New-SmokeCase -Message "Verify top-level schema --command filter works..." -Context 'top-level schema --command' -Args @('schema', '--command', 'cpu top') -MustContain @('"commands"', '"command_count"') -Validate {
            param($result)
            $json = Parse-JsonOutput -Text $result.Text -Context 'top-level schema --command'
            if ($null -eq $json.data.commands -or $json.data.commands.Count -ne 1) {
                throw 'Expected top-level schema --command to return exactly one command schema'
            }

            $item = $json.data.commands[0]
            if ([string]$item.group -ne 'cpu' -or [string]$item.action -ne 'top') {
                throw 'Expected top-level schema --command to return cpu top schema'
            }
        })
    )

    $script:StepCounter = 0
    $script:TotalSteps = $topLevelCases.Count
    Write-Host 'Running top-level smoke...'
    Invoke-SmokeCaseList -Cases $topLevelCases
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

Invoke-TopLevelSmoke

foreach ($trace in $TracePaths) {
    Invoke-NormalTraceSmoke -TracePath $trace
}

Write-Host "InsightCli output smoke checks passed for all traces."
