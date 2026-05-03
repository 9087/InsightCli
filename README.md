# InsightCli

InsightCli is a command-line tool for quick trace inspection and performance triage.
It reads `.utrace` / `.trace` inputs and returns machine-friendly JSON output for automation, dashboards, and scripting.

## What It Can Do

- Session and trace metadata: `info summary`, `info channels`
- Frame analysis: `frames summary`, `frames slowest`, `frames detail`
- CPU and GPU hotspots: `cpu top`, `cpu stack`, `gpu top`, `gpu pass-detail`
- Threads and tasks: `threads waits`, `tasks top`
- Counters and memory: `counters list`, `counters series`, `counters stats`, `memory summary`, `memory peak`, `memory tags`
- Marks and symbols: `marks search`, `marks around`, `symbols resolve`

## Quick Start

Build:

```powershell
Engine/Build/BatchFiles/Build.bat InsightCli Win64 Development
```

Run:

```powershell
Engine/Binaries/Win64/InsightCli.exe <trace_path> <group> <action> [options]
Engine/Binaries/Win64/InsightCli.exe <trace_path> --batch <commands.json|->
```

Example:

```powershell
Engine/Binaries/Win64/InsightCli.exe C:/Users/吴志伟/AppData/Local/UnrealEngine/Common/UnrealTrace/Store/001/20260502_160828.utrace frames summary
```

## Command Examples

```powershell
# Slow frames
InsightCli.exe <trace_path> frames slowest --limit 5

# CPU top scopes on game thread
InsightCli.exe <trace_path> cpu top --thread GameThread --limit 10

# GPU pass detail for a frame
InsightCli.exe <trace_path> gpu pass-detail --frame-index 120 --pass BasePass --limit 5

# Counter statistics
InsightCli.exe <trace_path> counters stats --name ActorCount

# List available counters
InsightCli.exe <trace_path> counters list

# Search marks by keyword
InsightCli.exe <trace_path> marks search --keyword load

# List enabled/known trace channels
InsightCli.exe <trace_path> info channels

# Run multiple commands in one process (NDJSON output)
InsightCli.exe <trace_path> --batch commands.json
```

Batch file example:

```json
[
	{ "group": "frames", "action": "slowest", "options": { "limit": 5 } },
	{ "group": "cpu", "action": "top", "options": { "thread": "GameThread", "limit": 10 } }
]
```

Batch behavior:

- Output is NDJSON on stdout (one envelope per command, in request order).
- Batch continues after individual command failures.
- Process exit code is the max exit code among sub-commands.
- Use `--batch -` to read batch JSON from stdin.

## Output Contract

- Success: JSON envelope on `stdout`, usually with `data` and optional `meta`.
- Error: JSON envelope on `stderr`, with `code`, `message`, and optional `details`.
- Typical codes:
	- `E1001`: trace file does not exist
	- `E1002`: unsupported extension
	- `E1003`: invalid arguments
	- `E2001`: unknown command or unsupported name

## Validation and Packaging

Smoke test:

```powershell
powershell -ExecutionPolicy Bypass -File Engine/Source/Programs/InsightCli/Tests/Smoke/InsightCli.Output.Smoke.ps1
```

Single-command validation (build + smoke + logs):

```powershell
powershell -ExecutionPolicy Bypass -File Engine/Source/Programs/InsightCli/Tools/Run-InsightCli-Validation.ps1
```

Create release zip (exe + logs + summary):

```powershell
powershell -ExecutionPolicy Bypass -File Engine/Source/Programs/InsightCli/Tools/Run-InsightCli-Validation.ps1 -PackageZip
```

Performance compare (3 rounds, 30 samples each, against historical baseline):

```powershell
powershell -ExecutionPolicy Bypass -File Engine/Source/Programs/InsightCli/Tools/Run-InsightCli-PerfCompare.ps1
```

Use a custom baseline file:

```powershell
powershell -ExecutionPolicy Bypass -File Engine/Source/Programs/InsightCli/Tools/Run-InsightCli-PerfCompare.ps1 -BaselineFile Engine/Source/Programs/InsightCli/Tools/InsightCli.PerfBaseline.json
```

Save benchmark CSV output:

```powershell
powershell -ExecutionPolicy Bypass -File Engine/Source/Programs/InsightCli/Tools/Run-InsightCli-PerfCompare.ps1 -OutputDir Engine/Saved/InsightCliArtifacts/Perf
```

## Usage Recommendations

- Start with `frames summary` to estimate baseline frame quality before drilling down.
- Use `--limit` to keep results compact when scripting.
- For time-scoped analysis, use either `--time-start/--time-end` or `--frame-range <start:end>` (mutually exclusive).
- For incident analysis, combine `frames slowest` -> `cpu/gpu` -> `threads/tasks` in that order.
- Treat CLI output as structured data first (JSON parsing) rather than plain text.
- Startup performance is currently considered good enough; use the perf compare script for periodic regression checks instead of continuous micro-optimization.
