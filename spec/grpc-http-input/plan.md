# gRPC 与 HTTP 输入支持实施计划

> 关联 Spec：spec/grpc-http-input/spec.md
> 创建日期：2026-06-19
> 状态：已确认

## 1. 技术方案概述

本次改造将 `standalone::agent` 从当前“gRPC Callback 客户端直接承载任务调度和结果发送”的结构，拆分为输入无关的任务运行时、gRPC 输入适配器、HTTP REST 输入适配器和 JSONL 任务存储。任务运行时负责统一校验、去重、串行排队、取消、生命周期状态和结果事件分发；gRPC 与 HTTP 只负责把各自协议的输入转换成统一任务请求，并把任务事件转换成对应输出。

gRPC 控制通道替换为基于 asio-grpc 的 C++20 异步客户端，保留 `Control.Connect` 双向流协议、`AgentHello`、心跳、重连、`StartOp`、`CancelOp` 和 `Shutdown` 语义。HTTP 使用 cpp-httplib 内嵌服务，REST 请求/响应使用 nlohmann/json，任务提交采用异步模式，结果通过任务状态和结果接口轮询读取。

任务结果使用 JSONL 加结果文件目录持久化。JSONL 保存任务元数据、生命周期事件、ACK、EOF、ERROR 和数据索引；较大的二进制或分块 payload 写入独立结果文件，并在 JSONL 中记录引用。Agent 重启后加载历史任务索引，未完成任务标记为终止状态，HTTP 查询可以读取历史任务和结果。

## 2. 技术决策

### 2.1 方案选型

1. gRPC 异步方案
   - 方案 A：继续使用 gRPC Callback API。
     - 优点：现有代码改动较小。
     - 缺点：不满足 spec 中替换为 asio-grpc 的要求，HTTP 与 gRPC 难以共享同一异步模型。
   - 方案 B：升级 standalone agent 到 C++20，使用 asio-grpc 异步客户端。
     - 优点：满足 spec，便于统一异步运行时，读写流和重连逻辑更清晰。
     - 缺点：需要调整 CMake、测试和现有 callback client。
   - 最终选择：方案 B。

2. HTTP 服务方案
   - 方案 A：cpp-httplib 内嵌 HTTP Server。
     - 优点：符合 spec，依赖轻量，适合 standalone agent。
     - 缺点：需要自行定义 REST 路由、错误响应和 JSON 编解码。
   - 方案 B：引入完整 Web 框架。
     - 优点：路由和中间件能力更丰富。
     - 缺点：超出当前项目轻量目标。
   - 最终选择：方案 A。

3. 持久化方案
   - 方案 A：JSONL 元数据 + payload 文件目录。
     - 优点：依赖少，可读性强，便于测试和排查。
     - 缺点：复杂查询和压缩清理能力有限。
   - 方案 B：SQLite。
     - 优点：查询能力强，事务完整。
     - 缺点：新增系统依赖和迁移复杂度。
   - 最终选择：方案 A。

4. JSON 方案
   - 方案 A：nlohmann/json。
     - 优点：易用，适合 REST 请求/响应，CMake 集成简单。
     - 缺点：需要新增第三方依赖。
   - 方案 B：protobuf JSON util。
     - 优点：贴近 proto 消息。
     - 缺点：REST 资源表达不够自然，错误和进程信息接口会被 proto 结构牵制。
   - 最终选择：方案 A。

### 2.2 外部依赖

- asio-grpc：用于 gRPC 异步客户端和 `Control.Connect` 双向流。
- Asio 或 Boost.Asio：由 asio-grpc 方案确定并随其集成。
- cpp-httplib：用于 standalone agent 内嵌 HTTP REST 服务。
- nlohmann/json：用于 HTTP 请求体、响应体、进程信息和 JSONL 编解码。
- 现有 gRPC、Protobuf、yaml-cpp、fmt、spdlog、eventpp、libpcap、GoogleTest 继续保留。

### 2.3 内部依赖

- `third_party/os.proto`：复用当前 `Control.Connect` 协议和任务消息。
- `standalone/source/agent/tasks/*`：复用 `LogFilterTask`、`PcapTask`、`ExecTask` 的任务执行逻辑。
- `zurg::log_ops`、`zurg::pcap_ops`、`zurg::temp_file`：复用日志过滤、抓包和临时文件能力。
- `zurg::logging::LoggerManager`：复用日志管理。
- `FeatureToggles`、auth event bus 和现有配置加载：复用功能开关和授权状态对任务的约束。
- `test/source/callback_agent_integration_test.cpp`：迁移为新的 gRPC/调度集成测试基础。

