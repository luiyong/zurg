cmake_minimum_required(VERSION 3.14...3.22)

project(ZurgAgent LANGUAGES CXX)

# --- Import tools ----

include(../cmake/tools.cmake)

# ---- Dependencies ----

include(../cmake/CPM.cmake)

CPMAddPackage(
  GITHUB_REPOSITORY jarro2783/cxxopts
  VERSION 3.0.0
  OPTIONS "CXXOPTS_BUILD_EXAMPLES NO" "CXXOPTS_BUILD_TESTS NO" "CXXOPTS_ENABLE_INSTALL YES"
)

CPMAddPackage(NAME zurg SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/..)

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

# ---- Create standalone executables ----

set(agent_sources
  ${CMAKE_CURRENT_SOURCE_DIR}/source/agent_main.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/agent/agent_impl.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/agent/tasks/task.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/agent/tasks/log_filter_task.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/agent/tasks/pcap_task.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/agent/tasks/exec_task.cpp
)

add_executable(${PROJECT_NAME} ${agent_sources})

set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 17 OUTPUT_NAME "zurg_agent")

target_link_libraries(${PROJECT_NAME}
  PRIVATE
    zurg::zurg
    cxxopts
    fmt::fmt
    ${GRPCPP_LINK_LIBS}
)

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
