# gRPC 与 HTTP 输入支持需求规格说明

> 状态：已确认
> 创建日期：2026-06-19
> 所属里程碑：无

## 1. 概述

### 1.1 功能简述
将 Zurg Agent 的输入能力扩展为同时支持基于 asio-grpc 的异步 gRPC 控制通道与内嵌 HTTP REST 输入，并统一映射到现有任务执行能力。

### 1.2 所属模块
所属模块包括 `standalone::agent` 运行时、gRPC 控制通道、HTTP 输入服务、任务调度队列、任务持久化、核心任务库与配置加载。

### 1.3 关联文档
- `README.md`
- `CallbackClient.README.md`
- `documentation/callback_tasks.md`
- `third_party/os.proto`
- `standalone/config/zurg_agent.yaml`

## 2. 用户故事与场景

### 2.1 目标用户
- 运维控制面服务开发者
- 运维平台前端或浏览器调试用户
- Zurg Agent 部署与维护人员
- 自动化测试与集成测试维护人员

### 2.2 用户故事
- 作为运维控制面服务开发者，我希望 Agent 使用基于 asio-grpc 的异步 gRPC 客户端连接控制面，以便保持现有控制协议语义并获得统一异步运行时能力。
- 作为运维平台前端或浏览器调试用户，我希望通过 HTTP REST 调用 Agent 的任务与进程信息接口，以便在浏览器或脚本中查询状态、提交任务、取消任务和读取结果。
- 作为 Agent 维护人员，我希望 gRPC 与 HTTP 输入复用同一套任务调度、功能开关、结果语义与持久化规则，以便避免两套输入产生行为差异。

### 2.3 使用场景
1. gRPC 控制面下发任务
   - 前置条件：Agent 已读取配置文件，gRPC 目标地址已配置，控制面服务可连接。
   - 期望结果：Agent 通过 asio-grpc 异步客户端建立控制通道，接收 `StartOp / CancelOp / Shutdown / Heartbeat`，并按现有生命周期返回 `OpAck / OpData / OpEof / OpError`。

2. HTTP REST 提交和查询任务
   - 前置条件：Agent 已启动 HTTP 服务，HTTP 监听地址和端口来自配置文件。
   - 期望结果：HTTP 调用方可提交 `LogFilter / Pcap / Exec` 任务，随后通过任务状态和任务结果接口轮询执行进度与最终结果。

3. 浏览器查询运行状态和进程信息
   - 前置条件：Agent 已启动 HTTP 服务，调用方可访问监听地址。
   - 期望结果：浏览器或脚本可获取 Agent 自身进程信息、系统进程列表、CPU/内存信息、当前运行任务、历史任务、配置状态和健康状态。

## 3. 功能需求

### 3.1 输入
1. gRPC 输入
   - 输入来源：远端 gRPC 控制面。
   - 输入协议：复用 `third_party/os.proto` 中的 `Control.Connect` 双向流控制协议。
   - 输入消息：`StartOp`、`CancelOp`、`Shutdown`、`Heartbeat`。
   - 必填配置：gRPC 控制面目标地址、Agent ID。
   - 选填配置：连接重试、功能开关、日志、鉴权模式、任务持久化路径。

2. HTTP REST 输入
   - 输入来源：HTTP 调用方，包括浏览器、curl、自动化脚本或本机运维工具。
   - 输入协议：HTTP REST。
   - 输入格式：请求体使用 JSON；二进制任务结果通过结果接口按 REST 语义读取。
   - 必填配置：HTTP 监听地址、HTTP 监听端口、任务持久化路径。
   - 任务提交输入：调用方提供 `op_id`，并提供 `LogFilter`、`Pcap` 或 `Exec` 任务参数。
   - 任务取消输入：调用方提供待取消任务的 `op_id`。
   - 查询输入：调用方提供任务标识、查询条件或进程信息查询目标。

3. 配置输入
   - gRPC 相关运行参数放入配置文件。
   - HTTP 监听地址和端口放入配置文件。
   - 任务持久化目录或文件位置放入配置文件。
   - 现有功能开关继续适用于 gRPC 与 HTTP 两类输入。

