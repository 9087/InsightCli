# InsightCli User Manual

Version: 1.0
Last Updated: 2026-05-03
Language: English

## 1. What InsightCli Is

InsightCli is a command-line tool that reads Unreal trace files (`.utrace` / `.trace`) and returns structured JSON.
It is designed for scripting, automation, and performance triage.

Supported command groups:
- `info`
- `frames`
- `cpu`
- `gpu`
- `anim`
- `slate`
- `threads`
- `tasks`
- `io`
- `loadtime`
- `gc`
- `symbols`
- `counters`
- `memory`
- `marks`

## 2. Command Format

```powershell
InsightCli.exe <trace_path> <group> <action> [options]
```

Example:

```powershell
InsightCli.exe C:/traces/run01.utrace frames summary
```

## 3. Output Model

### 3.1 Success Output

Success output is printed to `stdout` as JSON, typically with a `data` field and optional `meta`.

Example:

```json
{
  "data": {
    "trace_name": "run01.utrace",
    "trace_size_bytes": 120345678,
    "timestamp_utc": "2026-05-02T16:08:28Z"
  }
}
```

### 3.2 Error Output

Error output is printed to `stderr` as JSON.

Example:

```json
{
  "code": "E1003",
  "message": "--name is required for counters stats.",
  "details": {}
}
```

### 3.3 Common Error Codes

- `E1001`: Input trace file does not exist.
- `E1002`: Unsupported trace file extension.
- `E1003`: Invalid option or argument value.
- `E2001`: Unknown command/action or unknown requested entity.
- `E3001`: Trace-backed provider unavailable for that command.

## 4. Global Behavior and Conventions

- Unknown command options are rejected with `E1003`.
- Time windows use half-open interval semantics: `[time-start, time-end)`.
- Many commands include metadata in `meta` (for example: `limit`, `data_source`, filter echoes).
- `not found` in valid query contexts usually returns:
  - exit code `0`
  - empty `data`
  - `meta.found=false` and query echo

## 5. Command Reference

## 5.1 `info summary`

Purpose:
- Basic trace/session metadata.

Example:

```powershell
InsightCli.exe C:/traces/run01.utrace info summary
```

Sample output:

```json
{
  "data": {
    "trace_name": "run01.utrace",
    "trace_size_bytes": 120345678,
    "start_timestamp": "2026-05-02T16:08:28Z",
    "start_timestamp_source": "recorded_at_file_mtime",
    "end_timestamp": "2026-05-02T16:08:32Z",
    "duration_ms": "4220.000",
    "thread_count": "14",
    "event_count": "unavailable",
    "event_count_reason": "trace_event_count_not_exposed",
    "build_version": "unavailable",
    "build_version_reason": "trace_build_version_not_exposed"
  }
}
```

Notes:
- start_timestamp is derived from trace file mtime and is an approximate recording start marker.
- end_timestamp is computed as start_timestamp + duration_ms; when duration_ms is zero, end_timestamp is unavailable.
- `thread_id = -1` means the field is unspecified for that row and does not map to a concrete trace thread id.

## 5.2 `frames summary`

Purpose:
- Aggregated frame timeline metrics.

Options:
- `--time-start <ms>`: Inclusive start timestamp (milliseconds).
- `--time-end <ms>`: Exclusive end timestamp (milliseconds); when both are set, use `time-end >= time-start`.

Example:

```powershell
InsightCli.exe C:/traces/run01.utrace frames summary --time-start 0 --time-end 20000
```

Sample output:

```json
{
  "data": {
    "frame_count": 512,
    "avg_frame_ms": 16.42,
    "p95_frame_ms": 28.10,
    "max_frame_ms": 43.89
  },
  "meta": {
    "filter_time_start": "0.000",
    "filter_time_end": "20000.000"
  }
}
```

## 5.3 `frames slowest`

Purpose:
- Return slowest frames.

Options:
- `--limit <n>`: Maximum number of frames to return. Use a positive integer.

Example:

```powershell
InsightCli.exe C:/traces/run01.utrace frames slowest --limit 3
```

Sample output:

```json
{
  "data": [
    {
      "frame_index": 321,
      "frame_start_ms": 14561.2,
      "frame_end_ms": 14605.1,
      "frame_time_ms": 43.9
    },
    {
      "frame_index": 280,
      "frame_start_ms": 12802.4,
      "frame_end_ms": 12841.9,
      "frame_time_ms": 39.5
    }
  ],
  "meta": {
    "limit": "3"
  }
}
```

