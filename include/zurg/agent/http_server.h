#ifndef ZURG_AGENT_HTTP_SERVER_H_
#define ZURG_AGENT_HTTP_SERVER_H_

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <agrpc/asio_grpc.hpp>
#include <asio/io_context.hpp>
#include <grpcpp/grpcpp.h>
#include <httplib.h>

namespace zurg::agent {

// A dual-protocol server class that demonstrates simultaneous support for
// HTTP requests (via cpp-httplib) and gRPC integration (via asio-grpc).
class HttpGrpcServer {
 public:
  HttpGrpcServer();
  ~HttpGrpcServer();

  // Starts the HTTP server on the given port and initializes the gRPC context.
  // This will spawn internal threads for the HTTP server and asio io_context.
  bool Start(int http_port);

  // Stops the HTTP server and the io_context, waiting for threads to finish.
  void Stop();

  // Manipulates some internal state.
  void SetState(const std::string& new_state);
  std::string GetState() const;

 private:
  void RunHttpServer(int port);
  void RunIoContext();

  std::unique_ptr<httplib::Server> http_server_;
  std::unique_ptr<std::thread> http_thread_;

  std::unique_ptr<asio::io_context> io_context_;
  std::unique_ptr<agrpc::GrpcContext> grpc_context_;
  std::unique_ptr<grpc::Server> grpc_server_;
  std::unique_ptr<std::thread> asio_thread_;

  std::atomic<bool> is_running_;

  mutable std::mutex state_mutex_;
  std::string shared_state_;
};

}  // namespace zurg::agent

#endif  // ZURG_AGENT_HTTP_SERVER_H_