### 3.2 输出
1. gRPC 输出
   - 输出目标：远端 gRPC 控制面。
   - 输出格式：复用当前 `AgentToServer` 消息语义。
   - 成功输出：任务按 `OpAck -> OpData -> OpEof` 生命周期返回。
   - 失败输出：任务通过 `OpAck(accepted=false)` 或 `OpError` 返回失败原因。

2. HTTP 输出
   - 输出目标：HTTP 调用方。
   - 输出格式：JSON 元数据、任务状态、任务结果索引、错误信息和进程信息。
   - 成功提交任务：返回任务已接受状态、`op_id`、任务类型和初始状态。
   - 重复任务：返回拒绝结果，并说明 `op_id` 已存在或仍处于活跃生命周期。
   - 查询任务状态：返回任务当前生命周期状态、创建时间、更新时间、来源输入、任务类型和错误摘要。
   - 查询任务结果：返回与 gRPC 生命周期一致的 ACK、数据、EOF 或错误结果记录。
   - 查询进程信息：返回 Agent 自身进程信息、系统进程列表、CPU/内存信息、当前运行任务、历史任务、配置状态或健康状态。

3. 持久化输出
   - Agent 必须将任务生命周期、任务状态、任务结果元数据和可恢复的任务输出持久化到磁盘。
   - Agent 重启后，HTTP 查询接口必须能读取历史任务记录。
   - 持久化失败必须作为可观测错误记录到日志，并在相关 HTTP 响应或任务错误中体现。

### 3.3 核心行为描述
1. Agent 启动时读取配置文件，并根据配置同时初始化 gRPC 异步客户端和 HTTP REST 服务。
2. gRPC 控制通道从当前 gRPC Callback API 客户端替换为基于 asio-grpc 的异步客户端。
3. 替换后，gRPC 控制通道必须保持现有协议语义，包括连接后发送 `AgentHello`、处理心跳、处理任务启动、处理取消、处理关机和断线重连。
4. HTTP REST 输入必须映射到现有三类任务：`LogFilter`、`Pcap`、`Exec`。
5. gRPC 输入与 HTTP 输入必须复用同一套串行任务队列。
6. 同一时刻最多执行一个任务；多个任务到达时按进入队列顺序执行。
7. 任务 `op_id` 由输入调用方提供。Agent 使用 `op_id` 进行去重、状态关联、取消和结果查询。
8. 已存在或仍处于活跃生命周期的 `op_id` 再次提交时，Agent 必须拒绝新任务。
9. HTTP 任务提交采用异步模式：提交成功仅代表任务进入生命周期，不代表任务已完成。
10. HTTP 调用方通过任务状态接口和任务结果接口查询任务进度与结果。
11. HTTP 取消接口必须能取消排队任务或正在执行的任务。
12. gRPC `CancelOp` 与 HTTP 取消接口必须进入同一套取消逻辑。
13. `Shutdown` 行为继续按当前协议语义处理；HTTP 服务不定义新的全局关闭能力，除非后续需求另行确认。
14. HTTP 进程信息接口必须支持查询 Agent 自身进程信息、系统进程列表、CPU/内存信息、当前运行任务、历史任务、配置状态和健康状态。
15. `features.log_filter`、`features.pcap`、`features.exec` 必须同时约束 gRPC 与 HTTP 输入。
16. 当 `features.exec` 关闭时，gRPC 与 HTTP 的 `Exec` 任务都必须被拒绝。
17. 当 HTTP 触发 `Exec` 时，必须沿用当前 Exec 任务限制和错误语义。
18. 所有任务结果生命周期必须与当前 gRPC 输出语义一致，允许 HTTP 以 REST 资源形式表达相同的 ACK、DATA、EOF 和 ERROR 状态。
19. Agent 必须记录 gRPC 输入、HTTP 输入、任务调度、取消、完成、失败、持久化错误和 HTTP 服务错误日志。

