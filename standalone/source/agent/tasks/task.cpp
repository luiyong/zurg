#include "tasks/task.h"

#include <utility>

namespace zurg::agent::tasks {

Task::Task(std::string op_id, Kind kind, std::shared_ptr<spdlog::logger> logger)
    : logger_(std::move(logger)), op_id_(std::move(op_id)), kind_(kind) {}

Task::~Task() = default;

}  // namespace zurg::agent::tasks

