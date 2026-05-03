param(
    [string]$Configuration = "Development",
    [string]$Platform = "Win64",
    [string]$OutputRoot = "",
    [switch]$SkipBuild,
    [switch]$SkipSmoke,
    [switch]$PackageZip
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-RepoRoot {
    $candidate = Resolve-Path (Join-Path $PSScriptRoot "..\..\..\..\..")
    return $candidate.Path
}

function Ensure-Directory {
    param([string]$Path)
    if (-not (Test-Path -Path $Path)) {
        New-Item -ItemType Directory -Path $Path | Out-Null
    }
}

function Run-Step {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [Parameter(Mandatory = $true)]
        [scriptblock]$Action,
        [Parameter(Mandatory = $true)]
        [string]$LogPath
    )

    Write-Host "=== $Name ==="
    & $Action 2>&1 | Tee-Object -FilePath $LogPath
    if ($LASTEXITCODE -ne 0) {
        throw "$Name failed with exit code $LASTEXITCODE. See log: $LogPath"
    }
}

function Write-Summary {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Repository,
        [Parameter(Mandatory = $true)]
        [string]$Configuration,
        [Parameter(Mandatory = $true)]
        [string]$Platform,
        [Parameter(Mandatory = $true)]
        [string]$BuildStatus,
        [Parameter(Mandatory = $true)]
        [string]$SmokeStatus,
        [Parameter(Mandatory = $true)]
        [string]$PackageStatus,
        [Parameter(Mandatory = $true)]
        [string]$PackagePath,
        [Parameter(Mandatory = $true)]
        [string]$ArtifactDir,
        [string]$Result = "",
        [string]$ErrorMessage = ""
    )

    $lines = @(
        "InsightCli Validation Summary"
        "Timestamp: $(Get-Date -Format s)"
        "Repository: $Repository"
        "Configuration: $Configuration"
        "Platform: $Platform"
        "Build: $BuildStatus"
        "Smoke: $SmokeStatus"
        "Package: $PackageStatus"
        "PackageZip: $PackagePath"
    )

    if (-not [string]::IsNullOrWhiteSpace($Result)) {
        $lines += "Result: $Result"
    }
    if (-not [string]::IsNullOrWhiteSpace($ErrorMessage)) {
        $lines += "Error: $ErrorMessage"
    }

    $lines += "Artifact Dir: $ArtifactDir"
    $lines | Set-Content -Path $Path -Encoding UTF8
}

$repoRoot = Get-RepoRoot
$buildScript = Join-Path $repoRoot "Engine\Build\BatchFiles\Build.bat"
$smokeScript = Join-Path $repoRoot "Engine\Source\Programs\InsightCli\Tests\Smoke\InsightCli.Output.Smoke.ps1"
$exePath = Join-Path $repoRoot "Engine\Binaries\$Platform\InsightCli.exe"

if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $repoRoot "Engine\Saved\InsightCliArtifacts"
}

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$runDir = Join-Path $OutputRoot $stamp
Ensure-Directory -Path $runDir

$buildLog = Join-Path $runDir "build.log"
$smokeLog = Join-Path $runDir "smoke.log"
$summaryPath = Join-Path $runDir "summary.txt"

$buildStatus = "Skipped"
$smokeStatus = "Skipped"
$packageStatus = "Disabled"
$packagePath = ""

try {
    Push-Location $repoRoot

    if (-not $SkipBuild) {
        if (-not (Test-Path -Path $buildScript)) {
            throw "Build script not found: $buildScript"
        }

        Run-Step -Name "Build InsightCli ($Platform $Configuration)" -LogPath $buildLog -Action {
            & $buildScript "InsightCli" $Platform $Configuration
        }
        $buildStatus = "Passed"
    }

    if (-not $SkipSmoke) {
        if (-not (Test-Path -Path $smokeScript)) {
            throw "Smoke script not found: $smokeScript"
        }

        Run-Step -Name "Smoke Tests" -LogPath $smokeLog -Action {
            powershell -ExecutionPolicy Bypass -File $smokeScript
        }
        $smokeStatus = "Passed"
    }

    if (Test-Path -Path $exePath) {
        Copy-Item -Path $exePath -Destination (Join-Path $runDir "InsightCli.exe") -Force
    }

    Write-Summary -Path $summaryPath -Repository $repoRoot -Configuration $Configuration -Platform $Platform -BuildStatus $buildStatus -SmokeStatus $smokeStatus -PackageStatus $packageStatus -PackagePath $packagePath -ArtifactDir $runDir

    if ($PackageZip) {
        $packagePath = Join-Path $runDir "InsightCli-$Platform-$Configuration-$stamp.zip"
        $filesToPackage = @()
        $candidateFiles = @(
            (Join-Path $runDir "InsightCli.exe"),
            $buildLog,
            $smokeLog,
            $summaryPath
        )

        foreach ($candidate in $candidateFiles) {
            if (Test-Path -Path $candidate) {
                $filesToPackage += $candidate
            }
        }

        if ($filesToPackage.Count -eq 0) {
            throw "No files available for packaging in $runDir"
        }

        Compress-Archive -Path $filesToPackage -DestinationPath $packagePath -Force
        $packageStatus = "Passed"

        Write-Summary -Path $summaryPath -Repository $repoRoot -Configuration $Configuration -Platform $Platform -BuildStatus $buildStatus -SmokeStatus $smokeStatus -PackageStatus $packageStatus -PackagePath $packagePath -ArtifactDir $runDir
    }

    Write-Host "Validation completed successfully."
    Write-Host "Artifacts: $runDir"
    if ($PackageZip) {
        Write-Host "Package: $packagePath"
    }
    exit 0
}
catch {
    Write-Summary -Path $summaryPath -Repository $repoRoot -Configuration $Configuration -Platform $Platform -BuildStatus $buildStatus -SmokeStatus $smokeStatus -PackageStatus $packageStatus -PackagePath $packagePath -ArtifactDir $runDir -Result "Failed" -ErrorMessage $_.Exception.Message

    Write-Error $_.Exception.Message
    Write-Host "Artifacts: $runDir"
    exit 1
}
finally {
    Pop-Location
}