## 3. 任务拆解

### 3.1 Runtime / Agent 核心模块

- [x] **TODO-S1: 抽象任务事件模型**
  - **描述**：定义输入无关的任务事件类型，覆盖 ACK、DATA、EOF、ERROR、状态变化、来源输入和时间戳。事件模型需要能表达现有 `AgentToServer` 生命周期，并能被 gRPC、HTTP 和持久化层共同消费。
  - **涉及模块**：任务运行时、任务结果分发。
  - **涉及文件**：`standalone/source/agent/runtime/task_event.h`、`standalone/source/agent/runtime/task_event.cpp`。
  - **依赖**：无。
  - **验收标准**：事件类型能表达 `LogFilter`、`Pcap`、`Exec` 的所有现有输出；测试可构造并断言三类任务事件。

- [x] **TODO-S2: 抽象 TaskSink 和 TaskContext 适配层**
  - **描述**：将当前 `TaskContext` 的直接发送行为改为向一个或多个 TaskSink 发布任务事件；保留任务代码调用语义，避免任务实现关心 gRPC 或 HTTP。
  - **涉及模块**：任务接口、结果分发。
  - **涉及文件**：`standalone/source/agent/tasks/task.h`、`standalone/source/agent/tasks/task.cpp`、`standalone/source/agent/runtime/task_sink.h`、`standalone/source/agent/runtime/task_context_adapter.*`。
  - **依赖**：TODO-S1。
  - **验收标准**：现有三类任务可通过适配层产生任务事件；无 gRPC stream 依赖的任务单元测试可运行。

- [x] **TODO-S3: 抽出共享 TaskScheduler**
  - **描述**：从 `ControlCallbackClient` 中抽出任务去重、功能开关校验、参数校验、串行队列、运行线程、取消、drain 和任务表管理，形成 gRPC 与 HTTP 共享的调度核心。
  - **涉及模块**：任务调度、功能开关、取消。
  - **涉及文件**：`standalone/source/agent/runtime/task_scheduler.h`、`standalone/source/agent/runtime/task_scheduler.cpp`、`standalone/source/agent/control/control_callback_client.*`。
  - **依赖**：TODO-S1、TODO-S2。
  - **验收标准**：单元测试覆盖接受任务、重复 `op_id` 拒绝、功能关闭拒绝、排队任务取消、执行中任务取消和串行顺序。

- [x] **TODO-S4: 建立任务工厂**
  - **描述**：创建统一 TaskFactory，把 `StartOp` 或 HTTP 任务请求转换为 `LogFilterTask`、`PcapTask`、`ExecTask`，并集中处理任务类型识别和校验错误。
  - **涉及模块**：任务创建、输入适配。
  - **涉及文件**：`standalone/source/agent/runtime/task_factory.h`、`standalone/source/agent/runtime/task_factory.cpp`。
  - **依赖**：TODO-S3。
  - **验收标准**：gRPC 和 HTTP 不直接构造具体任务类；测试覆盖三类任务构造、未知任务拒绝和校验失败。

- [x] **TODO-S5: 实现 JSONL TaskStore**
  - **描述**：实现任务元数据、状态、生命周期事件和结果索引的 JSONL 追加写入；大 payload 写入结果文件并记录引用。启动时加载历史任务，未完成任务标记为重启终止。
  - **涉及模块**：持久化、历史任务查询。
  - **涉及文件**：`standalone/source/agent/runtime/task_store.h`、`standalone/source/agent/runtime/jsonl_task_store.cpp`、`standalone/source/agent/runtime/jsonl_task_store.h`。
  - **依赖**：TODO-S1。
  - **验收标准**：测试覆盖写入任务、追加事件、读取历史、读取 payload、重启后未完成任务终止和损坏行跳过并记录错误。

