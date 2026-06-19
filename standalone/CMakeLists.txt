cmake_minimum_required(VERSION 3.14...3.22)

project(ZurgAgent LANGUAGES CXX)

# --- Import tools ----

include(../cmake/tools.cmake)

# ---- Dependencies ----

include(../cmake/CPM.cmake)

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../third_party/cxxopts/CMakeLists.txt")
  set(CXXOPTS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(CXXOPTS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(CXXOPTS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../third_party/cxxopts
                   ${CMAKE_CURRENT_BINARY_DIR}/third_party/cxxopts)
else()
  CPMAddPackage(
    GITHUB_REPOSITORY jarro2783/cxxopts
    VERSION 3.0.0
    OPTIONS "CXXOPTS_BUILD_EXAMPLES NO" "CXXOPTS_BUILD_TESTS NO" "CXXOPTS_ENABLE_INSTALL YES"
  )
endif()

CPMAddPackage(NAME zurg SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/..)

find_package(Boost QUIET)

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../third_party/asio-grpc/CMakeLists.txt")
  set(ASIO_GRPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(ASIO_GRPC_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(ASIO_GRPC_INSTALL OFF CACHE BOOL "" FORCE)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../third_party/asio-grpc
                   ${CMAKE_CURRENT_BINARY_DIR}/third_party/asio-grpc)
else()
  CPMAddPackage(
    NAME asio-grpc
    GITHUB_REPOSITORY Tradias/asio-grpc
    VERSION 3.7.0
  )
endif()

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../third_party/cpp-httplib/CMakeLists.txt")
  set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)
  set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE BOOL "" FORCE)
  set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
  set(HTTPLIB_TEST OFF CACHE BOOL "" FORCE)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../third_party/cpp-httplib
                   ${CMAKE_CURRENT_BINARY_DIR}/third_party/cpp-httplib)
else()
  CPMAddPackage(
    NAME cpp-httplib
    GITHUB_REPOSITORY yhirose/cpp-httplib
    VERSION 0.20.0
    OPTIONS "HTTPLIB_INSTALL OFF" "HTTPLIB_REQUIRE_OPENSSL OFF"
  )
endif()

if(NOT TARGET nlohmann_json::nlohmann_json)
  find_package(nlohmann_json CONFIG QUIET)
endif()
if(NOT TARGET nlohmann_json::nlohmann_json)
  find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp
    HINTS
      ${CMAKE_CURRENT_LIST_DIR}/../third_party/json/include
      /opt/homebrew/include
      /usr/local/include
      /usr/include
  )
  if(NOT NLOHMANN_JSON_INCLUDE_DIR)
    CPMAddPackage(
      NAME nlohmann_json
      GITHUB_REPOSITORY nlohmann/json
      GIT_TAG v3.11.3
      OPTIONS "JSON_BuildTests OFF" "JSON_Install OFF"
    )
  else()
    add_library(nlohmann_json INTERFACE)
    add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
    target_include_directories(nlohmann_json INTERFACE ${NLOHMANN_JSON_INCLUDE_DIR})
  endif()
endif()

find_package(fmt CONFIG QUIET)
if(NOT TARGET fmt::fmt)
  CPMAddPackage(
    NAME fmt
    GIT_TAG 10.2.1
    GITHUB_REPOSITORY fmtlib/fmt
    OPTIONS "FMT_INSTALL YES"
  )
endif()

find_package(gRPC CONFIG QUIET)
find_package(yaml-cpp CONFIG QUIET)
set(GRPCPP_LINK_LIBS "")
set(GRPCPP_INCLUDE_DIRS_RES "")
set(GRPCPP_COMPILE_OPTS "")
set(GRPCPP_LIBRARY_DIRS_RES "")
if(gRPC_FOUND)
  list(APPEND GRPCPP_LINK_LIBS gRPC::grpc++)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GRPCPP QUIET grpc++)
  if(NOT GRPCPP_FOUND)
    message(FATAL_ERROR "gRPC++ (with re2) not found; install libgrpc++-dev or supply pkg-config files including re2")
  endif()
  list(APPEND GRPCPP_LINK_LIBS ${GRPCPP_LIBRARIES})
  list(APPEND GRPCPP_INCLUDE_DIRS_RES ${GRPCPP_INCLUDE_DIRS})
  list(APPEND GRPCPP_COMPILE_OPTS ${GRPCPP_CFLAGS_OTHER})
  list(APPEND GRPCPP_LIBRARY_DIRS_RES ${GRPCPP_LIBRARY_DIRS})
endif()

if(NOT TARGET yaml-cpp::yaml-cpp)
  CPMAddPackage(
    NAME yaml-cpp
    GITHUB_REPOSITORY jbeder/yaml-cpp
    GIT_TAG 0.8.0
    OPTIONS "YAML_CPP_BUILD_TESTS OFF" "YAML_CPP_BUILD_TOOLS OFF" "YAML_CPP_INSTALL OFF"
  )
endif()

# ---- Create standalone executables ----

set(agent_sources
  ${CMAKE_CURRENT_SOURCE_DIR}/source/agent_main.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/agent/config/agent_config.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/agent/http/http_server.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/agent/agent_impl.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/agent/control/control_asio_client.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/agent/control/control_stream_client.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/agent/control/control_callback_client.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/agent/runtime/jsonl_task_store.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/agent/runtime/grpc_task_sink.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/agent/runtime/task_context_adapter.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/agent/runtime/task_event.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/agent/runtime/task_factory.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/agent/runtime/task_query_service.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/agent/runtime/task_scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/agent/system/process_info.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/agent/tasks/task.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/agent/tasks/log_filter_task.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/agent/tasks/pcap_task.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/agent/tasks/exec_task.cpp
)

add_executable(${PROJECT_NAME} ${agent_sources})

set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES OUTPUT_NAME "zurg_agent")

target_link_libraries(${PROJECT_NAME}
  PRIVATE
    zurg::zurg
    cxxopts
    fmt::fmt
    yaml-cpp::yaml-cpp
    httplib::httplib
    nlohmann_json::nlohmann_json
    ${GRPCPP_LINK_LIBS}
)

if(TARGET asio-grpc::asio-grpc)
  target_link_libraries(${PROJECT_NAME} PRIVATE asio-grpc::asio-grpc)
endif()

if(TARGET Boost::headers)
  target_link_libraries(${PROJECT_NAME} PRIVATE Boost::headers)
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/source
  ${CMAKE_CURRENT_SOURCE_DIR}/source/agent
  ${GRPCPP_INCLUDE_DIRS_RES}
)

if(GRPCPP_LIBRARY_DIRS_RES)
  target_link_directories(${PROJECT_NAME} PRIVATE ${GRPCPP_LIBRARY_DIRS_RES})
endif()

if(GRPCPP_COMPILE_OPTS)
  target_compile_options(${PROJECT_NAME} PRIVATE ${GRPCPP_COMPILE_OPTS})
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE GRPC_CALLBACK_API_NONEXPERIMENTAL=1)

# ---- gNOI integration removed; standalone binaries depend solely on os.proto ----
