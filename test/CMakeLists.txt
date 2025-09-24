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
pkg_check_modules(PCAP REQUIRED libpcap)

set(PROTO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../third_party)
set(GENERATED_PROTO_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(OS_PROTO ${PROTO_ROOT}/os.proto)
set(OS_PROTO_SRC ${GENERATED_PROTO_DIR}/os.pb.cc)
set(OS_PROTO_HDR ${GENERATED_PROTO_DIR}/os.pb.h)

file(MAKE_DIRECTORY ${GENERATED_PROTO_DIR})

add_custom_command(
  OUTPUT ${OS_PROTO_SRC} ${OS_PROTO_HDR}
  COMMAND ${Protobuf_PROTOC_EXECUTABLE}
  ARGS --cpp_out=${GENERATED_PROTO_DIR} -I ${PROTO_ROOT} ${OS_PROTO}
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
  ${CMAKE_CURRENT_LIST_DIR}/../source/zurg/file_ops.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../source/zurg/pcap_ops.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../standalone/source/agent/agent_impl.cpp
  ${OS_PROTO_SRC}
)
target_link_libraries(${PROJECT_NAME} PRIVATE GTest::gtest GTest::gtest_main protobuf::libprotobuf fmt::fmt OpenSSL::Crypto)
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
target_link_libraries(${PROJECT_NAME} PRIVATE ${GRPCPP_LIBRARIES} ${PCAP_LIBRARIES})
target_compile_options(${PROJECT_NAME} PRIVATE ${GRPCPP_CFLAGS_OTHER} ${PCAP_CFLAGS_OTHER})

# ---- code coverage ----

# 覆盖率选项忽略（如需可单独配置当前目标）

# ---- gNOI integration removed; tests rely on os.proto only ----
