#include "zurg/agent/http_server.h"

#include <spdlog/spdlog.h>

namespace zurg::agent {

HttpGrpcServer::HttpGrpcServer()
    : is_running_(false),
      shared_state_("initial_state") {}

HttpGrpcServer::~HttpGrpcServer() {
  Stop();
}

bool HttpGrpcServer::Start(int http_port) {
  if (is_running_.exchange(true)) {
    spdlog::warn("HttpGrpcServer is already running");
    return false;
  }

  spdlog::info("Starting HttpGrpcServer...");

  // Initialize io_context and gRPC context
  io_context_ = std::make_unique<asio::io_context>();
  std::unique_ptr<grpc::ServerBuilder> builder = std::make_unique<grpc::ServerBuilder>();

  // NOTE: In a real application, you would register a gRPC service here.
  // builder->RegisterService(&my_service_);

  grpc_context_ = std::make_unique<agrpc::GrpcContext>(builder->AddCompletionQueue());

  // Start the gRPC server
  grpc_server_ = builder->BuildAndStart();

  // Start HTTP server
  http_server_ = std::make_unique<httplib::Server>();

  // Define HTTP endpoints
  http_server_->Get("/state", [this](const httplib::Request& req, httplib::Response& res) {
    res.set_content(GetState(), "text/plain");
  });

  http_server_->Post("/state", [this](const httplib::Request& req, httplib::Response& res) {
    SetState(req.body);
    res.set_content("State updated", "text/plain");
  });

  // Start threads
  http_thread_ = std::make_unique<std::thread>(&HttpGrpcServer::RunHttpServer, this, http_port);
  asio_thread_ = std::make_unique<std::thread>(&HttpGrpcServer::RunIoContext, this);

  return true;
}

void HttpGrpcServer::Stop() {
  if (!is_running_.exchange(false)) {
    return;
  }

  spdlog::info("Stopping HttpGrpcServer...");

  if (http_server_) {
    http_server_->stop();
  }

  if (grpc_server_) {
    grpc_server_->Shutdown();
  }

  if (io_context_) {
    io_context_->stop();
  }

  if (http_thread_ && http_thread_->joinable()) {
    http_thread_->join();
  }

  if (asio_thread_ && asio_thread_->joinable()) {
    asio_thread_->join();
  }

  http_server_.reset();
  grpc_server_.reset();
  grpc_context_.reset();
  io_context_.reset();

  spdlog::info("HttpGrpcServer stopped");
}

void HttpGrpcServer::SetState(const std::string& new_state) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  shared_state_ = new_state;
  spdlog::info("State updated to: {}", shared_state_);
}

std::string HttpGrpcServer::GetState() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return shared_state_;
}

void HttpGrpcServer::RunHttpServer(int port) {
  spdlog::info("HTTP server listening on port {}", port);
  if (!http_server_->listen("0.0.0.0", port)) {
    spdlog::error("HTTP server failed to listen on port {}", port);
  }
}

void HttpGrpcServer::RunIoContext() {
  spdlog::info("Running asio io_context");

  // Create a work guard to keep io_context running even if there's no work
  auto work_guard = asio::make_work_guard(*io_context_);

  io_context_->run();
  spdlog::info("asio io_context finished");
}

}  // namespace zurg::agent
