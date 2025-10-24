# Zurg gNOI Agent Toolkit

Zurg 提供了一套用于实现 gNOI Agent 的 C++ 组件与示例程序，涵盖日志采集、PCAP 抓包、回调客户端以及统一的日志管理接口。项目基于 C++17、gRPC Callback API、libpcap 与 spdlog 构建，可作为轻量级的网络运维代理或相关教学示例。

## 目录结构

- `include/zurg/` — 核心库头文件，暴露日志、抓包等对外接口。
- `source/zurg/` — 库实现，依赖 `os.proto` 生成的消息类型。
- `standalone/` — 独立的 gNOI Agent 可执行程序（`zurg_agent`），复用核心库并实现队列调度、任务执行与重连策略。
- `test/` — 基于 GoogleTest 的功能与集成测试，涵盖日志过滤、PCAP、LoggerManager 及 Callback Agent 行为。
- `documentation/` — Doxygen + m.css 文档工程。
- `third_party/os.proto` — gNOI 控制协议定义，构建时自动生成对应的 C++ 代码。

## 核心模块

| 模块 | 功能概要 |
| ---- | -------- |
| `zurg::log_ops` | 按 BPF/时间范围/关键字过滤日志文件，支持回放与 chunk 上报。 |
| `zurg::pcap_ops` | 基于 libpcap 或合成数据源抓包，返回统计信息与 packet 序列。 |
| `zurg::logging::LoggerManager` | 统一管理 spdlog logger，支持动态增删 sink、按名称调级以及测试覆写。 |
| `standalone::agent` | gRPC Callback 客户端，串行调度 `LogFilter`、`Pcap`、`Exec` 任务，支持重试与 cancel。 |

更多设计细节可查看 `LogManager.README.md` 与 `CallbackClient.README.md`。

## 构建指南

Zurg 采用 CMake 进行构建，推荐使用 v3.21+（最低 3.14）。默认依赖可通过系统包管理安装：

- 编译工具链：`g++` 或 `clang++`（支持 C++17）
- gRPC & Protobuf：`protobuf-compiler`, `libprotobuf-dev`, `libgrpc++-dev`
- libpcap：`libpcap-dev`
- spdlog（header-only 即可）

### 编译核心库

```bash
cmake -S . -B build
cmake --build build
```

上述流程会生成 `libzurg.a`（或对应的共享库）以及 `generated/os.pb.{h,cc}`。

### 构建 standalone Agent

```bash
cmake -S standalone -B build/standalone
cmake --build build/standalone

./build/standalone/zurg_agent --help
```

`zurg_agent` 支持以下常用参数：

```
Usage:
  zurg_agent [OPTIONS]

Options:
  -t, --target   gRPC 控制面地址（默认 127.0.0.1:50051）
  -a, --agent_id 自定义 Agent ID；未提供时自动生成 agent-<timestamp>
  -h, --help     打印帮助信息
```

代理会在启动时发送 `AgentHello` 并进入控制循环，串行执行 `LogFilter / Pcap / Exec` 任务，相关日志通过 `LoggerManager` 输出。

### 运行测试

```bash
cmake -S test -B build/test
cmake --build build/test
CTEST_OUTPUT_ON_FAILURE=1 cmake --build build/test --target test
```

测试工程会自动生成 `os.proto` 对应的 gRPC stub，并链接核心库与 agent 源码以覆盖端到端场景。若需要抓包或日志相关测试，请确保 `libpcap` 与必要的内核能力已安装。

### 构建全集工程

为方便 IDE 或一次性构建，可使用 `all` 目录：

```bash
cmake -S all -B build/all
cmake --build build/all
```

### 生成文档

```bash
cmake -S documentation -B build/docs
cmake --build build/docs --target GenerateDocs
```

输出位于 `build/docs/doxygen/`，包含 API 文档与架构概述。

### 容器支持

- `Dockerfile`：最小化构建环境，生成 `zurg_agent` 并默认执行 `--help`。
- `Dockerfile.simple`：仅打包已编译二进制，适合将 CI 产物封装为运行镜像。
- `docker/Dockerfile.callback-ci`：提供完整的 gRPC Callback 构建环境，可直接用于 CI。

## 开发与调试提示

- `cmake/fix_grpc_namespace.py` 会在生成 gRPC 代码后修正命名空间以契合项目布局；若调整 `.proto`，请保持该脚本同步。
- `zurg::agent::internal` 暴露大量 `Set*ForTests` 钩子，可在单元测试或实验场景下自定义回退策略、发送行为、日志输出。
- `LoggerManager` 默认以懒加载方式创建 logger，测试中可通过 `SetLoggerSinkForTests` 注入自定义 sink 捕获输出。

## 许可证

本项目继承模板的 [MIT License](LICENSE)。若在生产环境中使用，请确保遵循依赖库（gRPC、libpcap、spdlog 等）的许可要求。

## 参与贡献

欢迎通过 Issue 或 Pull Request 反馈问题与改进建议。提交前可运行：

```bash
cmake --build build/test --target test
cmake --build build/test --target check-format
```

以确保基础测试与格式检查通过。

