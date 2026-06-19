#include "runtime/jsonl_task_store.h"
#include "runtime/feature_toggles.h"
#include "runtime/grpc_task_sink.h"
#include "runtime/task_context_adapter.h"
#include "runtime/task_factory.h"
#include "runtime/task_scheduler.h"

#include <filesystem>
#include <algorithm>

#include <gtest/gtest.h>

namespace {

class CapturingSink : public zurg::agent::runtime::TaskSink {
 public:
  void OnTaskEvent(const zurg::agent::runtime::TaskEvent& event) override {
    events.push_back(event);
  }

  std::vector<zurg::agent::runtime::TaskEvent> events;
};

std::filesystem::path TestStoreDir(const std::string& name) {
  auto dir = std::filesystem::temp_directory_path() / ("zurg-" + name);
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir;
}

}  // namespace

TEST(TaskRuntimeTest, ContextAdapterPublishesLogEvents) {
  CapturingSink sink;
  zurg::agent::runtime::TaskContextAdapter ctx(
      42, zurg::agent::tasks::Task::Kind::kLogFilter,
      zurg::agent::runtime::TaskInputSource::kHttp, {&sink}, [] { return true; });

  ctx.PublishAccepted(true);
  ops::v1::LogChunk chunk;
  chunk.set_offset(7);
  chunk.set_data("hello");
  ctx.SendLogData(42, std::move(chunk));
  ops::v1::LogFilterEof eof;
  eof.set_total_size(5);
  ctx.SendEofLog(42, eof);

  ASSERT_EQ(sink.events.size(), 3U);
  EXPECT_EQ(sink.events[0].event_kind, zurg::agent::runtime::TaskEventKind::kAccepted);
  EXPECT_EQ(sink.events[1].payload_kind, zurg::agent::runtime::TaskPayloadKind::kLogChunk);
  EXPECT_EQ(sink.events[2].event_kind, zurg::agent::runtime::TaskEventKind::kEof);

  ops::v1::LogChunk parsed;
  ASSERT_TRUE(parsed.ParseFromString(sink.events[1].payload_bytes));
  EXPECT_EQ(parsed.offset(), 7);
  EXPECT_EQ(parsed.data(), "hello");
}

TEST(TaskRuntimeTest, JsonlTaskStorePersistsEvents) {
  auto dir = TestStoreDir("jsonl-store");
  zurg::agent::runtime::JsonlTaskStore store(dir);
  std::string error;
  ASSERT_TRUE(store.Open(&error)) << error;

  zurg::agent::runtime::TaskEvent accepted;
  accepted.op_id = 9;
  accepted.task_kind = zurg::agent::tasks::Task::Kind::kExec;
  accepted.source = zurg::agent::runtime::TaskInputSource::kHttp;
  accepted.event_kind = zurg::agent::runtime::TaskEventKind::kAccepted;
  accepted.accepted = true;
  store.OnTaskEvent(accepted);

  ops::v1::ExecChunk chunk;
  chunk.mutable_stdout()->assign("ok");
  zurg::agent::runtime::TaskEvent data;
  data.op_id = 9;
  data.task_kind = zurg::agent::tasks::Task::Kind::kExec;
  data.source = zurg::agent::runtime::TaskInputSource::kHttp;
  data.event_kind = zurg::agent::runtime::TaskEventKind::kData;
  data.payload_kind = zurg::agent::runtime::TaskPayloadKind::kExecChunk;
  chunk.SerializeToString(&data.payload_bytes);
  store.OnTaskEvent(data);

  auto task = store.GetTask(9);
  ASSERT_TRUE(task.has_value());
  EXPECT_EQ(task->state, zurg::agent::tasks::Task::State::kPending);

  zurg::agent::runtime::JsonlTaskStore reopened(dir);
  ASSERT_TRUE(reopened.Open(&error)) << error;
  auto events = reopened.ListEvents(9);
  ASSERT_EQ(events.size(), 3U);
  EXPECT_EQ(events[2].event_kind, zurg::agent::runtime::TaskEventKind::kError);
  EXPECT_EQ(events[2].code, "INTERRUPTED");
}