## 5.4 `frames detail`

Purpose:
- Detailed metrics for one frame.

Options:
- `--frame-index <n>` (required): Zero-based frame index in the trace timeline.
- `--breakdown <thread|statgroup>` (optional): Add frame composition breakdown by thread bucket or stat group bucket.

Example:

```powershell
InsightCli.exe C:/traces/run01.utrace frames detail --frame-index 120
InsightCli.exe C:/traces/run01.utrace frames detail --frame-index 120 --breakdown thread
```

Sample output:

```json
{
  "data": {
    "frame_index": 120,
    "frame_start_ms": 5100.2,
    "frame_end_ms": 5118.5,
    "frame_time_ms": 18.3,
    "game_thread_ms": 10.1,
    "render_thread_ms": 8.3,
    "rhi_thread_ms": 4.2,
    "gpu_ms": 12.9,
    "breakdown": [
      { "bucket": "game_thread", "ms": 10.1, "ratio": 0.55 },
      { "bucket": "render_thread", "ms": 8.3, "ratio": 0.45 }
    ]
  }
}
```

Not found sample (`frame-index` missing in trace):

```json
{
  "data": [],
  "meta": {
    "found": "false",
    "reason": "not_found",
    "query_key": "frame-index",
    "query_value": "9999"
  }
}
```

## 5.5 `cpu top`

Purpose:
- Top CPU scopes by inclusive time.

Options:
- `--thread <GameThread|RenderThread|RHIThread|thread_id>`: Select a CPU thread by common name or numeric thread id.
- `--limit <n>`: Maximum number of scopes to return, sorted by total inclusive time.

Example:

```powershell
InsightCli.exe C:/traces/run01.utrace cpu top --thread GameThread --limit 5
```

Sample output:

```json
{
  "data": [
    {
      "scope_name": "MoveActors",
      "thread_id": 1234,
      "call_count": 312,
      "total_ms": 415.7,
      "avg_ms": 1.33,
      "max_ms": 9.51,
      "min_ms": 0.07,
      "self_ms": 201.4
    }
  ],
  "meta": {
    "thread": "GameThread",
    "limit": "5",
    "data_source": "trace"
  }
}
```

## 5.6 `cpu stack`

Purpose:
- Return stack-like scope context for a given frame.

Options:
- `--frame-index <n>` (required): Frame to inspect (zero-based).
- `--thread <...>` (optional): Restrict stack output to one thread. If omitted, command-selected default thread is used.
- `--limit <n>` (optional): Cap returned stack rows.
- `--view <mode>` (optional): One of `top-down`, `bottom-up`, `leaf`. Default: `top-down`.

Example:

```powershell
InsightCli.exe C:/traces/run01.utrace cpu stack --frame-index 120 --thread GameThread --view top-down --limit 10
```

Sample output:

```json
{
  "data": [
    {
      "frame_index": 120,
      "thread_id": 1234,
      "thread_name": "GameThread",
      "scope_name": "MoveActors",
      "self_ms": 4.2,
      "total_ms": 10.1,
      "stack": [
        { "function": "UGameInstance::Tick", "module": "Game" },
        { "function": "UWorld::Tick", "module": "Engine" },
        { "function": "AMyActor::Tick", "module": "Game", "file": "Source/Game/MyActor.cpp", "line": 213 }
      ]
    }
  ]
}
```

Notes:
- `top-down`: root-to-leaf call chain.
- `bottom-up`: leaf-to-root reversed call chain.
- `leaf`: aggregate self-time by leaf function name inside the selected frame/thread.

## 5.6.1 `cpu hot-functions`

Purpose:
- List CPU functions sorted by `self_ms` descending.

Options:
- `--thread <name|id>`: CPU thread filter, for example `GameThread`.
- `--limit <n>`: Maximum row count to return.

Example:

```powershell
InsightCli.exe C:/traces/run01.utrace cpu hot-functions --thread GameThread --limit 5
```

Sample output:

```json
{
  "data": [
    {
      "scope_name": "FSceneRenderer::Render",
      "thread_id": 1234,
      "call_count": 88,
      "total_ms": 122.4,
      "avg_ms": 1.39,
      "max_ms": 4.92,
      "min_ms": 0.12,
      "self_ms": 38.7
    }
  ],
  "meta": {
    "limit": "5",
    "sort_by": "self_ms_desc",
    "thread": "GameThread",
    "data_source": "trace"
  }
}
```

## 5.7 `gpu top`

Purpose:
- Top GPU scopes (if available in trace).

Options:
- `--limit <n>`: Maximum number of GPU scopes to return.

Example:

```powershell
InsightCli.exe C:/traces/run01.utrace gpu top --limit 5
```

Sample output:

```json
{
  "data": [
    {
      "gpu_scope_name": "BasePass",
      "call_count": 240,
      "total_ms": 312.4,
      "avg_ms": 1.30,
      "max_ms": 4.22
    }
  ],
  "meta": {
    "limit": "5",
    "data_source": "trace"
  }
}
```

No-data traces may return an empty `data` array with success.

## 5.8 `gpu pass-detail`

Purpose:
- Detail for a named GPU pass in one frame.

Options:
- `--frame-index <n>` (required): Target frame index (zero-based).
- `--pass <name>` (required): GPU pass name, for example `BasePass`.
- `--limit <n>`: Maximum number of matching rows.

Example:

```powershell
InsightCli.exe C:/traces/run01.utrace gpu pass-detail --frame-index 120 --pass BasePass --limit 3
```

Sample output:

```json
{
  "data": [
    {
      "frame_index": 120,
      "pass_name": "BasePass",
      "draw_calls": 240,
      "duration_ms": 4.22
    }
  ],
  "meta": {
    "frame_index": "120",
    "limit": "3",
    "data_source": "trace"
  }
}
```

## 5.8.1 `gpu passes`

Purpose:
- Enumerate GPU passes in one frame, or aggregate pass cost in a time window.

Options:
- `--frame-index <n>`: Target frame index (zero-based).
- `--time-start <ms>`: Inclusive start timestamp in milliseconds.
- `--time-end <ms>`: Exclusive end timestamp in milliseconds.

Notes:
- `--frame-index` cannot be combined with `--time-start/--time-end`.

Example:

```powershell
InsightCli.exe C:/traces/run01.utrace gpu passes --frame-index 120
```

Sample output:

```json
{
  "data": [
    {
      "pass": "BasePass",
      "gpu_ms": 4.22,
      "draw_call_count": 240
    },
    {
      "pass": "ShadowDepths",
      "gpu_ms": 2.18,
      "draw_call_count": 128
    }
  ],
  "meta": {
    "frame_index": "120",
    "data_source": "trace"
  }
}
```

## 5.8.2 `rhi summary`

Purpose:
- Return RHI-level summary metrics, including draw call count and RHI thread time approximation.

Options:
- `--frame-index <n>`: Restrict summary to one frame.

Example:

```powershell
InsightCli.exe C:/traces/run01.utrace rhi summary --frame-index 120
```

Sample output:

```json
{
  "data": {
    "draw_call_count": 460,
    "primitive_count": 0,
    "triangle_count": 0,
    "rhi_thread_ms": 0.0
  },
  "meta": {
    "frame_index": "120",
    "data_source": "approx_cpu_gpu",
    "approximation": "draw_calls_from_gpu_passes; rhi_thread_ms_from_frame_samples"
  }
}
```

## 5.8.3 `rhi drawcalls`

Purpose:
- Return draw call hotspots sorted by draw call count.

Options:
- `--limit <n>`: Maximum number of rows returned.
- `--frame-index <n>`: Restrict drawcall rows to one frame.

Example:

```powershell
InsightCli.exe C:/traces/run01.utrace rhi drawcalls --limit 5
```

Sample output:

```json
{
  "data": [
    {
      "render_target": "BasePass",
      "material": "unknown",
      "mesh": "unknown",
      "draw_call_count": 240,
      "gpu_ms": 4.2
    }
  ],
  "meta": {
    "limit": "5",
    "data_source": "approx_cpu_gpu",
    "approximation": "draw_calls_from_gpu_passes"
  }
}
```

## 5.8.4 `rhi top-materials`

Purpose:
- Return top material buckets by draw call count.

Notes:
- When material-level channels are unavailable, output uses a single `unknown` bucket.