### 3.4 业务规则与约束
1. gRPC 与 HTTP 输入同时启用。
2. 本次不支持 TLS。
3. 本次 HTTP REST 明确不提供鉴权能力。
4. HTTP 服务应通过配置绑定到指定监听地址；默认部署建议由使用方配置为本地或受信网络地址。
5. 现有 `--target`、`--agent_id`、`--config` 参数不要求作为长期唯一配置入口；gRPC 与 HTTP 相关参数可以迁移到配置文件。
6. 配置文件必须成为 gRPC 与 HTTP 输入参数的主要来源。
7. `third_party/os.proto` 允许修改，但必须复用当前控制协议，不引入不兼容的任务语义。
8. `LogFilter`、`Pcap`、`Exec` 的业务含义与当前实现保持一致。
9. 任务持久化必须覆盖 HTTP 查询历史任务的需求。
10. Agent 重启后，不要求恢复执行重启前未完成的任务；未完成任务必须以可查询的终止状态呈现。
11. HTTP 输入不得绕过现有功能开关、任务校验和取消机制。
12. 任务结果中包含本地路径、命令输出、进程信息和配置状态时，调用方需自行承担无鉴权访问风险；本次需求只要求如实暴露所需信息。

## 4. 非功能需求

### 4.1 性能要求
- gRPC `StartOp` 接收后应保持当前快速 ACK 行为。
- HTTP 任务提交应快速返回任务已接受或拒绝结果，不等待任务执行完成。
- gRPC 与 HTTP 输入处理不得阻塞任务执行线程。
- 任务结果持久化不得破坏串行任务执行顺序。

### 4.2 安全要求
- 本次明确不提供 HTTP 鉴权。
- 本次明确不支持 TLS。
- HTTP 暴露 `Exec`、进程信息、配置状态和任务结果，因此部署时必须由配置控制监听地址。
- 功能开关必须对 HTTP 与 gRPC 同时生效。

### 4.3 兼容性要求
- 保持现有 `Control.Connect` 控制协议语义。
- 保持现有 `LogFilter / Pcap / Exec` 任务行为。
- 保持现有配置中的功能开关语义。
- 保持现有测试覆盖目标，并新增 gRPC asio-grpc、HTTP REST、任务持久化和进程信息查询相关测试。
- 项目整体继续覆盖核心库、standalone agent、测试工程和文档。

## 5. 边界与限制

### 5.1 明确包含（In Scope）
- 引入 asio-grpc，并将当前 gRPC Callback 客户端替换为基于 asio-grpc 的异步 gRPC 客户端。
- 引入 cpp-httplib，作为 Agent 内嵌 HTTP REST 服务。
- gRPC 与 HTTP 输入同时启用。
- HTTP REST 支持任务提交、任务状态查询、任务取消、任务结果读取、进程信息查询和健康检查。
- HTTP REST 映射 `LogFilter`、`Pcap`、`Exec` 三类任务。
- gRPC 与 HTTP 复用当前串行任务队列。
- gRPC 与 HTTP 复用当前任务功能开关。
- 任务生命周期和结果语义保持与当前 gRPC `AgentToServer` 一致。
- 任务历史与可恢复结果持久化到磁盘。
- Agent 重启后可通过 HTTP 查询历史任务。
- 配置文件提供 gRPC、HTTP 和持久化相关配置。
- 修改 `third_party/os.proto` 以满足复用当前协议所需的兼容扩展。
- 更新核心库、standalone agent、测试工程和文档。
- 提供可通过 curl、浏览器和 gRPC 客户端验证的端到端能力。

### 5.2 明确排除（Out of Scope）
- HTTP TLS。
- HTTP 鉴权、API key、Bearer token、mTLS 或用户权限体系。
- HTTP 同步阻塞式任务执行接口。
- 多任务并行执行。
- Agent 重启后恢复执行未完成任务。
- 新增 `LogFilter / Pcap / Exec` 之外的任务类型。
- 独立的 Web 前端页面。
- 将 cpp-httplib 用作 HTTP client。
- 替换现有任务业务逻辑为全新实现。

### 5.3 已知约束
- HTTP 无鉴权会暴露敏感操作和敏感信息，只适合受控网络或本地绑定场景。
- `Exec` 任务通过 HTTP 暴露后风险较高，必须受现有功能开关约束。
- `Pcap` 任务仍受系统权限、网卡权限、libpcap 能力和运行环境限制。
- 持久化到磁盘会引入结果清理、容量控制和损坏恢复需求；本次必须定义可观测错误，但不要求实现复杂归档策略。
- asio-grpc 替换当前 gRPC Callback API 后，现有回调客户端相关测试需要调整为新的异步运行时验证方式。

