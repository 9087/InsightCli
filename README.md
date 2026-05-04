# InsightCli

InsightCli is a command-line tool for quick trace inspection and performance triage.
It reads `.utrace` / `.trace` inputs and returns machine-friendly JSON output for automation, dashboards, and scripting.

## What It Can Do

- Session and trace metadata: `info summary`, `info channels`
- Frame analysis: `frames summary`, `frames slowest`, `frames detail`
- CPU/GPU/RHI hotspots: `cpu top`, `cpu stack`, `cpu hot-functions`, `gpu top`, `gpu passes`, `gpu pass-detail`, `rhi summary`, `rhi drawcalls`, `rhi top-materials`, `rhi top-meshes`
- Animation diagnostics: `anim top-actors`, `anim graph`, `anim skinning`
- UI/Slate diagnostics: `slate top-widgets`, `slate paint-cost`, `slate invalidation-rate`
- File I/O diagnostics: `io summary`, `io slowest-reads`, `io top-files`
- Networking diagnostics: `net summary`, `net top-actors`, `net top-rpcs`, `net bandwidth-series`
- Niagara diagnostics: `niagara top-systems`, `niagara emitter-cost`
- Physics diagnostics: `physics summary`, `physics solver-stages`, `physics top-bodies`
- Threads and tasks: `threads waits`, `threads wait-chain`, `tasks top`, `tasks critical-path`
- Asset loading diagnostics: `loadtime summary`, `loadtime packages`, `loadtime slowest`, `loadtime timeline`
- GC diagnostics: `gc summary`, `gc events`, `gc longest`
- Counters and memory: `counters list`, `counters series`, `counters stats`, `memory summary`, `memory peak`, `memory tags`, `memory diff`, `memory alloc-top`, `memory leak-suspect`
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

# Frame detail with per-thread breakdown
InsightCli.exe <trace_path> frames detail --frame-index 120 --breakdown thread

# CPU top scopes on game thread
InsightCli.exe <trace_path> cpu top --thread GameThread --limit 10

# CPU stack bottom-up view for one frame
InsightCli.exe <trace_path> cpu stack --frame-index 120 --thread GameThread --view bottom-up --limit 10

# CPU hot functions ranked by self-time
InsightCli.exe <trace_path> cpu hot-functions --thread GameThread --limit 10

# GPU pass detail for a frame
InsightCli.exe <trace_path> gpu pass-detail --frame-index 120 --pass BasePass --limit 5

# Enumerate GPU passes for one frame
InsightCli.exe <trace_path> gpu passes --frame-index 120

# RHI summary and drawcall hotspots
InsightCli.exe <trace_path> rhi summary --frame-index 120
InsightCli.exe <trace_path> rhi drawcalls --limit 10

# Slate/UMG diagnostics
InsightCli.exe <trace_path> slate top-widgets --by paint --limit 10
InsightCli.exe <trace_path> slate invalidation-rate --time-start 0 --time-end 1000

# Animation diagnostics
InsightCli.exe <trace_path> anim top-actors --limit 10
InsightCli.exe <trace_path> anim graph --actor Character
InsightCli.exe <trace_path> anim skinning --limit 10

# File I/O diagnostics
InsightCli.exe <trace_path> io summary
InsightCli.exe <trace_path> io slowest-reads --limit 10
InsightCli.exe <trace_path> io top-files --limit 10

# Networking diagnostics
InsightCli.exe <trace_path> net summary
InsightCli.exe <trace_path> net top-actors --limit 10
InsightCli.exe <trace_path> net top-rpcs --limit 10
InsightCli.exe <trace_path> net bandwidth-series --time-start 0 --time-end 1000

# Niagara diagnostics
InsightCli.exe <trace_path> niagara top-systems --limit 10
InsightCli.exe <trace_path> niagara emitter-cost --system NiagaraSystem

# Physics diagnostics
InsightCli.exe <trace_path> physics summary --frame-index 120
InsightCli.exe <trace_path> physics solver-stages
InsightCli.exe <trace_path> physics top-bodies --limit 10

# Counter statistics
InsightCli.exe <trace_path> counters stats --name ActorCount

# List available counters
InsightCli.exe <trace_path> counters list

# Search marks by keyword
InsightCli.exe <trace_path> marks search --keyword load

# Load time package hotspots
InsightCli.exe <trace_path> loadtime slowest --limit 10

# Load time timeline in a time window
InsightCli.exe <trace_path> loadtime timeline --time-start 0 --time-end 5000

# GC event summary and longest events
InsightCli.exe <trace_path> gc summary
InsightCli.exe <trace_path> gc longest --limit 5

# Memory allocation hotspots and leak suspects
InsightCli.exe <trace_path> memory alloc-top --by tag --limit 10
InsightCli.exe <trace_path> memory leak-suspect --window 5 --limit 10

# Memory tag snapshot diff between two timestamps (seconds)
InsightCli.exe <trace_path> memory diff --t1 0 --t2 5 --limit 10

# Thread wait causality chain
InsightCli.exe <trace_path> threads wait-chain --thread GameThread --depth 4 --time-start 0 --time-end 5000

# Task critical path in one frame
InsightCli.exe <trace_path> tasks critical-path --frame-index 120 --top 3

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
- GC results are inferred from CPU scope name patterns (`meta.source=cpu_scope_pattern`), so pattern coverage may vary by engine version.
- `memory alloc-top --by callstack` currently returns an empty data set with `meta.warning`; use `--by tag` for actionable results.