- [x] **TODO-S6: 实现任务状态查询服务**
  - **描述**：在运行时层提供查询当前任务、排队任务、历史任务、单任务状态和单任务结果的只读接口，供 HTTP 使用，也供测试验证调度状态。
  - **涉及模块**：任务查询、HTTP 后端。
  - **涉及文件**：`standalone/source/agent/runtime/task_query_service.h`、`standalone/source/agent/runtime/task_query_service.cpp`。
  - **依赖**：TODO-S3、TODO-S5。
  - **验收标准**：测试可查询运行中、排队中、已完成、已取消、失败和历史任务。

- [ ] **TODO-S7: 迁移授权事件与功能开关处理**
  - **描述**：把当前 `ControlCallbackClient` 中的 auth event 订阅和 `ApplyFeatureUpdate` 行为迁移到共享 TaskScheduler，使 gRPC 与 HTTP 输入一致受限。
  - **涉及模块**：授权事件、功能开关、任务取消。
  - **涉及文件**：`standalone/source/agent/runtime/task_scheduler.*`、`standalone/source/agent/agent_impl.*`。
  - **依赖**：TODO-S3。
  - **验收标准**：授权状态变为非在线时，排队任务被拒绝或取消，执行中任务收到取消；gRPC 与 HTTP 行为一致。

### 3.2 gRPC / HTTP 输入适配模块

- [x] **TODO-C1: CMake 升级到 C++20 并引入依赖**
  - **描述**：将核心库、standalone 和测试目标升级到 C++20；通过现有 CPM 风格引入 asio-grpc、cpp-httplib、nlohmann/json，并确保 gRPC、Protobuf 兼容。
  - **涉及模块**：构建系统、依赖管理。
  - **涉及文件**：`CMakeLists.txt`、`standalone/CMakeLists.txt`、`test/CMakeLists.txt`、`all/CMakeLists.txt`、`docker/Dockerfile.callback-ci`。
  - **依赖**：无。
  - **验收标准**：配置阶段能解析新增依赖；standalone 和测试目标以 C++20 编译；旧的 callback API 编译定义不再是新客户端必要条件。

- [ ] **TODO-C2: 实现 asio-grpc 控制流客户端骨架**
  - **描述**：创建新的 gRPC 控制流客户端，负责建立 `Control.Connect` 双向流、发送 `AgentHello`、读 `ServerToAgent`、顺序写 `AgentToServer` 和停止流。
  - **涉及模块**：gRPC 输入适配。
  - **涉及文件**：`standalone/source/agent/control/control_asio_client.h`、`standalone/source/agent/control/control_asio_client.cpp`。
  - **依赖**：TODO-C1、TODO-S1。
  - **验收标准**：测试桩可观察到连接后发送 `AgentHello`；写队列保持单写顺序；停止时能取消流。

- [ ] **TODO-C3: 迁移 gRPC 消息处理到共享调度器**
  - **描述**：将 `StartOp`、`CancelOp`、`Shutdown`、`Heartbeat` 处理接入 TaskScheduler；任务事件通过 gRPC sink 转回 `AgentToServer`。
  - **涉及模块**：gRPC 输入、任务调度、结果输出。
  - **涉及文件**：`standalone/source/agent/control/control_asio_client.*`、`standalone/source/agent/runtime/grpc_task_sink.*`。
  - **依赖**：TODO-S3、TODO-S4、TODO-C2。
  - **验收标准**：gRPC 集成测试覆盖 `StartOp -> Ack -> Data -> Eof`、取消、shutdown 和重复 `op_id`。

- [ ] **TODO-C4: 实现 gRPC 重连与 backoff**
  - **描述**：在 asio-grpc 客户端中复用现有 backoff、sleep 和 should_run 语义，断线后取消当前活跃任务并按配置重连。
  - **涉及模块**：gRPC 输入、运行控制。
  - **涉及文件**：`standalone/source/agent/control/control_asio_client.*`、`standalone/source/agent/agent_impl.*`。
  - **依赖**：TODO-C2、TODO-C3。
  - **验收标准**：测试覆盖流关闭触发取消、backoff 调用、停止信号退出和重连后再次发送 hello。

- [x] **TODO-C5: 实现 HTTP REST Server 骨架**
  - **描述**：基于 cpp-httplib 创建内嵌 HTTP 服务，支持启动、停止、监听配置、统一 JSON 响应、错误响应和健康检查。
  - **涉及模块**：HTTP 输入服务。
  - **涉及文件**：`standalone/source/agent/http/http_server.h`、`standalone/source/agent/http/http_server.cpp`。
  - **依赖**：TODO-C1。
  - **验收标准**：测试或本地 curl 可访问健康检查；服务停止后端口释放；无鉴权访问不被拒绝。