TEST(TaskRuntimeTest, SchedulerRejectsDuplicateAndDisabledTasks) {
  CapturingSink sink;
  zurg::agent::FeatureToggles features;
  features.enabled = true;
  features.enable_exec = true;
  zurg::agent::runtime::TaskFactory factory({}, nullptr);
  zurg::agent::runtime::TaskScheduler::Options options;
  options.features = features;
  options.should_run = [] { return true; };
  zurg::agent::runtime::TaskScheduler scheduler(std::move(factory), options);
  scheduler.AddSink(&sink);

  zurg::agent::runtime::TaskRequest request;
  request.op_id = 88;
  request.source = zurg::agent::runtime::TaskInputSource::kHttp;
  ops::v1::ExecSpec exec;
  exec.set_cmd("ip");
  request.spec = exec;

  auto first = scheduler.Submit(request);
  EXPECT_TRUE(first.accepted);
  auto duplicate = scheduler.Submit(request);
  EXPECT_FALSE(duplicate.accepted);
  EXPECT_EQ(duplicate.reason, "duplicate op_id");

  scheduler.Cancel(88);
  scheduler.Stop();

  auto rejected = std::find_if(sink.events.begin(), sink.events.end(), [](const auto& event) {
    return event.event_kind == zurg::agent::runtime::TaskEventKind::kRejected &&
           event.message == "duplicate op_id";
  });
  EXPECT_NE(rejected, sink.events.end());
}

TEST(TaskRuntimeTest, SchedulerHonorsFeatureToggles) {
  CapturingSink sink;
  zurg::agent::FeatureToggles features;
  features.enabled = true;
  features.enable_exec = false;
  zurg::agent::runtime::TaskFactory factory({}, nullptr);
  zurg::agent::runtime::TaskScheduler::Options options;
  options.features = features;
  zurg::agent::runtime::TaskScheduler scheduler(std::move(factory), options);
  scheduler.AddSink(&sink);

  zurg::agent::runtime::TaskRequest request;
  request.op_id = 7;
  request.source = zurg::agent::runtime::TaskInputSource::kHttp;
  ops::v1::ExecSpec exec;
  exec.set_cmd("ip");
  request.spec = exec;

  auto result = scheduler.Submit(request);
  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.reason, "exec disabled");
}

TEST(TaskRuntimeTest, GrpcTaskSinkConvertsEventsToAgentMessages) {
  std::vector<ops::v1::AgentToServer> sent;
  zurg::agent::runtime::GrpcTaskSink sink(
      [&](ops::v1::AgentToServer msg) { sent.push_back(std::move(msg)); });

  zurg::agent::runtime::TaskEvent ack;
  ack.op_id = 3;
  ack.event_kind = zurg::agent::runtime::TaskEventKind::kAccepted;
  ack.accepted = true;
  sink.OnTaskEvent(ack);

  ops::v1::ExecChunk chunk;
  chunk.mutable_stdout()->assign("stdout");
  zurg::agent::runtime::TaskEvent data;
  data.op_id = 3;
  data.event_kind = zurg::agent::runtime::TaskEventKind::kData;
  data.payload_kind = zurg::agent::runtime::TaskPayloadKind::kExecChunk;
  chunk.SerializeToString(&data.payload_bytes);
  sink.OnTaskEvent(data);

  ASSERT_EQ(sent.size(), 2U);
  ASSERT_TRUE(sent[0].has_ack());
  EXPECT_TRUE(sent[0].ack().accepted());
  ASSERT_TRUE(sent[1].has_data());
  ASSERT_TRUE(sent[1].data().has_exec_chunk());
  EXPECT_EQ(sent[1].data().exec_chunk().stdout(), "stdout");
}
