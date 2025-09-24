cmake_minimum_required(VERSION 3.14...3.22)

project(GreeterTests LANGUAGES CXX)

# ---- Options ----

option(ENABLE_TEST_COVERAGE "Enable test coverage" OFF)
option(TEST_INSTALLED_VERSION "Test the version found by find_package" OFF)

# --- 基础设置（移除外部工具依赖） ----

# ---- Dependencies ----

## 不再使用 CPM，避免网络依赖

# 使用系统安装的 GoogleTest（建议通过包管理器安装）
find_package(GTest REQUIRED)
find_package(Protobuf REQUIRED)
find_package(fmt REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GRPCPP REQUIRED grpc++)
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

set(PROTO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../third_party)
set(GENERATED_PROTO_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(OS_PROTO ${PROTO_ROOT}/os.proto)
set(OS_PROTO_SRC ${GENERATED_PROTO_DIR}/os.pb.cc)
set(OS_PROTO_HDR ${GENERATED_PROTO_DIR}/os.pb.h)
set(OS_GRPC_PROTO_SRC ${GENERATED_PROTO_DIR}/os.grpc.pb.cc)
set(OS_GRPC_PROTO_HDR ${GENERATED_PROTO_DIR}/os.grpc.pb.h)

find_program(GRPC_CPP_PLUGIN_EXECUTABLE NAMES grpc_cpp_plugin
  HINTS /usr/bin /usr/local/bin /opt/homebrew/bin)
if(NOT GRPC_CPP_PLUGIN_EXECUTABLE)
  message(FATAL_ERROR "grpc_cpp_plugin not found; install protobuf-compiler-grpc 或 brew install grpc")
endif()

file(MAKE_DIRECTORY ${GENERATED_PROTO_DIR})

add_custom_command(
  OUTPUT ${OS_PROTO_SRC} ${OS_PROTO_HDR} ${OS_GRPC_PROTO_SRC} ${OS_GRPC_PROTO_HDR}
  COMMAND ${Protobuf_PROTOC_EXECUTABLE}
  ARGS --cpp_out=${GENERATED_PROTO_DIR}
       --grpc_out=${GENERATED_PROTO_DIR}
       --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXECUTABLE}
       -I ${PROTO_ROOT} ${OS_PROTO}
  DEPENDS ${OS_PROTO}
  COMMENT "Generating test C++ sources from os.proto"
  VERBATIM
)

# ---- Create binary ----

set(sources
  ${CMAKE_CURRENT_SOURCE_DIR}/source/main.cpp
)
add_executable(${PROJECT_NAME} ${sources})
target_sources(${PROJECT_NAME} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/source/smoke.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/file_ops_unit.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/pcap_ops_test.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/agent_test.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/logger_manager_test.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../source/zurg/file_ops.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../source/zurg/pcap_ops.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../source/zurg/logger_manager.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../standalone/source/agent/agent_impl.cpp
  ${OS_PROTO_SRC}
  ${OS_GRPC_PROTO_SRC}
)
target_link_libraries(${PROJECT_NAME} PRIVATE GTest::gtest GTest::gtest_main protobuf::libprotobuf fmt::fmt OpenSSL::Crypto)
if(spdlog_FOUND)
  target_link_libraries(${PROJECT_NAME} PRIVATE spdlog::spdlog_header_only)
endif()
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 17)

# 这里不设置上层 Greeter 目标的编译选项，避免依赖上层工程

# ---- 代码覆盖率配置 ----
if (ENABLE_TEST_COVERAGE)
  set(COVERAGE_FLAGS --coverage -O0 -g)
  target_compile_options(${PROJECT_NAME} PRIVATE ${COVERAGE_FLAGS})
  target_link_options(${PROJECT_NAME} PRIVATE --coverage)
endif()

# ---- Add GreeterTests ----

enable_testing()
include(GoogleTest)
gtest_discover_tests(${PROJECT_NAME})

target_include_directories(${PROJECT_NAME} PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../include
  ${CMAKE_CURRENT_LIST_DIR}/../standalone/source
  ${CMAKE_CURRENT_LIST_DIR}/../standalone/source/agent
  ${GENERATED_PROTO_DIR}
  ${GRPCPP_INCLUDE_DIRS}
  ${PCAP_INCLUDE_DIRS}
)
if(NOT spdlog_FOUND)
  target_include_directories(${PROJECT_NAME} PRIVATE ${SPDLOG_INCLUDE_DIR})
endif()
target_link_libraries(${PROJECT_NAME} PRIVATE ${GRPCPP_LIBRARIES} ${PCAP_LIBRARIES})
target_compile_options(${PROJECT_NAME} PRIVATE ${GRPCPP_CFLAGS_OTHER} ${PCAP_CFLAGS_OTHER})

# ---- code coverage ----

# 覆盖率选项忽略（如需可单独配置当前目标）

# ---- gNOI integration removed; tests rely on os.proto only ----
