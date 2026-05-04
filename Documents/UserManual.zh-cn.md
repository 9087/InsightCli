# InsightCli 用户手册

版本: 1.0
最后更新: 2026-05-03
语言: 简体中文

## 1. InsightCli 是什么

InsightCli 是一个命令行工具，用于读取 Unreal trace 文件（`.utrace` / `.trace`）并输出结构化 JSON。
它适用于脚本化、自动化流水线和性能排查。

支持的命令组：
- `info`
- `frames`
- `cpu`
- `gpu`
- `threads`
- `tasks`
- `loadtime`
- `gc`
- `symbols`
- `counters`
- `memory`
- `marks`

## 2. 命令格式

```powershell
InsightCli.exe <trace_path> <group> <action> [options]
```

示例：

```powershell
InsightCli.exe C:/traces/run01.utrace frames summary
```

## 3. 输出模型

### 3.1 成功输出

成功时会在 `stdout` 输出 JSON，通常包含 `data` 字段，以及可选的 `meta`。

示例：

```json
{
  "data": {
    "trace_name": "run01.utrace",
    "trace_size_bytes": 120345678,
    "timestamp_utc": "2026-05-02T16:08:28Z"
  }
}
```

### 3.2 错误输出

错误时会在 `stderr` 输出 JSON。

示例：

```json
{
  "code": "E1003",
  "message": "--name is required for counters stats.",
  "details": {}
}
```

### 3.3 常见错误码

- `E1001`: 输入 trace 文件不存在。
- `E1002`: trace 文件扩展名不受支持。
- `E1003`: 参数或参数值非法。
- `E2001`: 未知命令/动作，或请求实体不存在。
- `E3001`: 当前命令依赖的 trace provider 不可用。

## 4. 全局行为与约定

- 未声明的命令参数会被拒绝，返回 `E1003`。
- 时间窗口统一为左闭右开区间：`[time-start, time-end)`。
- 很多命令会在 `meta` 中回显元信息（例如：`limit`、`data_source`、过滤参数）。
- 在合法查询场景中，`not found` 通常表现为：
  - 退出码 `0`
  - 空 `data`
  - `meta.found=false` 并回显查询条件

## 5. 命令参考

## 5.1 `info summary`

用途：
- 查看 trace / 会话基础元数据。

示例：

```powershell
InsightCli.exe C:/traces/run01.utrace info summary
```

示例输出：

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

说明：
- start_timestamp 来自 trace 文件 mtime，表示近似录制起点。
- end_timestamp 由 start_timestamp + duration_ms 推导；当 duration_ms 为 0 时，end_timestamp 为 unavailable。
- `thread_id = -1` 表示该行字段未指定（unspecified），不对应具体的 trace 线程 ID。

## 5.2 `frames summary`

用途：
- 查看帧时间线聚合指标。

参数：
- `--time-start <ms>`：时间窗口起点（毫秒，包含）。
- `--time-end <ms>`：时间窗口终点（毫秒，不包含）；同时传入时需满足 `time-end >= time-start`。

示例：

```powershell
InsightCli.exe C:/traces/run01.utrace frames summary --time-start 0 --time-end 20000
```

示例输出：

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

用途：
- 返回最慢帧列表。

参数：
- `--limit <n>`：返回最慢帧数量上限，使用正整数。

示例：

```powershell
InsightCli.exe C:/traces/run01.utrace frames slowest --limit 3
```

示例输出：

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

用途：
- 查看单帧详细指标。

参数：
- `--frame-index <n>`（必填）：要查询的帧索引（从 0 开始）。

示例：

```powershell
InsightCli.exe C:/traces/run01.utrace frames detail --frame-index 120
```