## 5.8.5 `rhi top-meshes`

Purpose:
- Return top mesh buckets by draw call count.

Notes:
- When mesh-level channels are unavailable, output uses a single `unknown` bucket.

## 5.8.6 `slate top-widgets`

Purpose:
- Return top Slate/UMG widget scopes by selected metric.

Options:
- `--by paint|tick|invalidation`: Select aggregation metric (default `paint`).
- `--limit <n>`: Maximum row count.

## 5.8.7 `slate paint-cost`

Purpose:
- Return frame-oriented Slate paint cost approximation.

Options:
- `--frame-index <n>`: Target frame index.

## 5.8.8 `slate invalidation-rate`

Purpose:
- Return Slate invalidation event rate in a time window.

Options:
- `--time-start <ms>`: Inclusive start timestamp.
- `--time-end <ms>`: Exclusive end timestamp.

Notes:
- If Slate trace channel data is unavailable, this command falls back to CPU scope name pattern approximation and emits `meta.warning`.

## 5.8.9 `anim top-actors`

Purpose:
- Return top actor buckets by animation-related CPU scope cost.

Options:
- `--limit <n>`: Maximum row count.

## 5.8.10 `anim graph`

Purpose:
- Return animation graph node-level cost approximation for one actor filter.

Options:
- `--actor <name>`: Actor name fragment to match (required).

## 5.8.11 `anim skinning`

Purpose:
- Return top skinning-related hotspots by CPU scope approximation.

Options:
- `--limit <n>`: Maximum row count.

Notes:
- When Animation channel data is unavailable, these commands fall back to CPU scope name pattern approximation and emit `meta.warning`.

## 5.8.12 `io summary`

Purpose:
- Return total read bytes, read count, and sync/async split from FileActivity traces.

## 5.8.13 `io slowest-reads`

Purpose:
- Return slowest read operations ordered by read duration.

Options:
- `--limit <n>`: Maximum row count.

## 5.8.14 `io top-files`

Purpose:
- Return top files by aggregated read bytes.

Options:
- `--limit <n>`: Maximum row count.

## 5.9 `threads waits`

Purpose:
- Thread waiting samples and blocker chain context.

Options:
- `--frame-index <n>` (optional): Restrict waiting samples to one frame.
- `--limit <n>`: Maximum number of wait samples.

Example:

```powershell
InsightCli.exe C:/traces/run01.utrace threads waits --frame-index 120 --limit 5
```

Sample output:

```json
{
  "data": [
    {
      "frame_index": 120,
      "thread_id": 1234,
      "thread_name": "GameThread",
      "wait_type": "Event",
      "wait_object": "SyncObj#1200",
      "wait_ms": 3.5,
      "owner_thread_id": 5678,
      "owner_thread_name": "RenderThread",
      "blocker_thread_id": 5678,
      "blocker_thread_name": "RenderThread",
      "chain_depth": 1,
      "chain_status": "resolved",
      "unresolved_reason": "",
      "blocked_to_blocker_thread_chain": [1234, 5678],
      "begin_ms": 5101.0,
      "end_ms": 5104.5
    }
  ]
}
```

## 5.9.1 `threads wait-chain`

Purpose:
- Build wait causality chains for one target thread.

Options:
- `--thread <name>` (required): Target thread name filter.
- `--depth <n>`: Max hop depth to emit. Default: `4`.
- `--time-start <ms>`: Inclusive start timestamp in milliseconds.
- `--time-end <ms>`: Exclusive end timestamp in milliseconds.

Example:

```powershell
InsightCli.exe C:/traces/run01.utrace threads wait-chain --thread GameThread --depth 4 --time-start 0 --time-end 5000
```

Sample output:

```json
{
  "data": [
    {
      "frame_index": 120,
      "begin_ms": 5101.0,
      "end_ms": 5104.5,
      "wait_ms": 3.5,
      "chain_depth": 2,
      "confidence": "medium",
      "chain": [
        {
          "hop_index": 0,
          "thread_id": 1234,
          "thread_name": "GameThread",
          "task_id": -1,
          "task_name": "unavailable",
          "wait_ms": 3.5,
          "next_thread_id": 5678,
          "next_thread_name": "RenderThread",
          "next_task": "unavailable"
        },
        {
          "hop_index": 1,
          "thread_id": 5678,
          "thread_name": "RenderThread",
          "task_id": -1,
          "task_name": "unavailable",
          "wait_ms": 0.0,
          "next_thread_id": -1,
          "next_thread_name": "",
          "next_task": "unavailable"
        }
      ]
    }
  ],
  "meta": {
    "thread": "GameThread",
    "depth": "4",
    "data_source": "trace"
  }
}
```

