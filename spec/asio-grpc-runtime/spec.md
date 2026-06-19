# asio-grpc 控制流与 Agent 运行时编排需求规格说明

> 状态：草稿
> 创建日期：2026-06-19
> 所属里程碑：无

## 1. 概述

### 1.1 功能简述
将 standalone Agent 的 gRPC 控制通道完整迁移到基于 asio-grpc 的双向流客户端，并由 `agent_main` 统一编排 gRPC、HTTP、任务调度、持久化和停止流程。

### 1.2 所属模块
所属模块包括 `standalone::agent` 运行时入口、`ControlAsioClient`、共享 `TaskScheduler`、`GrpcTaskSink`、JSONL `TaskStore`、HTTP Server 和配置加载模块。

### 1.3 关联文档
- `spec/grpc-http-input/spec.md`
- `spec/grpc-http-input/plan.md`
- `third_party/os.proto`
- `standalone/source/agent/control/control_asio_client.*`
- `standalone/source/agent/runtime/*`
- `standalone/source/agent/http/http_server.*`
- `standalone/source/agent_main.cpp`

## 2. 用户故事与场景

### 2.1 目标用户
- 运维控制面服务开发者
- Zurg Agent 部署与维护人员
- 自动化测试与集成测试维护人员

### 2.2 用户故事
- 作为运维控制面服务开发者，我希望 Agent 使用 asio-grpc 建立 `Control.Connect` 双向流，以便保持现有控制协议并移除旧 gRPC Callback 客户端依赖。
- 作为 Agent 维护人员，我希望 gRPC 断线后按既有 backoff 规则重连，以便网络波动时 Agent 能自动恢复控制通道。
- 作为 Agent 部署人员，我希望 `agent_main` 同时启动 gRPC、HTTP、任务持久化和调度器，并在停止时有序释放资源，以便运行行为可预测。

### 2.3 使用场景
1. gRPC 控制面正常连接
   - 前置条件：配置文件启用 gRPC 输入，控制面地址可访问。
   - 期望结果：Agent 建立 `Control.Connect` 双向流，连接建立后发送 `AgentHello`，随后处理控制面下发的心跳、任务、取消和关机消息。

2. gRPC 控制面断开后重连
   - 前置条件：Agent 正在运行，gRPC 控制面连接异常关闭。
   - 期望结果：Agent 取消当前连接生命周期内未完成的活跃任务，按 backoff 策略等待后重新建立控制通道，并在重连成功后再次发送 `AgentHello`。

3. Agent 统一启动与停止
   - 前置条件：配置文件启用 gRPC、HTTP 和持久化。
   - 期望结果：`agent_main` 初始化配置、日志、持久化、任务调度器、HTTP Server 和 gRPC Client；收到停止信号或 shutdown 后有序停止各组件。

## 3. 功能需求

### 3.1 输入
1. gRPC 双向流输入
   - 来源：远端控制面服务。
   - 协议：`ops.v1.Control.Connect`。
   - 消息：`StartOp`、`CancelOp`、`Shutdown`、`Heartbeat`。
   - 必填配置：gRPC 启用开关、控制面目标地址、Agent ID。
   - 选填配置：重连 backoff 参数、keepalive 参数、任务功能开关、持久化路径。

2. 运行时控制输入
   - 来源：进程信号、`Shutdown` 消息、内部停止请求。
   - 输入类型：SIGINT、SIGTERM、gRPC `Shutdown`、本地 `RequestStop`。
   - 必填行为：所有停止输入必须进入统一停止流程。

3. 配置输入
   - 来源：YAML 配置文件和保留的命令行参数。
   - gRPC、HTTP、持久化、日志、功能开关和 auth 配置由配置文件提供。
   - 命令行 `--target` 和 `--agent_id` 继续可用；显式命令行值优先于配置文件目标地址。

### 3.2 输出
1. gRPC 输出
   - 成功连接后输出 `AgentHello`。
   - 收到 `Heartbeat` 后输出对应 `Heartbeat` pong。
   - 任务接受或拒绝时输出 `OpAck`。
   - 任务执行中输出 `OpData`。
   - 任务成功结束输出 `OpEof`。
   - 任务失败或取消输出 `OpError`。