- [x] **TODO-C6: 实现 HTTP 任务提交接口**
  - **描述**：实现 REST 任务提交，将 JSON 请求解析为统一任务请求，要求调用方提供 `op_id`，支持 `LogFilter`、`Pcap`、`Exec`。
  - **涉及模块**：HTTP 输入、任务工厂、任务调度。
  - **涉及文件**：`standalone/source/agent/http/http_routes_tasks.*`、`standalone/source/agent/http/http_json_proto.*`。
  - **依赖**：TODO-S4、TODO-C5。
  - **验收标准**：curl 可提交三类任务；缺失 `op_id`、重复 `op_id`、未知任务和功能关闭返回明确错误。

- [x] **TODO-C7: 实现 HTTP 状态、结果和取消接口**
  - **描述**：实现任务列表、单任务状态、单任务结果和取消接口，结果读取从 TaskQueryService 和 TaskStore 获取。
  - **涉及模块**：HTTP 查询、取消、持久化读取。
  - **涉及文件**：`standalone/source/agent/http/http_routes_tasks.*`、`standalone/source/agent/runtime/task_query_service.*`。
  - **依赖**：TODO-S5、TODO-S6、TODO-C6。
  - **验收标准**：curl 可轮询任务状态、读取 ACK/DATA/EOF/ERROR 结果、取消排队任务和取消执行中任务。

- [ ] **TODO-C8: 实现 HTTP 进程与配置信息接口**
  - **描述**：提供 Agent 自身进程信息、系统进程列表、CPU/内存信息、当前运行任务、历史任务、配置状态和健康状态查询。
  - **涉及模块**：HTTP 查询、系统信息、配置快照。
  - **涉及文件**：`standalone/source/agent/http/http_routes_process.*`、`standalone/source/agent/system/process_info.*`、`standalone/source/agent/config/agent_config.*`。
  - **依赖**：TODO-C5、TODO-S6、TODO-G2。
  - **验收标准**：浏览器或 curl 可获取所有 spec 要求的信息；Linux 环境下进程和资源字段稳定返回；读取失败时返回局部错误而不是整个接口崩溃。

### 3.3 共享 / 配置 / 文档模块

- [x] **TODO-G1: 重构 Agent 配置模型**
  - **描述**：把当前 `agent_main.cpp` 内部配置结构迁移为独立模块，新增 gRPC、HTTP、持久化配置字段，保留功能开关、日志、log_filter 和 auth 配置。
  - **涉及文件**：`standalone/source/agent/config/agent_config.h`、`standalone/source/agent/config/agent_config.cpp`、`standalone/source/agent_main.cpp`、`standalone/config/zurg_agent.yaml`。
  - **依赖**：无。
  - **验收标准**：配置加载单元测试覆盖默认值、必填字段缺失、HTTP 监听配置、持久化路径和现有字段兼容。

- [ ] **TODO-G2: 调整 agent_main 启动编排**
  - **描述**：让 main 负责加载配置、初始化日志、创建 TaskScheduler、TaskStore、gRPC 客户端和 HTTP Server，并协调启动与停止顺序。
  - **涉及文件**：`standalone/source/agent_main.cpp`、`standalone/source/agent/agent_impl.h`、`standalone/source/agent/agent_impl.cpp`。
  - **依赖**：TODO-G1、TODO-S3、TODO-S5、TODO-C2、TODO-C5。
  - **验收标准**：Agent 启动后 gRPC 与 HTTP 同时启用；SIGINT/SIGTERM 能停止 HTTP、gRPC 和任务线程。

- [ ] **TODO-G3: 清理旧 Callback 客户端边界**
  - **描述**：删除或隔离 `ControlCallbackClient` 和 `ControlStreamClient` 的旧实现，避免新旧 gRPC 输入并存造成行为分叉；迁移仍有价值的测试 hook。
  - **涉及文件**：`standalone/source/agent/control/control_callback_client.*`、`standalone/source/agent/control/control_stream_client.*`、`standalone/CMakeLists.txt`、`test/CMakeLists.txt`。
  - **依赖**：TODO-C3、TODO-C4。
  - **验收标准**：standalone 目标不再编译旧 callback client；测试不依赖旧 callback API 类型。