## 6. 验收标准（Acceptance Criteria）

1. gRPC 异步客户端替换
   - **Given** Agent 配置了可连接的 gRPC 控制面地址
   - **When** Agent 启动
   - **Then** Agent 使用基于 asio-grpc 的异步客户端建立控制通道，并发送 `AgentHello`

2. gRPC 任务执行
   - **Given** Agent 已连接 gRPC 控制面
   - **When** 控制面下发 `LogFilter`、`Pcap` 或 `Exec` 的 `StartOp`
   - **Then** Agent 按现有生命周期返回 `OpAck`、任务数据和最终 `OpEof` 或 `OpError`

3. HTTP 任务提交
   - **Given** Agent 已启动 HTTP REST 服务
   - **When** HTTP 调用方提交带有 `op_id` 的 `LogFilter`、`Pcap` 或 `Exec` 任务
   - **Then** Agent 返回任务已接受或拒绝结果，并且不等待任务执行完成

4. HTTP 任务状态查询
   - **Given** HTTP 任务已提交
   - **When** 调用方查询任务状态
   - **Then** Agent 返回任务生命周期状态、任务类型、创建时间、更新时间和错误摘要

5. HTTP 任务结果读取
   - **Given** HTTP 任务已产生输出或已结束
   - **When** 调用方读取任务结果
   - **Then** Agent 返回与 gRPC 生命周期一致的 ACK、DATA、EOF 或 ERROR 结果记录

6. HTTP 任务取消
   - **Given** HTTP 任务处于排队或执行中
   - **When** 调用方请求取消该任务
   - **Then** Agent 通过统一取消逻辑取消任务，并在任务结果中记录取消错误或终止状态

7. 串行队列复用
   - **Given** gRPC 与 HTTP 在短时间内提交多个任务
   - **When** 多个任务同时处于活跃生命周期
   - **Then** Agent 同一时刻只执行一个任务，并按队列顺序处理

8. 重复 `op_id` 拒绝
   - **Given** 已存在活跃任务 `op_id`
   - **When** gRPC 或 HTTP 再次提交相同 `op_id`
   - **Then** Agent 拒绝新任务并返回明确拒绝原因

9. 功能开关约束
   - **Given** 配置关闭 `exec` 功能
   - **When** gRPC 或 HTTP 提交 `Exec` 任务
   - **Then** Agent 拒绝任务并返回功能不可用原因

10. 进程信息查询
    - **Given** Agent 已启动 HTTP REST 服务
    - **When** 浏览器或 HTTP 调用方查询进程信息
    - **Then** Agent 返回自身进程信息、系统进程列表、CPU/内存信息、当前运行任务、历史任务、配置状态和健康状态

11. 任务持久化
    - **Given** Agent 执行过任务并写入持久化存储
    - **When** Agent 重启后 HTTP 调用方查询历史任务
    - **Then** Agent 返回重启前已持久化的历史任务记录

12. 无鉴权行为
    - **Given** HTTP REST 服务已启动
    - **When** 调用方不携带任何鉴权信息访问 HTTP 接口
    - **Then** Agent 按接口规则处理请求，不因缺少鉴权信息拒绝

13. 配置文件控制
    - **Given** 配置文件包含 gRPC、HTTP 和持久化参数
    - **When** Agent 启动
    - **Then** Agent 按配置启用 gRPC 输入、HTTP 输入和任务持久化

14. 端到端验证
    - **Given** 项目完成构建
    - **When** 使用 gRPC 客户端、curl 和浏览器分别验证输入能力
    - **Then** 三类任务、取消、状态查询、结果读取、进程信息查询和健康检查均可按本 spec 行为完成

## 7. 开放问题（Open Questions）

无。

## 8. 变更记录

| 日期 | 变更内容 | 变更人 |
|------|---------|--------|
| 2026-06-19 | 创建 gRPC 与 HTTP 输入支持需求规格草稿 | Codex |
