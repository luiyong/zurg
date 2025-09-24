# LoggerManager

## 背景

项目包含多个独立模块，各模块需要单独的日志器（logger）以便在调试时精确控制日志级别。所有 logger 应共享一组 sink（如控制台、文件），保证日志格式一致并统一输出。

本项目基于 [spdlog](https://github.com/gabime/spdlog) 实现一个 `LoggerManager`，用于统一创建、管理 logger。

---

## 功能特性

* **多模块独立 logger**：每个模块有独立的 logger，可单独设置日志级别。
* **共享 sink**：所有 logger 使用相同的 sink 列表（控制台、文件等）。
* **同步线程安全**：保证在多线程环境中安全操作。
* **弱生命周期管理**：`LoggerManager` 不持有 logger 生命周期，仅保存 `weak_ptr`。
* **自动清理**：使用自定义 deleter，在 logger 被销毁时自动从管理表移除。
* **单个调级**：支持按名称调整 logger 级别。
* **默认级别**：支持设置全局默认级别，新建 logger 自动应用。
* **清单查询**：可以列出当前所有活跃 logger，返回其名称与级别。

---

## 非目标（未来可扩展）

* 异步日志（async logger）。
* 批量调级（按前缀/全局调整）。
* 配置文件或环境变量驱动的热更新。
* sink 的动态增删。

---

## 使用方式

### 初始化

```cpp
LoggerManager::init({
    .default_level = spdlog::level::info,
    .pattern = "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] %v"
});

auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
LoggerManager::add_sink(console_sink);

auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("logs/app.log", true);
LoggerManager::add_sink(file_sink);
```

### 获取 logger

```cpp
auto log = LoggerManager::logger("net.http");
log->info("hello from http");
```

### 调整级别

```cpp
LoggerManager::set_level("net.http", spdlog::level::debug);
```

### 查看当前 logger

```cpp
for (auto& info : LoggerManager::list_loggers()) {
    std::cout << info.name << " level=" << info.level << " active=" << info.active << "\n";
}
```

### 程序退出

```cpp
LoggerManager::shutdown();
```

---

## 验证清单

### Logger 创建

* [ ] `LoggerManager::logger("foo")` 可返回 logger。
* [ ] 同名 logger 返回同一实例。
* [ ] logger 销毁后从管理表自动移除。

### Sink 共享

* [ ] 多个 logger 输出到同一 sink。
* [ ] 输出格式一致。

### 日志级别

* [ ] 可以单独调整某个 logger 的级别。
* [ ] 新建 logger 使用默认级别。
* [ ] 不存在的 logger 调级时，可选择立即创建或记录覆盖意图。

### 清单

* [ ] `list_loggers()` 能返回当前活跃 logger 名称与级别。
* [ ] 销毁后 logger 不再出现在清单中。

### 生命周期

* [ ] `shutdown()` 后不再操作管理表。
* [ ] 程序正常退出不崩溃。

---