2. HTTP 和持久化输出
   - HTTP Server 可读取同一个 `TaskScheduler` 和 `TaskStore` 暴露的状态。
   - JSONL 持久化记录 gRPC 和 HTTP 两类输入产生的任务事件。

3. 日志输出
   - gRPC 连接、断线、重连、写失败、读失败、任务提交、任务取消、shutdown、HTTP 启停、持久化错误均需要有日志记录。

### 3.3 核心行为描述
1. `ControlAsioClient` 必须使用 asio-grpc 建立 `Control.Connect` 双向流，替代旧 `ControlCallbackClient` 和 `ControlStreamClient` 的运行时职责。
2. gRPC 流建立后，Agent 必须首先发送 `AgentHello`，然后开始并发处理读取和写入。
3. gRPC 读取路径必须把 `ServerToAgent` 消息转换为共享运行时操作：
   - `StartOp` 提交到 `TaskScheduler`。
   - `CancelOp` 调用 `TaskScheduler` 取消指定任务。
   - `Shutdown(drain=true)` 进入 drain 停止流程。
   - `Shutdown(drain=false)` 取消活跃任务并停止 gRPC 控制流。
   - `Heartbeat` 返回 pong，不进入任务队列。
4. gRPC 写入路径必须消费 `GrpcTaskSink` 产生的 `AgentToServer` 消息，并保证同一条流上任意时刻最多一个写操作进行中。
5. 当任务事件由 `TaskScheduler` 产生时，`GrpcTaskSink` 必须将 ACK、DATA、EOF、ERROR 转换成当前 proto 的 `AgentToServer` 消息。
6. gRPC 流异常关闭时，Agent 必须取消或终止该流生命周期内尚未完成的 gRPC 输入任务。
7. gRPC 流关闭后，只要 `should_run` 仍为 true 且未收到终止性 shutdown，Agent 必须按 backoff 策略重连。
8. 每次重新建立流后，Agent 必须重新发送 `AgentHello`。
9. 重连等待期间必须响应进程停止请求；停止请求到达后不得继续建立新流。
10. `agent_main` 必须创建并持有共享 `TaskScheduler`、`TaskStore`、`TaskQueryService`、HTTP Server、gRPC Client 和 auth manager。
11. `agent_main` 必须先打开持久化存储，再启动任务调度器和输入服务。
12. `agent_main` 必须根据配置同时启用 gRPC 和 HTTP；配置关闭某个输入时，不启动对应服务。
13. 停止流程必须按顺序停止 gRPC Client、HTTP Server、TaskScheduler、AuthManager，并确保线程 join 完成后进程退出。
14. 旧 Callback 客户端不得再作为 standalone Agent 的默认 gRPC 输入路径。

### 3.4 业务规则与约束
1. gRPC 控制协议必须继续复用 `third_party/os.proto` 的 `Control.Connect`。
2. 任务执行仍复用当前 `LogFilter`、`Pcap`、`Exec` 三类任务。
3. gRPC 与 HTTP 必须复用同一个任务调度器和持久化存储。
4. 功能开关必须同时约束 gRPC 和 HTTP 输入。
5. gRPC 重连不得重放断线前已经失败、取消或终止的任务。
6. Agent 重启后不恢复执行重启前未完成任务。
7. 本次不引入 TLS、鉴权变更或新 proto service。

## 4. 非功能需求

### 4.1 性能要求
- gRPC `StartOp` 到达后必须保持快速 ACK 行为，ACK 不等待任务执行完成。
- gRPC 读回调或协程不得执行阻塞任务逻辑。
- gRPC 写队列不得并发写同一条 stream。
- HTTP 查询不得阻塞 gRPC 流读写。

### 4.2 安全要求
- 本功能不新增 TLS。
- 本功能不新增 HTTP 鉴权。
- `Exec` 暴露仍必须受 `features.exec` 控制。

### 4.3 兼容性要求
- 保持 `AgentHello`、`Heartbeat`、`StartOp`、`CancelOp`、`Shutdown`、`OpAck`、`OpData`、`OpEof`、`OpError` 的协议语义。
- 保持现有 callback 集成测试覆盖的行为语义，并迁移到新 asio-grpc 客户端路径。
- 保持 `--target`、`--agent_id`、`--config` 可用。

