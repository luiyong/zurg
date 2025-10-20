# Callback Tasks Overview

## Runtime Structure
- `ControlCallbackClient` implements the gRPC callback reactor and acts as the `TaskContext`; it is responsible for queueing work, dispatching tasks to the worker thread, and streaming responses (`standalone/source/agent/agent_impl.cpp`).
- Task definitions live under `standalone/source/agent/tasks/` and currently include `LogFilterTask`, `PcapTask`, and `ExecTask`. Each task handles its own execution lifecycle, cancellation checks, and reporting through the context interface.
- Tasks transition through `Pending → Running → Completed/Cancelled/Failed`; cancellation requests are honoured both from explicit `CancelOp` and from shutdown/stream tear-down paths.

## Supported Task Types
### LogFilterTask
- Streams filtered log content based on `ops::v1::LogFilterSpec` by calling `zurg::log_ops::StreamLogFilter` (`standalone/source/agent/tasks/log_filter_task.cpp`).
- Supports time-window, level, rotation, and substring filters, limits output size, and emits `LogChunk` data followed by `LogFilterEof` metadata.
- Cancellation stops the streaming loop and reports an error with code `CANCELLED`.

### PcapTask
- Bridges `ops::v1::PcapSpec` requests to `zurg::pcap_ops::StreamCapture` (`standalone/source/agent/tasks/pcap_task.cpp`).
- 写入临时 PCAP 文件并通过 `LogChunk` 块上传完整文件内容，随后在 `OpEof.pcap` 中返回统计信息；上传结束后自动清理临时文件（可配置）。
- Observes cancellation requests and propagates any capture errors back through `OpError`。

### ExecTask
- Serves `ops::v1::ExecSpec` requests oriented around interface inspection (e.g. `ip addr`), collecting all UP interfaces and their IP addresses (`standalone/source/agent/tasks/exec_task.cpp`).
- Emits data through `OpData.exec_chunk` and concludes with `OpEof.exec` (`ExecExit`). Unsupported commands return `UNIMPLEMENTED`.
- Cancellation halts execution early and reports `CANCELLED` to the server.

## Scheduling & Queueing
- Tasks are pushed onto a FIFO queue when a `StartOp` arrives; duplicates are rejected and drain mode prevents new work.
- A dedicated worker thread pops tasks, marks them running, and executes `Task::Run`. Completion removes the task from the tracking map, ensuring sequential execution even under bursty `StartOp` traffic.
- `Shutdown(drain=false)` and stream closures call `CancelAllLocked`, which cancels remaining tasks and emits `OpError(CANCELLED)` for queued work.

## Test Coverage
### Unit Tests
- `LogOpsTest.FiltersByTimeLevelAndSubstring` verifies basic time/level/content filtering and EOF metadata (`test/source/log_ops_test.cpp`).
- `LogOpsTest.FiltersAcrossRotations` covers multi-file windows with rotation scanning.
- `LogOpsTest.RespectsOutputLimit` exercises the output cap path, expecting a `RESOURCE_EXHAUSTED` status.
- `PcapOpsTest` suite validates parameter checking, packet production limits, and timeout handling (`test/source/pcap_ops_test.cpp`).
- Exec behaviour currently relies on the integration suite; additional unit coverage can be added as more commands are introduced.

### Integration Tests
- `CallbackAgentIntegrationTest.HandlesLogFilterAndShutdown` checks the happy path: Start → data streaming → EOF → controlled shutdown (`test/source/callback_agent_integration_test.cpp`).
- `CallbackAgentIntegrationTest.CancelsLogTask` issues `StartOp` followed by `CancelOp`, ensuring the task reports cancellation and the worker exits cleanly.
- `CallbackAgentIntegrationTest.SequentialLogTasksExecuteInOrder` enqueues two log tasks back-to-back, asserting that all data/EOF messages for the first task are emitted before the second begins streaming.
- `CallbackAgentIntegrationTest.HandlesExecTaskCollectsInterfaces` ensures the exec task streams interface information and reports a zero exit code.

These scenarios ensure the shared task queue, cancellation/cleanup logic, and per-task streaming behaviour are exercised end-to-end. Future task types can plug into the same interface without altering the scheduler.