## 5.10 `tasks top`

Purpose:
- Task queue and dependency path diagnostics.

Options:
- `--frame-index <n>` (optional): Restrict task diagnostics to one frame.
- `--limit <n>`: Maximum number of task rows.

Example:

```powershell
InsightCli.exe C:/traces/run01.utrace tasks top --frame-index 120 --limit 5
```

Sample output:

```json
{
  "data": [
    {
      "task_id": 1001,
      "frame_index": 120,
      "task_name": "BuildVisibilityLists",
      "queue_name": "AnyThread",
      "dependency_task_ids": [1000],
      "dependency_status": "resolved",
      "dependency_issue": "",
      "enqueue_ms": 5100.3,
      "start_ms": 5100.8,
      "end_ms": 5102.1,
      "queue_wait_ms": 0.5,
      "run_ms": 1.3,
      "critical_path_ms": 3.8,
      "critical_path_depth": 2,
      "critical_path_task_chain": [999, 1000, 1001],
      "worker_thread_id": 72
    }
  ]
}
```

## 5.10.1 `tasks critical-path`

Purpose:
- Return weighted longest dependency paths for one frame.

Options:
- `--frame-index <n>` (required): Target frame index.
- `--top <k>`: Number of critical paths to return. Default: `3`.

Example:

```powershell
InsightCli.exe C:/traces/run01.utrace tasks critical-path --frame-index 120 --top 3
```

Sample output:

```json
{
  "data": [
    {
      "path_rank": 1,
      "frame_index": 120,
      "total_duration_ms": 5.7,
      "node_count": 3,
      "partial": false,
      "path": [
        {
          "task_id": 998,
          "task_name": "BuildVisibilityLists",
          "thread": "AnyThread",
          "start_ms": 5100.8,
          "duration_ms": 1.3,
          "waits_for": [997]
        },
        {
          "task_id": 999,
          "task_name": "GatherLights",
          "thread": "AnyThread",
          "start_ms": 5102.4,
          "duration_ms": 2.1,
          "waits_for": [998]
        },
        {
          "task_id": 1000,
          "task_name": "FinalizeShadows",
          "thread": "RenderThread",
          "start_ms": 5104.8,
          "duration_ms": 2.3,
          "waits_for": [999]
        }
      ]
    }
  ],
  "meta": {
    "frame_index": "120",
    "top": "3",
    "data_source": "trace"
  }
}
```

## 5.11 `symbols resolve`

Purpose:
- Resolve a scope name to source symbol information.

Options:
- `--name <scope_name>` (required): Scope/symbol-like name to resolve from trace data.

Example:

```powershell
InsightCli.exe C:/traces/run01.utrace symbols resolve --name MoveActors
```

Sample output:

```json
{
  "data": {
    "scope_name": "MoveActors",
    "module": "Engine",
    "symbol": "MoveActors",
    "function": "MoveActors",
    "file": "Engine/Source/Runtime/Engine/Private/WorldTick.cpp",
    "line": 317,
    "confidence": 0.99,
    "alternatives": [
      {
        "module": "Engine",
        "symbol": "MoveActors_Internal",
        "function": "MoveActors_Internal",
        "file": "Engine/Source/Runtime/Engine/Private/WorldTick.cpp",
        "line": 365,
        "confidence": 0.77
      }
    ]
  },
  "meta": {
    "data_source": "trace"
  }
}
```

Not-found sample:

```json
{
  "data": {},
  "meta": {
    "found": "false",
    "reason": "not_found",
    "query_key": "name",
    "query_value": "DefinitelyMissingScope_123",
    "data_source": "trace"
  }
}
```

## 5.12 `counters list`

Purpose:
- Enumerate available trace counters and metadata.

Example:

```powershell
InsightCli.exe C:/traces/run01.utrace counters list
```

Sample output:

```json
{
  "data": [
    {
      "name": "DynamicNaniteScalingPrimary_Fraction",
      "type": "float",
      "unit": "count",
      "sample_count": 5131,
      "trace_backed": true
    },
    {
      "name": "STAT_TotalAllocatorCalls",
      "type": "int64",
      "unit": "count",
      "sample_count": 5131,
      "trace_backed": true
    }
  ],
  "meta": {
    "count": "1924",
    "data_source": "trace"
  }
}
```

## 5.13 `counters series`

Purpose:
- Time series for one counter.

Options:
- `--name <counter_name>` (required): Exact counter name from `counters list`.
- `--time-start <ms>`: Inclusive start timestamp in milliseconds.
- `--time-end <ms>`: Exclusive end timestamp in milliseconds.

Example:

```powershell
InsightCli.exe C:/traces/run01.utrace counters series --name DynamicNaniteScalingPrimary_Fraction --time-start 0 --time-end 5000
```

Sample output:

```json
{
  "data": [
    { "timestamp_ms": 12.4, "value": 0.62 },
    { "timestamp_ms": 29.1, "value": 0.63 },
    { "timestamp_ms": 45.8, "value": 0.61 }
  ],
  "meta": {
    "counter_name": "DynamicNaniteScalingPrimary_Fraction",
    "counter_type": "float",
    "counter_unit": "count",
    "filter_time_start": "0.000",
    "filter_time_end": "5000.000",
    "data_source": "trace"
  }
}
```

## 5.14 `counters stats`

Purpose:
- Aggregate stats for one counter.

Options:
- `--name <counter_name>` (required): Exact counter name from `counters list`.

Example:

```powershell
InsightCli.exe C:/traces/run01.utrace counters stats --name DynamicNaniteScalingPrimary_Fraction
```

Sample output:

```json
{
  "data": {
    "counter_name": "DynamicNaniteScalingPrimary_Fraction",
    "sample_count": 5131,
    "min": 0.44,
    "max": 0.86,
    "avg": 0.63,
    "delta": -0.02
  },
  "meta": {
    "counter_type": "float",
    "counter_unit": "count",
    "data_source": "trace"
  }
}
```

Unknown counter sample:

```json
{
  "code": "E2001",
  "message": "Counter name not found.",
  "details": {
    "name": "UnknownCounter",
    "available_counters": "DynamicNaniteScalingPrimary_Fraction,STAT_TotalAllocatorCalls,..."
  }
}
```

## 5.15 `memory summary`

Purpose:
- Summary memory metrics in selected scope.

Options:
- `--time-start <ms>`: Inclusive start timestamp in milliseconds.
- `--time-end <ms>`: Exclusive end timestamp in milliseconds.

Example:

```powershell
InsightCli.exe C:/traces/run01.utrace memory summary
```

Sample output:

```json
{
  "data": {
    "min_bytes": 2194422570,
    "max_bytes": 2419938540,
    "avg_bytes": 2301801184,
    "end_bytes": 2314016240
  },
  "meta": {
    "data_source": "trace"
  }
}
```

## 5.16 `memory peak`

Purpose:
- Peak memory point and nearby context.

Options:
- `--time-start <ms>`: Inclusive start timestamp in milliseconds.
- `--time-end <ms>`: Exclusive end timestamp in milliseconds.

Example:

```powershell
InsightCli.exe C:/traces/run01.utrace memory peak
```

Sample output:

```json
{
  "data": {
    "peak_bytes": 2419938540,
    "peak_timestamp_ms": 14922.1,
    "peak_frame_index": 328,
    "peak_context": [
      { "timestamp_ms": 14896.3, "bytes": 2401121300, "frame_index": 327 },
      { "timestamp_ms": 14922.1, "bytes": 2419938540, "frame_index": 328 },
      { "timestamp_ms": 14938.8, "bytes": 2410112770, "frame_index": 329 }
    ]
  },
  "meta": {
    "data_source": "trace"
  }
}
```

## 5.17 `memory series`

Purpose:
- Time series of memory values.

Options:
- `--time-start <ms>`: Inclusive start timestamp in milliseconds.
- `--time-end <ms>`: Exclusive end timestamp in milliseconds.
- `--limit <n>`: Maximum number of points returned after filtering.

Example:

```powershell
InsightCli.exe C:/traces/run01.utrace memory series --time-start 0 --time-end 5000 --limit 5
```

