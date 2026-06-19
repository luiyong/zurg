cmake_minimum_required(VERSION 3.14...3.22)

project(ZurgTests LANGUAGES CXX)

# ---- Options ----

option(ENABLE_TEST_COVERAGE "Enable test coverage" OFF)
option(TEST_INSTALLED_VERSION "Test the version found by find_package" OFF)
option(ENABLE_AUTH_TESTS "Build auth client/unit tests" OFF)

# --- 基础设置（移除外部工具依赖） ----

# ---- Dependencies ----

## 不再使用 CPM，避免网络依赖

# 使用系统安装的 GoogleTest（建议通过包管理器安装）
find_package(GTest REQUIRED)
find_package(Protobuf CONFIG REQUIRED)

if (NOT Protobuf_PROTOC_EXECUTABLE AND TARGET protobuf::protoc)
  get_target_property(_PROTOC_LOCATION protobuf::protoc IMPORTED_LOCATION)
  if (NOT _PROTOC_LOCATION)
    get_target_property(_PROTOC_LOCATION protobuf::protoc IMPORTED_LOCATION_RELEASE)
  endif()
  if (NOT _PROTOC_LOCATION)
    get_target_property(_PROTOC_LOCATION protobuf::protoc IMPORTED_LOCATION_DEBUG)
  endif()
  set(Protobuf_PROTOC_EXECUTABLE ${_PROTOC_LOCATION})
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(gRPC CONFIG QUIET)
find_package(PkgConfig REQUIRED)
set(GRPCPP_LINK_LIBS "")
set(GRPCPP_INCLUDE_DIRS_RES "")
set(GRPCPP_COMPILE_OPTS "")
set(GRPCPP_LIBRARY_DIRS_RES "")
if(gRPC_FOUND)
  list(APPEND GRPCPP_LINK_LIBS gRPC::grpc++)
else()
  pkg_check_modules(GRPCPP QUIET grpc++)
  if(NOT GRPCPP_FOUND)
    message(FATAL_ERROR "gRPC++ (with re2) not found; install libgrpc++-dev or provide pkg-config metadata")
  endif()
  list(APPEND GRPCPP_LINK_LIBS ${GRPCPP_LIBRARIES})
  list(APPEND GRPCPP_INCLUDE_DIRS_RES ${GRPCPP_INCLUDE_DIRS})
  list(APPEND GRPCPP_COMPILE_OPTS ${GRPCPP_CFLAGS_OTHER})
  list(APPEND GRPCPP_LIBRARY_DIRS_RES ${GRPCPP_LIBRARY_DIRS})
endif()
pkg_check_modules(PCAP libpcap)

if(PCAP_FOUND)
  set(PCAP_LIBRARIES ${PCAP_LIBRARIES})
  set(PCAP_INCLUDE_DIRS ${PCAP_INCLUDE_DIRS})
  set(PCAP_CFLAGS_OTHER ${PCAP_CFLAGS_OTHER})
else()
  find_path(PCAP_INCLUDE_DIR pcap.h
    HINTS
      /usr/include
      /usr/local/include
      /opt/homebrew/include
  )
  find_library(PCAP_LIBRARY pcap
    HINTS
      /usr/lib
      /usr/local/lib
      /opt/homebrew/lib
  )
  if(NOT PCAP_INCLUDE_DIR OR NOT PCAP_LIBRARY)
    message(FATAL_ERROR "libpcap headers or libraries not found; install libpcap-dev / brew install libpcap")
  endif()
  set(PCAP_LIBRARIES ${PCAP_LIBRARY})
  set(PCAP_INCLUDE_DIRS ${PCAP_INCLUDE_DIR})
  set(PCAP_CFLAGS_OTHER "")
endif()

find_package(spdlog CONFIG QUIET)
if(NOT spdlog_FOUND)
  find_path(SPDLOG_INCLUDE_DIR spdlog/spdlog.h
    HINTS
      ${CMAKE_CURRENT_LIST_DIR}/../build/_deps/spdlog-src/include
      ${CMAKE_CURRENT_LIST_DIR}/../build-debug/_deps/spdlog-src/include
      ${CMAKE_CURRENT_LIST_DIR}/../build-standalone/_deps/spdlog-src/include
      ${CMAKE_CURRENT_LIST_DIR}/../third_party/spdlog/include
      /usr/local/include
      /usr/include
  )
  if(NOT SPDLOG_INCLUDE_DIR)
    message(FATAL_ERROR "spdlog headers not found; install libspdlog-dev 或先配置顶层工程")
  endif()
endif()