- [ ] **TODO-G4: 更新 proto 兼容扩展**
  - **描述**：评估并修改 `third_party/os.proto`，只添加保持兼容所需的字段或注释；不得破坏当前 `Control.Connect` 和现有任务消息语义。
  - **涉及文件**：`third_party/os.proto`、`cmake/fix_grpc_namespace.py`、相关生成代码测试。
  - **依赖**：TODO-S1、TODO-C3。
  - **验收标准**：proto 生成成功；旧任务消息仍可被现有测试构造；新增字段不影响已有字段编号。

- [ ] **TODO-G5: 更新文档与示例**
  - **描述**：更新 README、callback 设计文档、任务文档、配置样例和 curl/gRPC 验证说明，明确无 TLS、无鉴权和推荐绑定地址风险。
  - **涉及文件**：`README.md`、`CallbackClient.README.md`、`documentation/callback_tasks.md`、`standalone/config/zurg_agent.yaml`。
  - **依赖**：TODO-C6、TODO-C7、TODO-C8。
  - **验收标准**：文档包含构建依赖、配置字段、HTTP 接口、gRPC 行为、持久化路径和测试命令。

- [ ] **TODO-G6: 更新容器与 CI 构建说明**
  - **描述**：更新 Dockerfile 和 CI 容器依赖，确保 C++20、asio-grpc、cpp-httplib 和 nlohmann/json 构建可用。
  - **涉及文件**：`Dockerfile`、`Dockerfile.simple`、`docker/Dockerfile.callback-ci`、`.github/workflows/*`。
  - **依赖**：TODO-C1。
  - **验收标准**：容器内可完成 standalone 和 test 构建；CI 文档说明新增依赖来源。

## 4. 依赖关系与执行顺序

基础依赖：
- TODO-C1 与 TODO-G1 可并行启动。
- TODO-S1 → TODO-S2 → TODO-S3 → TODO-S4。
- TODO-S1 → TODO-S5 → TODO-S6。

输入适配：
- TODO-C1 → TODO-C2 → TODO-C3 → TODO-C4。
- TODO-C1 → TODO-C5 → TODO-C6 → TODO-C7。
- TODO-C5 + TODO-S6 + TODO-G2 → TODO-C8。

启动与清理：
- TODO-G1 + TODO-S3 + TODO-S5 + TODO-C2 + TODO-C5 → TODO-G2。
- TODO-C3 + TODO-C4 → TODO-G3。
- TODO-S1 + TODO-C3 → TODO-G4。
- TODO-C6 + TODO-C7 + TODO-C8 → TODO-G5。
- TODO-C1 → TODO-G6。

建议执行批次：
1. 批次一：TODO-C1、TODO-G1、TODO-S1、TODO-S5。
2. 批次二：TODO-S2、TODO-S3、TODO-S4、TODO-C5。
3. 批次三：TODO-C2、TODO-C3、TODO-C6、TODO-S6。
4. 批次四：TODO-C4、TODO-C7、TODO-C8、TODO-G2。
5. 批次五：TODO-G3、TODO-G4、TODO-G5、TODO-G6。

## 5. 测试标准

### 5.1 单元测试标准

- TODO-S1：测试三类任务事件、状态事件、错误事件、payload 引用事件的序列化前数据完整性。
- TODO-S2：测试 TaskContext 适配层把 `SendLogData`、`SendPcapData`、`SendExecData`、EOF 和 ERROR 转成 TaskEvent。
- TODO-S3：测试任务接受、重复拒绝、功能开关拒绝、串行顺序、排队取消、执行中取消、drain 行为。
- TODO-S4：测试从 gRPC `StartOp` 和 HTTP JSON 请求创建三类任务；测试未知任务和参数校验错误。
- TODO-S5：测试 JSONL 追加写入、payload 文件写入、历史加载、损坏行处理、未完成任务重启终止。
- TODO-S6：测试当前任务、排队任务、历史任务、单任务状态和结果分页或范围读取。
- TODO-C2：测试 gRPC 客户端 hello、读消息回调、写队列顺序、停止流。
- TODO-C3：测试 gRPC `StartOp / CancelOp / Shutdown / Heartbeat` 到调度器和 gRPC sink 的映射。
- TODO-C5：测试健康检查、JSON 错误响应、服务启动停止。
- TODO-C6：测试 HTTP 三类任务提交、缺失字段、重复 `op_id`、功能关闭。
- TODO-C7：测试 HTTP 状态、结果读取、取消接口。
- TODO-C8：测试进程信息字段存在性、配置状态脱敏策略和局部失败响应。
- TODO-G1：测试配置默认值、必填项、旧字段兼容和非法配置报错。
- TODO-G2：测试启动编排的依赖注入和停止顺序。