## 5. 边界与限制

### 5.1 明确包含（In Scope）
- asio-grpc 双向流连接、读、写、取消和关闭。
- gRPC 写队列顺序化。
- `AgentHello` 首包发送。
- `Heartbeat` pong 响应。
- `StartOp / CancelOp / Shutdown` 到 `TaskScheduler` 的映射。
- `GrpcTaskSink` 到 gRPC stream 写入路径。
- gRPC 断线后的任务终止和 backoff 重连。
- `agent_main` 新运行时编排。
- gRPC、HTTP、TaskScheduler、TaskStore 的统一生命周期管理。
- 旧 Callback 客户端从 standalone 默认路径移除。

### 5.2 明确排除（Out of Scope）
- TLS/mTLS。
- 新增认证授权协议。
- 新增任务类型。
- 多任务并行执行。
- 断线任务重放。
- Agent 重启后恢复执行未完成任务。
- Web UI 页面。

### 5.3 已知约束
- asio-grpc API 必须以本地 `third_party/asio-grpc` 版本为准。
- gRPC 本地端口监听测试在沙箱环境可能失败，需要在允许监听 socket 的环境中运行集成测试。
- 当前已有 `TaskScheduler`、`TaskStore`、HTTP Server 和 `GrpcTaskSink` 基础实现，但 `ControlAsioClient` 仍是占位骨架。

## 6. 验收标准（Acceptance Criteria）

1. gRPC 建连与 Hello
   - **Given** 控制面服务可连接且 Agent 启用 gRPC 输入
   - **When** Agent 启动
   - **Then** Agent 建立 `Control.Connect` 双向流并发送 `AgentHello`

2. gRPC 心跳
   - **Given** Agent 已建立 gRPC 双向流
   - **When** 控制面发送 `Heartbeat`
   - **Then** Agent 返回相同序号的 pong

3. gRPC 任务提交
   - **Given** Agent 已建立 gRPC 双向流
   - **When** 控制面发送 `StartOp`
   - **Then** Agent 通过 `TaskScheduler` 接受或拒绝任务，并返回 `OpAck`

4. gRPC 任务结果
   - **Given** gRPC 任务已被接受
   - **When** 任务产生数据、完成或失败
   - **Then** Agent 在同一条 gRPC stream 上返回对应 `OpData`、`OpEof` 或 `OpError`

5. gRPC 取消
   - **Given** gRPC 任务处于排队或执行中
   - **When** 控制面发送 `CancelOp`
   - **Then** Agent 取消对应任务，并返回取消相关错误事件

6. gRPC 重连
   - **Given** Agent 与控制面的 gRPC stream 异常关闭
   - **When** Agent 仍处于运行状态
   - **Then** Agent 按 backoff 策略重连，并在新流建立后再次发送 `AgentHello`

7. 停止期间不重连
   - **Given** Agent 收到 SIGTERM 或本地停止请求
   - **When** gRPC stream 关闭
   - **Then** Agent 不再发起新连接，并完成所有组件停止流程

8. agent_main 统一编排
   - **Given** 配置文件启用 gRPC、HTTP 和持久化
   - **When** Agent 启动
   - **Then** `agent_main` 打开 TaskStore，启动 TaskScheduler、HTTP Server 和 gRPC Client

9. agent_main 有序停止
   - **Given** Agent 已启动所有组件
   - **When** 收到停止信号
   - **Then** gRPC Client、HTTP Server、TaskScheduler 和 AuthManager 均停止并完成线程 join

10. 默认路径替换
    - **Given** standalone Agent 完成构建
    - **When** 运行 `zurg_agent`
    - **Then** 默认 gRPC 输入路径使用 `ControlAsioClient`，不再启动旧 Callback 客户端

## 7. 开放问题（Open Questions）

无。

## 8. 变更记录

| 日期 | 变更内容 | 变更人 |
|------|---------|--------|
| 2026-06-19 | 创建 asio-grpc 控制流与 Agent 运行时编排规格草稿 | Codex |
