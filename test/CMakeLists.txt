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

# ---- Create binary ----

set(sources
  ${CMAKE_CURRENT_SOURCE_DIR}/source/main.cpp
)
add_executable(${PROJECT_NAME} ${sources})
target_sources(${PROJECT_NAME} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/source/smoke.cpp
)
target_link_libraries(${PROJECT_NAME} PRIVATE GTest::gtest GTest::gtest_main)
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 17)

# 这里不设置上层 Greeter 目标的编译选项，避免依赖上层工程

# ---- Add GreeterTests ----

enable_testing()
include(GoogleTest)
gtest_discover_tests(${PROJECT_NAME})

# ---- code coverage ----

# 覆盖率选项忽略（如需可单独配置当前目标）

# ---- gNOI File tests (require Protobuf + gRPC) ----

find_package(PkgConfig REQUIRED)
pkg_check_modules(GRPCPP QUIET grpc++)
pkg_check_modules(PCAP QUIET libpcap)

find_program(Protobuf_PROTOC_EXECUTABLE NAMES protoc)
find_program(GRPC_CPP_PLUGIN_EXECUTABLE NAMES grpc_cpp_plugin)

if (GRPCPP_FOUND AND PCAP_FOUND AND Protobuf_PROTOC_EXECUTABLE)
  message(STATUS "Enabling gNOI File tests (Protobuf/gRPC found)")
  find_package(Protobuf REQUIRED)
  find_package(OpenSSL REQUIRED)
  set(PROTO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../third_party)
  get_filename_component(PROTO_ROOT ${PROTO_ROOT} REALPATH)
  set(GNOI_PROTO_DIR ${PROTO_ROOT}/github.com/openconfig/gnoi)
  set(PROTO_FILES
    ${GNOI_PROTO_DIR}/types/types.proto
    ${GNOI_PROTO_DIR}/common/common.proto
    ${GNOI_PROTO_DIR}/file/file.proto
    ${GNOI_PROTO_DIR}/packet_capture/packet_capture.proto
  )
  set(GEN_SRC_DIR generated)
  file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${GEN_SRC_DIR})
  set(PROTO_INCLUDES -I ${PROTO_ROOT})

  set(GNOI_PB_SRCS)
  set(GNOI_PB_HDRS)
  set(GNOI_GRPC_SRCS)
  set(GNOI_GRPC_HDRS)
  if (TARGET grpc_cpp_plugin)
    set(GRPC_CPP_PLUGIN_PATH $<TARGET_FILE:grpc_cpp_plugin>)
  else()
    if (NOT GRPC_CPP_PLUGIN_EXECUTABLE)
      message(FATAL_ERROR "grpc_cpp_plugin not found")
    endif()
    set(GRPC_CPP_PLUGIN_PATH ${GRPC_CPP_PLUGIN_EXECUTABLE})
  endif()

  foreach(proto ${PROTO_FILES})
    get_filename_component(abs ${proto} ABSOLUTE)
    get_filename_component(fname ${proto} NAME_WE)
    file(RELATIVE_PATH relpath ${PROTO_ROOT} ${abs})
    get_filename_component(reldir ${relpath} DIRECTORY)
    set(pb_cc ${CMAKE_CURRENT_BINARY_DIR}/${GEN_SRC_DIR}/${reldir}/${fname}.pb.cc)
    set(pb_h  ${CMAKE_CURRENT_BINARY_DIR}/${GEN_SRC_DIR}/${reldir}/${fname}.pb.h)
    execute_process(
      COMMAND ${Protobuf_PROTOC_EXECUTABLE} --cpp_out=${CMAKE_CURRENT_BINARY_DIR}/${GEN_SRC_DIR} ${PROTO_INCLUDES} ${abs}
      RESULT_VARIABLE rv1
      COMMAND_ECHO STDOUT
    )
    if (NOT rv1 EQUAL 0)
      message(FATAL_ERROR "protoc cpp generation failed for ${abs}")
    endif()
    list(APPEND GNOI_PB_SRCS ${pb_cc})
    list(APPEND GNOI_PB_HDRS ${pb_h})
    if (fname STREQUAL "file" OR fname STREQUAL "packet_capture")
      set(grpc_cc ${CMAKE_CURRENT_BINARY_DIR}/${GEN_SRC_DIR}/${reldir}/${fname}.grpc.pb.cc)
      set(grpc_h  ${CMAKE_CURRENT_BINARY_DIR}/${GEN_SRC_DIR}/${reldir}/${fname}.grpc.pb.h)
      execute_process(
        COMMAND ${Protobuf_PROTOC_EXECUTABLE} --grpc_out=${CMAKE_CURRENT_BINARY_DIR}/${GEN_SRC_DIR} --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_PATH} ${PROTO_INCLUDES} ${abs}
        RESULT_VARIABLE rv2
        COMMAND_ECHO STDOUT
      )
      if (NOT rv2 EQUAL 0)
        message(FATAL_ERROR "protoc grpc generation failed for ${abs}")
      endif()
      list(APPEND GNOI_GRPC_SRCS ${grpc_cc})
      list(APPEND GNOI_GRPC_HDRS ${grpc_h})
    endif()
  endforeach()

  # 标记生成文件，避免配置期不存在导致错误
  set_source_files_properties(${GNOI_PB_SRCS} ${GNOI_GRPC_SRCS} PROPERTIES GENERATED TRUE)

  add_library(gnoi_file_test_protos ${GNOI_PB_SRCS} ${GNOI_GRPC_SRCS})
  target_include_directories(gnoi_file_test_protos PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/${GEN_SRC_DIR} ${PROTO_ROOT} ${GRPCPP_INCLUDE_DIRS} ${Protobuf_INCLUDE_DIRS})
  target_compile_options(gnoi_file_test_protos PUBLIC ${GRPCPP_CFLAGS_OTHER})
  target_link_libraries(gnoi_file_test_protos PUBLIC ${GRPCPP_LIBRARIES} ${Protobuf_LIBRARIES})

  # 将 standalone 的服务实现加入到测试目标
  target_sources(${PROJECT_NAME} PRIVATE 
    ${CMAKE_CURRENT_LIST_DIR}/../source/zurg/gnoi/file_service.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../source/zurg/gnoi/packet_capture_service.cpp
    ${CMAKE_CURRENT_LIST_DIR}/source/gnoi_file.cpp
  )
  target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/${GEN_SRC_DIR} ${CMAKE_CURRENT_LIST_DIR}/../include ${GRPCPP_INCLUDE_DIRS} ${Protobuf_INCLUDE_DIRS} ${PCAP_INCLUDE_DIRS})
  target_compile_options(${PROJECT_NAME} PRIVATE ${GRPCPP_CFLAGS_OTHER} ${PCAP_CFLAGS_OTHER})
  target_link_libraries(${PROJECT_NAME} PRIVATE gnoi_file_test_protos ${GRPCPP_LIBRARIES} OpenSSL::Crypto ${PCAP_LIBRARIES})
  target_compile_definitions(${PROJECT_NAME} PRIVATE
    GRPC_CALLBACK_API_NONEXPERIMENTAL=1
    ZURG_GNOI_TESTS_ENABLED=1
    ZURG_HAS_GNOI_FILE_SERVICE=1
    ZURG_HAS_GNOI_PCAP_SERVICE=1
  )
  target_compile_definitions(${PROJECT_NAME} PRIVATE ZURG_HAVE_OPENSSL=1 ZURG_HAVE_PCAP=1)
endif()