find_package(nlohmann_json CONFIG QUIET)
if(NOT TARGET nlohmann_json::nlohmann_json)
  find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp
    HINTS
      ${CMAKE_CURRENT_LIST_DIR}/../build/_deps/nlohmann_json-src/include
      ${CMAKE_CURRENT_LIST_DIR}/../build-standalone/_deps/nlohmann_json-src/include
      ${CMAKE_CURRENT_LIST_DIR}/../third_party/json/include
      /usr/local/include
      /usr/include
      /opt/homebrew/include
  )
  if(NOT NLOHMANN_JSON_INCLUDE_DIR)
    message(FATAL_ERROR "nlohmann/json.hpp not found; configure the top-level or standalone project first, or install nlohmann-json")
  endif()
endif()

set(GENERATED_PROTO_DIR ${PROJECT_BINARY_DIR}/generated)

# ---- Create binary ----

set(sources
  ${CMAKE_CURRENT_SOURCE_DIR}/source/main.cpp
)
add_executable(${PROJECT_NAME} ${sources})
target_sources(${PROJECT_NAME} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/source/smoke.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/pcap_ops_test.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/logger_manager_test.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/log_ops_test.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/temp_file_test.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/task_runtime_test.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/callback_agent_integration_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../standalone/source/agent/runtime/jsonl_task_store.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../standalone/source/agent/runtime/grpc_task_sink.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../standalone/source/agent/runtime/task_context_adapter.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../standalone/source/agent/runtime/task_event.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../standalone/source/agent/runtime/task_factory.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../standalone/source/agent/runtime/task_query_service.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../standalone/source/agent/runtime/task_scheduler.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../standalone/source/agent/tasks/task.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../standalone/source/agent/tasks/log_filter_task.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../standalone/source/agent/tasks/pcap_task.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../standalone/source/agent/tasks/exec_task.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../standalone/source/agent/agent_impl.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../standalone/source/agent/control/control_stream_client.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../standalone/source/agent/control/control_callback_client.cpp
)
if(ENABLE_AUTH_TESTS)
  target_sources(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/source/auth/auth_client_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/auth/auth_manager_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/auth/persistence_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/auth/connection_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/auth/reconnect_test.cpp
  )
endif()
target_link_libraries(${PROJECT_NAME} PRIVATE
  GTest::gtest
  GTest::gtest_main
  protobuf::libprotobuf
  zurg::zurg
  spdlog::spdlog
)
add_dependencies(${PROJECT_NAME} zurg)
if(GRPCPP_LIBRARY_DIRS_RES)
  target_link_directories(${PROJECT_NAME} PRIVATE ${GRPCPP_LIBRARY_DIRS_RES})
endif()
target_link_libraries(${PROJECT_NAME} PRIVATE ${GRPCPP_LINK_LIBS})
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)

# 这里不设置上层 zurg 目标的编译选项，避免依赖上层工程

# ---- 代码覆盖率配置 ----
if (ENABLE_TEST_COVERAGE)
  set(COVERAGE_FLAGS --coverage -O0 -g)
  target_compile_options(${PROJECT_NAME} PRIVATE ${COVERAGE_FLAGS})
  target_link_options(${PROJECT_NAME} PRIVATE --coverage)
endif()

# ---- Add ZurgTests ----

enable_testing()
include(GoogleTest)
gtest_discover_tests(${PROJECT_NAME})

target_include_directories(${PROJECT_NAME} PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../include
  ${CMAKE_CURRENT_LIST_DIR}/../standalone/source
  ${CMAKE_CURRENT_LIST_DIR}/../standalone/source/agent
  ${GENERATED_PROTO_DIR}
  ${PROJECT_BINARY_DIR}/generated
  ${GRPCPP_INCLUDE_DIRS_RES}
  ${PCAP_INCLUDE_DIRS}
)
if(NOT spdlog_FOUND)
  target_include_directories(${PROJECT_NAME} PRIVATE ${SPDLOG_INCLUDE_DIR})
endif()
target_link_libraries(${PROJECT_NAME} PRIVATE ${PCAP_LIBRARIES})
if(TARGET nlohmann_json::nlohmann_json)
  target_link_libraries(${PROJECT_NAME} PRIVATE nlohmann_json::nlohmann_json)
else()
  target_include_directories(${PROJECT_NAME} PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
endif()
target_compile_options(${PROJECT_NAME} PRIVATE ${GRPCPP_COMPILE_OPTS} ${PCAP_CFLAGS_OTHER})

# ---- code coverage ----

# 覆盖率选项忽略（如需可单独配置当前目标）

# ---- gNOI integration removed; tests rely on os.proto only ----