Sample output:

```json
{
  "data": [
    { "timestamp_ms": 101.3, "bytes": 2231157760, "frame_index": 5 },
    { "timestamp_ms": 117.9, "bytes": 2233042650, "frame_index": 6 }
  ],
  "meta": {
	"data_source": "trace",
    "limit": "5",
	"time_start_ms": "0.000",
	"time_end_ms": "5000.000"
  }
}
```

## 5.18 `memory tags`

Purpose:
- Memory usage split by tag/category.

Options:
- `--limit <n>`: Maximum number of memory tags to return.

Example:

```powershell
InsightCli.exe C:/traces/run01.utrace memory tags --limit 3
```

Sample output:

```json
{
  "data": [
    { "tag_name": "Textures", "bytes": 948746658, "percent_ratio": 0.41 },
    { "tag_name": "Meshes", "bytes": 740289772, "percent_ratio": 0.32 },
    { "tag_name": "Animations", "bytes": 300123456, "percent_ratio": 0.13 }
  ],
  "meta": {
	"data_source": "trace",
    "limit": "3"
  }
}
```

## 5.19 `memory alloc-top`

Purpose:
- Return top memory allocation owners.

Options:
- `--limit <n>`: Maximum number of rows returned.
- `--by <tag|callstack>`: Group mode; default is `tag`.

Example:

```powershell
InsightCli.exe C:/traces/run01.utrace memory alloc-top --by tag --limit 5
```

Sample output:

```json
{
  "data": [
    { "tag_name": "Textures", "bytes": 948746658, "sample_count": 64 },
    { "tag_name": "Meshes", "bytes": 740289772, "sample_count": 64 }
  ],
  "meta": {
    "data_source": "trace",
    "by": "tag",
    "limit": "5"
  }
}
```

Notes:
- `--by callstack` currently returns an empty `data` array with `meta.warning`.

## 5.19.1 `memory diff`

Purpose:
- Diff memory tag snapshots between two timestamps.

Options:
- `--t1 <sec>` (required): Start snapshot timestamp in seconds.
- `--t2 <sec>` (required): End snapshot timestamp in seconds.
- `--limit <n>`: Maximum number of rows returned.

Example:

```powershell
InsightCli.exe C:/traces/run01.utrace memory diff --t1 0 --t2 5 --limit 5
```

Sample output:

```json
{
  "data": [
    { "tag_name": "Textures", "delta_bytes": 104857600, "delta_alloc_count": 12, "t1_bytes": 734003200, "t2_bytes": 838860800 }
  ],
  "meta": {
    "data_source": "trace",
    "t1_sec": "0",
    "t2_sec": "5",
    "limit": "5"
  }
}
```

## 5.20 `memory leak-suspect`

Purpose:
- Identify tags whose memory grows in the recent trailing window.

Options:
- `--window <sec>` (required): Trailing time window in seconds.
- `--limit <n>`: Maximum number of rows returned.

Example:

```powershell
InsightCli.exe C:/traces/run01.utrace memory leak-suspect --window 5 --limit 5
```

Sample output:

```json
{
  "data": [
    { "tag_name": "Streaming", "growth_bytes": 52428800, "sample_count": 12, "start_bytes": 104857600, "end_bytes": 157286400 }
  ],
  "meta": {
    "data_source": "trace",
    "window_sec": "5.000",
    "limit": "5"
  }
}
```

## 5.21 `marks search`

Purpose:
- Search bookmark/log messages by keyword with optional filters.

Options:
- `--keyword <text>` (required): Search text in mark/log messages.
- `--case-sensitive`: Use case-sensitive matching. Default is case-insensitive.
- `--exact`: Require full-string match. Default is substring match.
- `--category <bookmark|log|...>`: Filter by mark category.
- `--channel <name>`: Filter by channel/category name.
- `--thread-id <id>`: Filter by numeric thread id.
- `--time-start <ms>`: Inclusive start timestamp in milliseconds.
- `--time-end <ms>`: Exclusive end timestamp in milliseconds.

Example:

```powershell
InsightCli.exe C:/traces/run01.utrace marks search --keyword load --category log --channel Streaming --time-start 0 --time-end 5000
```

Sample output:

```json
{
  "data": [
    {
      "timestamp_ms": 130.2,
      "category": "log",
      "channel": "Streaming",
      "message": "LoadPackage started for /Game/Maps/Main",
      "thread_id": -1
    }
  ],
  "meta": {
    "keyword": "load",
    "case_sensitive": "false",
    "exact": "false",
    "filter_category": "log",
    "filter_channel": "Streaming",
    "filter_time_start": "0.000",
    "filter_time_end": "5000.000",
    "data_source": "trace"
  }
}
```

## 5.22 `marks around`

Purpose:
- Return marks around a timestamp window.

Options:
- `--timestamp <ms>` (required): Center timestamp in milliseconds.
- `--window <ms>` (required): Half-window size; effective range is `[timestamp-window, timestamp+window)`.
- `--category <...>`: Optional category filter within the around window.
- `--channel <...>`: Optional channel filter within the around window.
- `--thread-id <...>`: Optional numeric thread filter.
- `--time-start <ms>`: Optional additional lower bound, intersected with the around window.
- `--time-end <ms>`: Optional additional upper bound, intersected with the around window.

Example:

```powershell
InsightCli.exe C:/traces/run01.utrace marks around --timestamp 12000 --window 500 --channel Streaming
```

Sample output:

```json
{
  "data": [
    {
      "timestamp_ms": 11880.4,
      "category": "bookmark",
      "channel": "Bookmark",
      "message": "Streaming update begin",
      "thread_id": -1
    },
    {
      "timestamp_ms": 12102.7,
      "category": "log",
      "channel": "Streaming",
      "message": "LoadPackage completed for /Game/Maps/Main",
      "thread_id": -1
    }
  ],
  "meta": {
    "around_timestamp": "12000.000",
    "around_window": "500.000",
    "filter_channel": "Streaming",
    "data_source": "trace"
  }
}
```

## 5.11 `loadtime` commands

Purpose:
- Analyze asset/package loading cost from the trace `LoadTime` channel.

Commands:
- `loadtime summary`
- `loadtime packages --limit <n> --sort-by total|serialize|postload`
- `loadtime slowest --limit <n>`
- `loadtime timeline --time-start <ms> --time-end <ms>`

Notes:
- If the trace does not include the LoadTime channel, the command returns success with empty data and `meta.channel_state`.
- `loadtime timeline` echoes time-window metadata in `meta`.

## 5.12 `gc` commands

Purpose:
- Surface garbage collection events from trace CPU scopes in structured form.

Commands:
- `gc summary`
- `gc events --limit <n>`
- `gc longest --limit <n>`

Notes:
- GC detection currently relies on CPU scope name patterns and reports `meta.source=cpu_scope_pattern`.
- Pattern-based matching can vary across engine versions and trace instrumentation settings.

## 6. Practical Workflows

### 6.1 Slow Frame Investigation

```powershell
InsightCli.exe <trace> frames slowest --limit 5
InsightCli.exe <trace> cpu top --thread GameThread --limit 10
InsightCli.exe <trace> gpu top --limit 10
InsightCli.exe <trace> threads waits --frame-index <frame>
InsightCli.exe <trace> tasks top --frame-index <frame>
```

### 6.2 Counter-Driven Regression Check

```powershell
InsightCli.exe <trace> counters list
InsightCli.exe <trace> counters series --name <counter> --time-start 0 --time-end 10000
InsightCli.exe <trace> counters stats --name <counter>
```

### 6.3 Event-Correlated Diagnosis

```powershell
InsightCli.exe <trace> marks search --keyword hitch
InsightCli.exe <trace> marks around --timestamp <ms> --window 300
InsightCli.exe <trace> frames detail --frame-index <frame>
```

## 7. Validation Commands

Build:

```powershell
Engine/Build/BatchFiles/Build.bat InsightCli Win64 Development
```

Smoke test:

```powershell
powershell -ExecutionPolicy Bypass -File Engine/Source/Programs/InsightCli/Tests/Smoke/InsightCli.Output.Smoke.ps1 -ExePath Engine/Binaries/Win64/InsightCli.exe
```

## 8. Notes

- All sample outputs in this manual are representative. Exact values vary by trace.
- For robust automation, parse JSON and rely on field names rather than text matching.
- If your pipeline needs schema stability checks, pair this manual with smoke/golden tests.