### 5.2 集成 / 场景验证标准

1. gRPC 端到端
   - 前置条件：启动 in-process 或回环 gRPC 控制面。
   - 操作步骤：Agent 连接控制面，控制面下发 `LogFilter`、`Pcap`、`Exec`，随后下发取消和 shutdown。
   - 期望结果：Agent 发送 hello、pong、ack、data、eof/error；取消和 shutdown 行为与 spec 一致。
   - 异常场景：控制面断开后 Agent 取消活跃任务并按 backoff 重连。

2. HTTP REST 端到端
   - 前置条件：使用配置文件启动 Agent HTTP 服务和 JSONL 持久化目录。
   - 操作步骤：使用 curl 提交三类任务，轮询状态，读取结果，取消任务，查询健康和进程信息。
   - 期望结果：所有接口返回 JSON；任务异步执行；结果生命周期与 gRPC 语义一致。
   - 异常场景：重复 `op_id`、未知任务、功能关闭、非法 JSON、任务不存在均返回明确错误。

3. gRPC 与 HTTP 混合输入
   - 前置条件：gRPC 控制面和 HTTP 服务同时启用。
   - 操作步骤：快速提交多个 gRPC 和 HTTP 任务。
   - 期望结果：任务进入同一串行队列，同一时刻只执行一个，重复 `op_id` 跨输入被拒绝。
   - 异常场景：HTTP 取消 gRPC 创建的任务或 gRPC 取消 HTTP 创建的任务时，统一取消逻辑生效。

4. 持久化重启
   - 前置条件：Agent 已执行并持久化任务。
   - 操作步骤：停止 Agent，重启后通过 HTTP 查询历史任务。
   - 期望结果：已完成任务可查询结果；重启前未完成任务显示终止状态。
   - 异常场景：JSONL 中存在损坏行时，Agent 跳过损坏记录并记录错误，不影响其它历史任务读取。

5. 构建验证
   - 前置条件：安装 gRPC、Protobuf、libpcap 和构建工具。
   - 操作步骤：运行 standalone、test、all 三套 CMake 构建。
   - 期望结果：C++20 编译通过，测试通过，格式检查可执行。
   - 异常场景：缺失新增依赖时，CMake 给出明确错误。

## 6. 风险与缓解

| 风险 | 影响 | 缓解措施 |
|------|------|---------|
| asio-grpc 与系统 gRPC 版本不兼容 | 构建或运行失败 | 在 TODO-C1 中锁定依赖版本，并在 Docker/CI 中验证 |
| 从 callback API 迁移到 asio-grpc 导致重连或写队列行为回归 | 控制面通信不稳定 | 保留现有 callback 集成测试语义，迁移为新客户端测试 |
| TaskScheduler 抽取影响现有任务执行顺序 | gRPC 与 HTTP 任务行为不一致 | 先写调度单元测试，再接入 gRPC 和 HTTP |
| JSONL 持久化写入失败 | HTTP 历史查询不完整 | 持久化错误进入日志和任务错误，测试磁盘失败路径 |
| HTTP 无鉴权暴露敏感操作 | 部署安全风险 | 文档明确风险，配置默认建议绑定本地地址，功能开关必须生效 |
| Exec 通过 HTTP 暴露后误用 | 命令执行风险 | 沿用现有 Exec 白名单和 `features.exec` 开关 |
| C++20 升级影响现有编译环境 | CI 或用户环境构建失败 | 更新 Dockerfile 和 README，明确编译器版本要求 |
| cpp-httplib 服务线程与任务线程停止顺序不当 | Agent 退出卡住或丢事件 | TODO-G2 中集中管理生命周期，增加停止顺序测试 |

## 7. 开放问题

无。
