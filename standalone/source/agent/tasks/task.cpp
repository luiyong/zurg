#include "tasks/task.h"

#include <utility>

namespace zurg::agent::tasks {

Task::Task(std::uint32_t op_id, Kind kind, std::shared_ptr<spdlog::logger> logger)
    : logger_(std::move(logger)), op_id_(op_id), kind_(kind) {}

Task::~Task() = default;

}  // namespace zurg::agent::tasks