示例输出：

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
    "gpu_ms": 12.9
  }
}
```

未命中示例（trace 中没有该 `frame-index`）：

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

用途：
- 按总耗时返回 CPU 热点 scope。

参数：
- `--thread <GameThread|RenderThread|RHIThread|thread_id>`：按线程名或数值线程 ID 过滤。
- `--limit <n>`：返回热点 scope 数量上限（按总耗时排序）。

示例：

```powershell
InsightCli.exe C:/traces/run01.utrace cpu top --thread GameThread --limit 5
```

示例输出：

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

用途：
- 返回指定帧的栈式 scope 上下文。

参数：
- `--frame-index <n>`（必填）：目标帧索引（从 0 开始）。
- `--thread <...>`（可选）：仅返回指定线程的栈式 scope。
- `--limit <n>`（可选）：限制返回行数。

示例：

```powershell
InsightCli.exe C:/traces/run01.utrace cpu stack --frame-index 120 --thread GameThread --limit 10
```

示例输出：

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

## 5.7 `gpu top`

用途：
- 返回 GPU 热点 scope（trace 中有 GPU 数据时）。

参数：
- `--limit <n>`：返回 GPU 热点 scope 的数量上限。

示例：

```powershell
InsightCli.exe C:/traces/run01.utrace gpu top --limit 5
```

示例输出：

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

若 trace 无 GPU 数据，可能成功返回空 `data`。

## 5.8 `gpu pass-detail`

用途：
- 返回某帧某个 GPU pass 的详细信息。

参数：
- `--frame-index <n>`（必填）：目标帧索引（从 0 开始）。
- `--pass <name>`（必填）：GPU pass 名称，例如 `BasePass`。
- `--limit <n>`：返回匹配记录的数量上限。

示例：

```powershell
InsightCli.exe C:/traces/run01.utrace gpu pass-detail --frame-index 120 --pass BasePass --limit 3
```

示例输出：

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

## 5.9 `threads waits`

用途：
- 返回线程等待样本与阻塞链。

参数：
- `--frame-index <n>`（可选）：仅查看指定帧内的等待样本。
- `--limit <n>`：返回等待样本数量上限。

示例：

```powershell
InsightCli.exe C:/traces/run01.utrace threads waits --frame-index 120 --limit 5
```

示例输出：

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

## 5.10 `tasks top`

用途：
- 返回任务队列与依赖关键路径诊断信息。

参数：
- `--frame-index <n>`（可选）：仅查看指定帧内的任务诊断。
- `--limit <n>`：返回任务记录数量上限。

示例：

```powershell
InsightCli.exe C:/traces/run01.utrace tasks top --frame-index 120 --limit 5
```

示例输出：

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

## 5.11 `symbols resolve`

用途：
- 将 scope 名解析到源码符号信息。

参数：
- `--name <scope_name>`（必填）：要解析的 scope/符号名。

示例：

```powershell
InsightCli.exe C:/traces/run01.utrace symbols resolve --name MoveActors
```

示例输出：

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

未命中示例：

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

用途：
- 列出可用 counter 及元数据。

示例：

```powershell
InsightCli.exe C:/traces/run01.utrace counters list
```

示例输出：

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

用途：
- 查询单个 counter 的时间序列。

参数：
- `--name <counter_name>`（必填）：counter 名称，需来自 `counters list`。
- `--time-start <ms>`：时间窗口起点（毫秒，包含）。
- `--time-end <ms>`：时间窗口终点（毫秒，不包含）。

示例：

```powershell
InsightCli.exe C:/traces/run01.utrace counters series --name DynamicNaniteScalingPrimary_Fraction --time-start 0 --time-end 5000
```

示例输出：

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

用途：
- 查询单个 counter 的聚合统计。

参数：
- `--name <counter_name>`（必填）：counter 名称，需来自 `counters list`。

示例：

```powershell
InsightCli.exe C:/traces/run01.utrace counters stats --name DynamicNaniteScalingPrimary_Fraction
```

示例输出：

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

未知 counter 示例：

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

用途：
- 返回内存摘要指标。

参数：
- `--time-start <ms>`：时间窗口起点（毫秒，包含）。
- `--time-end <ms>`：时间窗口终点（毫秒，不包含）。

示例：

```powershell
InsightCli.exe C:/traces/run01.utrace memory summary
```

示例输出：

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

用途：
- 返回内存峰值及邻域上下文。

参数：
- `--time-start <ms>`：时间窗口起点（毫秒，包含）。
- `--time-end <ms>`：时间窗口终点（毫秒，不包含）。

示例：

```powershell
InsightCli.exe C:/traces/run01.utrace memory peak
```

示例输出：

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

用途：
- 返回内存时序数据。

参数：
- `--time-start <ms>`：时间窗口起点（毫秒，包含）。
- `--time-end <ms>`：时间窗口终点（毫秒，不包含）。
- `--limit <n>`：过滤后返回点数上限。

示例：

```powershell
InsightCli.exe C:/traces/run01.utrace memory series --time-start 0 --time-end 5000 --limit 5
```

示例输出：

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

用途：
- 返回按标签聚合的内存占用。

参数：
- `--limit <n>`：返回内存标签数量上限。

示例：

```powershell
InsightCli.exe C:/traces/run01.utrace memory tags --limit 3
```

示例输出：

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

用途：
- 返回内存分配热点（Top N）。

参数：
- `--limit <n>`：返回记录数量上限。
- `--by <tag|callstack>`：分组方式，默认 `tag`。

示例：

```powershell
InsightCli.exe C:/traces/run01.utrace memory alloc-top --by tag --limit 5
```

示例输出：

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

说明：
- `--by callstack` 当前会返回空 `data`，并在 `meta.warning` 中提示原因。

## 5.20 `memory leak-suspect`

用途：
- 在最近时间窗口内识别持续增长的内存标签。

参数：
- `--window <sec>`（必填）：回溯窗口（秒）。
- `--limit <n>`：返回记录数量上限。

示例：

```powershell
InsightCli.exe C:/traces/run01.utrace memory leak-suspect --window 5 --limit 5
```

示例输出：

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

用途：
- 按关键字检索 bookmark/log 事件，支持组合过滤。

参数：
- `--keyword <text>`（必填）：在 message 字段中检索的文本。
- `--case-sensitive`：启用大小写敏感匹配，默认不敏感。
- `--exact`：启用整串精确匹配，默认是子串匹配。
- `--category <bookmark|log|...>`：按 marks 类别过滤。
- `--channel <name>`：按 channel 名过滤。
- `--thread-id <id>`：按数值线程 ID 过滤。
- `--time-start <ms>`：时间窗口起点（毫秒，包含）。
- `--time-end <ms>`：时间窗口终点（毫秒，不包含）。

示例：

```powershell
InsightCli.exe C:/traces/run01.utrace marks search --keyword load --category log --channel Streaming --time-start 0 --time-end 5000
```

示例输出：

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

用途：
- 返回某时间点附近窗口内的 marks。

参数：
- `--timestamp <ms>`（必填）：中心时间点（毫秒）。
- `--window <ms>`（必填）：半窗口宽度，实际区间为 `[timestamp-window, timestamp+window)`。
- `--category <...>`：在 around 窗口内再按类别过滤。
- `--channel <...>`：在 around 窗口内再按 channel 过滤。
- `--thread-id <...>`：在 around 窗口内再按线程过滤。
- `--time-start <ms>`：附加下界，会与 around 窗口求交集。
- `--time-end <ms>`：附加上界，会与 around 窗口求交集。

示例：

```powershell
InsightCli.exe C:/traces/run01.utrace marks around --timestamp 12000 --window 500 --channel Streaming
```

示例输出：

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

## 5.11 `loadtime` 命令

用途：
- 基于 trace 的 `LoadTime` 通道分析资产/包加载耗时。

命令：
- `loadtime summary`
- `loadtime packages --limit <n> --sort-by total|serialize|postload`
- `loadtime slowest --limit <n>`
- `loadtime timeline --time-start <ms> --time-end <ms>`

说明：
- 当 trace 未包含 LoadTime 通道时，命令会成功返回空数据，并在 `meta.channel_state` 给出提示。
- `loadtime timeline` 会在 `meta` 中回显时间窗口信息。

## 5.12 `gc` 命令

用途：
- 以结构化方式输出 trace 中的垃圾回收事件。

命令：
- `gc summary`
- `gc events --limit <n>`
- `gc longest --limit <n>`

说明：
- 当前 GC 识别依赖 CPU scope 名称 pattern，并通过 `meta.source=cpu_scope_pattern` 标识来源。
- 该匹配方式受引擎版本与 trace 埋点影响，覆盖范围可能变化。

## 6. 实用工作流

### 6.1 慢帧排查

```powershell
InsightCli.exe <trace> frames slowest --limit 5
InsightCli.exe <trace> cpu top --thread GameThread --limit 10
InsightCli.exe <trace> gpu top --limit 10
InsightCli.exe <trace> threads waits --frame-index <frame>
InsightCli.exe <trace> tasks top --frame-index <frame>
```

### 6.2 基于 Counter 的回归检查

```powershell
InsightCli.exe <trace> counters list
InsightCli.exe <trace> counters series --name <counter> --time-start 0 --time-end 10000
InsightCli.exe <trace> counters stats --name <counter>
```

### 6.3 事件关联诊断

```powershell
InsightCli.exe <trace> marks search --keyword hitch
InsightCli.exe <trace> marks around --timestamp <ms> --window 300
InsightCli.exe <trace> frames detail --frame-index <frame>
```

## 7. 验证命令

构建：

```powershell
Engine/Build/BatchFiles/Build.bat InsightCli Win64 Development
```

冒烟测试：

```powershell
powershell -ExecutionPolicy Bypass -File Engine/Source/Programs/InsightCli/Tests/Smoke/InsightCli.Output.Smoke.ps1 -ExePath Engine/Binaries/Win64/InsightCli.exe
```

## 8. 说明

- 本手册中的输出示例为代表性示例，具体数值随 trace 数据而变化。
- 在自动化场景中建议按 JSON 字段解析，不要依赖纯文本匹配。
- 若需做 schema 稳定性守护，建议配合 smoke/golden 测试。
