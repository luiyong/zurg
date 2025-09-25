# gRPC Callback Client 设计概要

## 背景

- `os.proto` 的 `Control.Connect` 定义了 Agent 端与 Server 端的双向流式 RPC。
- 现有同步实现难以满足实时性与扩展性需求，需要基于 gRPC Callback API 重构客户端。
- Agent 必须按 Server 下发的 `StartOp` 顺序串行执行操作，同一时刻仅允许一个文件下载或抓包任务运行。

---

## 总体目标

1. 使用 gRPC Callback API (`grpc::ClientCallbackReaderWriter`) 构建新的 Control 客户端。
2. 连接建立后自动发送 `AgentHello`，处理 `Heartbeat/Shutdown/CancelOp`。
3. `StartOp` 进入串行队列，保障操作按序执行，执行时输出 `OpAck/OpData/OpEof/OpError`。
4. 支持任务取消、异常恢复、自动重连与指数回退。
5. 可复用现有 `file_ops`、`pcap_ops` 等模块实现具体任务。

---

## 架构概览

```
                ┌────────────────────────┐
                │ gRPC Callback Runtime   │
                └─────────────┬──────────┘
                              │
           ┌──────────────────┴──────────────────┐
           │  ControlCallbackClient (Reactive)   │
           └──────┬────────────────┬─────────────┘
                  │                │
         OnRead(ServerToAgent)     │ OnWriteDone/OnDone
                  │                │
                  ▼                │
        ┌────────────────────┐     │
        │   TaskDispatcher   │◄────┘
        └──────┬─────────────┘
               │
               ▼
        ┌─────────────┐
        │ Task Queue   │ (单线程串行执行)
        └────┬────────┘
             │
     ┌───────┴────────┐
     │ FileTaskRunner │ 复用 file_ops
     │ PcapTaskRunner │ 复用 pcap_ops
     │ ExecTaskRunner │ 复用 exec 模块
     └────────────────┘
```

---

## 核心模块

### 1. ControlCallbackClient

- **职责**：封装 gRPC Callback 流，连接建立、消息收发、重连控制。
- **关键回调**：
  - `OnReadInitialMetadata`：完成初始握手。
  - `OnReadDone`：解析 `ServerToAgent`，交由 `TaskDispatcher`。
  - `OnWriteDone`：跟踪发送完成，按需继续写入。
  - `OnDone`：处理流结束（异常/正常），触发重连或退出。
- **重连策略**：指数回退（复用现有 `ComputeBackoff` 逻辑），尊重 `Shutdown` 指令。

### 2. TaskDispatcher

- 持有串行任务队列与执行线程。
- 收到 `StartOp` 时：
  1. 立即回应 `OpAck(accepted=true)`。
  2. 构造 `Task`（包含 op_id、spec、状态句柄），压入队列。
  3. 唤醒执行线程，若空闲则马上运行。
- 收到 `CancelOp` 时：
  - 若任务在队列中，标记取消并发送 `OpError`/`OpEof`。
  - 若当前执行中，向执行器发送取消信号，由 task runner 轮询响应。
- 收到 `Shutdown` 时：
  - `drain=true`：执行完队列后停止收新任务，终止连接。
  - `drain=false`：立即取消当前任务并清空队列，返回结束。

### 3. Task Queue（串行执行）

- 实现为单线程消费的 `std::deque`，每次取出一个任务执行，保证同一时间最多一个任务。
- 文件下载与抓包任务共用这条队列，自然满足互斥要求。
- 任务执行过程中定期检查取消标志，并通过 `OpLog/OpProgress` 反馈状态。

### 4. Task Runner

- **FileTaskRunner**：调用 `file_ops::StreamFile`，将 chunk 封装为 `OpData`。
- **PcapTaskRunner**：调用 `pcap_ops::StreamCapture`，串流抓包数据。
- **ExecTaskRunner**：运行命令行（后续扩展），传输 stdout/stderr。
- 统一接口：`Run(TaskContext&) -> TaskResult`，负责在结束时发送 `OpEof` 或 `OpError`。

---

## 任务生命周期

1. `StartOp` 到达 → 排队 → `OpAck(accepted=true)`。
2. 任务出队，执行 runner：
   - 期间可多次发送 `OpLog`/`OpProgress`。
   - 产出数据时发送 `OpData`（FileChunk/PcapPacket/...）。
3. 成功完成 → `OpEof`（包含 FileGetEof/PcapStats 等）。
4. 异常或取消 → `OpError`（含 code/message）。

---

## 并发与同步

- Callback API 在 gRPC 线程池中运行，不直接阻塞；任务执行移交给内部线程，防止长时间阻塞回调。
- 对队列和状态使用互斥锁保护；当前任务指针提供给 CancelOp 查找。
- 发送操作（Write）通过顺序队列管理，避免并发写导致的 `FAILED_PRECONDITION`。

---

## 错误恢复

