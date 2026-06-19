#pragma once

#include <memory>
#include <string>
#include <thread>

#include <httplib.h>

#include "runtime/task_query_service.h"
#include "runtime/task_scheduler.h"

namespace zurg::agent::http {

struct HttpServerOptions {
  std::string listen_address = "127.0.0.1";
  int port = 8080;
};

class HttpServer {
 public:
  HttpServer(HttpServerOptions options,
             std::shared_ptr<runtime::TaskQueryService> query_service,
             std::shared_ptr<runtime::TaskScheduler> scheduler = nullptr);
  ~HttpServer();

  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  void Start();
  void Stop();
  bool is_running() const { return running_; }

 private:
  void RegisterRoutes();

  HttpServerOptions options_;
  std::shared_ptr<runtime::TaskQueryService> query_service_;
  std::shared_ptr<runtime::TaskScheduler> scheduler_;
  httplib::Server server_;
  std::thread thread_;
  bool running_ = false;
};

}  // namespace zurg::agent::http