- gRPC 流异常关闭 → `OnDone` 检查状态，若未收到 `Shutdown` 指令，则触发重连流程。
- 任务执行错误 → `OpError` + 继续下一个任务。
- 连接断开时，如果队列仍有任务，可选择重连后重放或直接失败（视需求决定，默认失败并发送 `OpError`）。

---

## 测试策略

1. **单位测试**：
   - `TaskDispatcher` 队列行为、取消逻辑、Shutdown 分支。
   - Runner 的错误处理（使用假数据源）。

2. **Mock gRPC 测试**：
   - 利用 in-process server 模拟 `ServerToAgent` 消息，验证消息序列。

3. **端到端测试**：
   - 启动真实回环 Server，串行下发 FileGet/Pcap/Exec，并检查互斥与顺序。
   - 测试 Cancel 和 Shutdown（drain=true/false）行为。

4. **回归**：
   - 保留旧版同步客户端的核心测试，确保功能等价。

---

## 后续工作

- 代码实现与集成至项目（可能位于 `source/zurg/callback`）。
- 更新 `CMakeLists.txt` 添加新的库/可执行目标。
- 在文档或示例中说明如何启用 callback 客户端。
- 检查日志与监控集成（可结合 `LoggerManager`）。

---

## 验收清单映射（ops_acceptance_checklist.xlsx）

| ID      | 需求摘要                                   | 设计要点与保障措施 |
|---------|-------------------------------------------|--------------------|
| CTL-001 | Agent 连接后 2 秒内上报 `AgentHello`      | `ControlCallbackClient` 在 `OnWriteInit` 阶段立即发送 Hello，超时 watchdog 确保 `T_conn<=2s`。 |
| CTL-002 | `StartOp` 下发后 500 ms 内回复 `OpAck`    | Ack 在入队前立刻发送，队列执行与否不影响 Ack 延迟。 |
| CTL-003 | 同一流支持并发 ≥5 个操作、消息不混淆     | 队列接受任意数量的 `StartOp`，立即 Ack 并记录上下文；串行执行仅限制耗时操作并保持响应分组，响应通过 op_id 映射确保无交叉。 |
| CTL-004 | 心跳保活，连续 3 次缺失则判离线          | 心跳处理独立于任务队列；`TaskDispatcher` 不阻塞 `OnReadDone`，miss 计数器触发离线逻辑。 |
| CTL-005 | 重复 `StartOp` 识别并拒绝                 | 在任务表中缓存活跃/排队 op_id，重复请求直接发送 `OpAck(accepted=false)` 并记录原因。 |
| CTL-006 | `Shutdown(drain=true)` 时平滑退出          | 设置 drain 标志：允许当前任务完成并清空队列后关闭流；`drain=false` 则立即取消所有任务。

> 注：需求 CTL-003 要求“并发 >=5”指的是 Agent 能在短时间内接受多个 `StartOp`，并保证响应按 op_id 匹配。本文设计通过**快速 Ack + 串行执行器**满足互斥约束的同时保持协议并发性，必要时可扩展为多执行器以提高吞吐量。

---

## 当前进度 & 后续事项（2025-09-24）

- ✅ `ControlCallbackClient` 已完成，串行任务队列、取消、Shutdown 逻辑在现有单元测试中得到验证。
- ✅ 日志统一接入 `LoggerManager`，连接重试、StartOp/Ack/Data/Eof/Shutdown 等关键路径都会产生日志，可通过 `SetLoggerSinkForTests` 捕获。
- ⚠️ gRPC 版本仍为 1.51.x，缺少稳定的 Callback Server 注册接口。当前新增的集成测试 `CallbackAgentIntegrationTest.DISABLED_HandlesPcapStartAndShutdown` 默认禁用，仅保留示例代码；待升级到 gRPC ≥1.56 并统一 `GRPC_CALLBACK_API_NONEXPERIMENTAL` 后再启用。
- 📝 测试时可继续使用 `SetSendHookForTests`/`SetLoggerSinkForTests` 捕获客户端输出，验证 Ack/Data/Eof 序列与日志内容。
- ⏭️ 下一步：升级 gRPC（本地 & CI），启用 callback 服务端后重构 mock server，恢复并扩展端到端测试覆盖。

### 推荐容器环境

- 新增 `docker/Dockerfile.callback-ci`，基于 `ubuntu:24.04` 构建并安装 gRPC `${GRPC_VERSION}`（默认为 v1.62.0）、google-test、spdlog 等依赖，默认已满足 Callback API 需求。
- 构建镜像：
  ```bash
  docker build -t ghcr.io/<owner>/zurg-callback-ci -f docker/Dockerfile.callback-ci .
  ```
- 推送后可在 GitHub Actions 中直接使用：
  ```yaml
  jobs:
    build:
      runs-on: ubuntu-latest
      container:
        image: ghcr.io/<owner>/zurg-callback-ci:latest
  ```
