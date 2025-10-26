cmake_minimum_required(VERSION 3.14...3.22)

# ---- Project ----

# Note: update this to your new project's name and version
project(
  zurg
  VERSION 0.1.0
  LANGUAGES CXX
)

# ---- Include guards ----

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(
    FATAL_ERROR
      "In-source builds not allowed. Please make a new directory (called a build directory) and run CMake from there."
  )
endif()

if(EXISTS "${PROJECT_SOURCE_DIR}/test/CMakeLists.txt")
  add_subdirectory(test)
endif()

# ---- Add dependencies via CPM ----
# see https://github.com/TheLartians/CPM.cmake for more info

include(cmake/CPM.cmake)

CPMAddPackage(
  NAME fmt
  GIT_TAG 10.2.1
  GITHUB_REPOSITORY fmtlib/fmt
  OPTIONS "FMT_INSTALL YES" # create an installable target
)

# Add spdlog dependency
CPMAddPackage(
  NAME spdlog
  GIT_TAG v1.14.1
  GITHUB_REPOSITORY gabime/spdlog
)

CPMAddPackage(
  NAME eventpp
  GITHUB_REPOSITORY wqking/eventpp
  GIT_TAG v0.1.3
  DOWNLOAD_ONLY YES
)

if(eventpp_ADDED)
  add_library(eventpp INTERFACE)
  target_include_directories(eventpp INTERFACE ${eventpp_SOURCE_DIR}/include)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(PCAP REQUIRED libpcap)

find_package(Protobuf CONFIG REQUIRED)
if (TARGET protobuf::protoc)
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
set(GRPCPP_LINK_LIBS "")
set(GRPCPP_INCLUDE_DIRS "")
set(GRPCPP_COMPILE_OPTS "")
set(GRPCPP_LIBRARY_DIRS "")
if(gRPC_FOUND)
  list(APPEND GRPCPP_LINK_LIBS gRPC::grpc++ gRPC::grpc)
else()
  pkg_check_modules(GRPCPP REQUIRED grpc++)
  list(APPEND GRPCPP_LINK_LIBS ${GRPCPP_LIBRARIES})
  list(APPEND GRPCPP_INCLUDE_DIRS ${GRPCPP_INCLUDE_DIRS})
  list(APPEND GRPCPP_COMPILE_OPTS ${GRPCPP_CFLAGS_OTHER})
  list(APPEND GRPCPP_LIBRARY_DIRS ${GRPCPP_LIBRARY_DIRS})
endif()

find_program(GRPC_CPP_PLUGIN_EXECUTABLE NAMES grpc_cpp_plugin
  HINTS /usr/bin /usr/local/bin /opt/homebrew/bin)
if(NOT GRPC_CPP_PLUGIN_EXECUTABLE)
  message(FATAL_ERROR "grpc_cpp_plugin not found; install gRPC development packages")
endif()

set(PROTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set(GENERATED_PROTO_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(OS_PROTO ${PROTO_ROOT}/os.proto)
set(OS_PROTO_SRC ${GENERATED_PROTO_DIR}/os.pb.cc)
set(OS_PROTO_HDR ${GENERATED_PROTO_DIR}/os.pb.h)
set(OS_GRPC_PROTO_SRC ${GENERATED_PROTO_DIR}/os.grpc.pb.cc)
set(OS_GRPC_PROTO_HDR ${GENERATED_PROTO_DIR}/os.grpc.pb.h)
set(AUTH_PROTO ${PROTO_ROOT}/auth/auth.proto)
set(AUTH_PROTO_OUTPUT_DIR ${GENERATED_PROTO_DIR}/auth)
set(AUTH_PROTO_SRC ${AUTH_PROTO_OUTPUT_DIR}/auth.pb.cc)
set(AUTH_PROTO_HDR ${AUTH_PROTO_OUTPUT_DIR}/auth.pb.h)
set(AUTH_GRPC_PROTO_SRC ${AUTH_PROTO_OUTPUT_DIR}/auth.grpc.pb.cc)
set(AUTH_GRPC_PROTO_HDR ${AUTH_PROTO_OUTPUT_DIR}/auth.grpc.pb.h)

file(MAKE_DIRECTORY ${GENERATED_PROTO_DIR})
file(MAKE_DIRECTORY ${AUTH_PROTO_OUTPUT_DIR})

add_custom_command(
  OUTPUT ${OS_PROTO_SRC} ${OS_PROTO_HDR} ${OS_GRPC_PROTO_SRC} ${OS_GRPC_PROTO_HDR}
  COMMAND ${Protobuf_PROTOC_EXECUTABLE}
  ARGS --cpp_out=${GENERATED_PROTO_DIR}
       --grpc_out=${GENERATED_PROTO_DIR}
       --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXECUTABLE}
       -I ${PROTO_ROOT} ${OS_PROTO}
  COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/cmake/fix_grpc_namespace.py
          ${OS_GRPC_PROTO_SRC} ${OS_GRPC_PROTO_HDR}
  DEPENDS ${OS_PROTO}
  COMMENT "Generating C++ and gRPC sources from os.proto"
  VERBATIM
)

add_custom_command(
  OUTPUT ${AUTH_PROTO_SRC} ${AUTH_PROTO_HDR} ${AUTH_GRPC_PROTO_SRC} ${AUTH_GRPC_PROTO_HDR}
  COMMAND ${Protobuf_PROTOC_EXECUTABLE}
  ARGS --cpp_out=${GENERATED_PROTO_DIR}
       --grpc_out=${GENERATED_PROTO_DIR}
       --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXECUTABLE}
       -I ${PROTO_ROOT} ${AUTH_PROTO}
  COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/cmake/fix_grpc_namespace.py
          ${AUTH_GRPC_PROTO_SRC} ${AUTH_GRPC_PROTO_HDR}
  DEPENDS ${AUTH_PROTO}
  COMMENT "Generating C++ sources from auth.proto"
  VERBATIM
)

# ---- Add source files ----

# Note: globbing sources is considered bad practice as CMake's generators may not detect new files
# automatically. Keep that in mind when changing files, or explicitly mention them here.
file(GLOB_RECURSE headers CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h")
file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/source/*.cpp")

list(APPEND headers ${OS_PROTO_HDR} ${OS_GRPC_PROTO_HDR} ${AUTH_PROTO_HDR} ${AUTH_GRPC_PROTO_HDR})
list(APPEND sources ${OS_PROTO_SRC} ${OS_GRPC_PROTO_SRC} ${AUTH_PROTO_SRC} ${AUTH_GRPC_PROTO_SRC})

# ---- Create library ----

# Note: for header-only libraries change all PUBLIC flags to INTERFACE and create an interface
# target: add_library(${PROJECT_NAME} INTERFACE)
add_library(${PROJECT_NAME} ${headers} ${sources})
add_library(${PROJECT_NAME}::${PROJECT_NAME} ALIAS ${PROJECT_NAME})
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 17)

# being a cross-platform target, we enforce standards conformance on MSVC
target_compile_options(${PROJECT_NAME} PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/permissive->")

# Link dependencies
target_link_libraries(
  ${PROJECT_NAME}
  PUBLIC spdlog::spdlog protobuf::libprotobuf ${PCAP_LIBRARIES} ${GRPCPP_LINK_LIBS} eventpp
)

if(GRPCPP_LIBRARY_DIRS)
  target_link_directories(${PROJECT_NAME} PUBLIC ${GRPCPP_LIBRARY_DIRS})
endif()

# Include directories for header-only dependencies
target_include_directories(
  ${PROJECT_NAME} PUBLIC 
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/${PROJECT_NAME}-${PROJECT_VERSION}>
    $<BUILD_INTERFACE:${GENERATED_PROTO_DIR}>
    $<BUILD_INTERFACE:${PCAP_INCLUDE_DIRS}>
    $<BUILD_INTERFACE:${GRPCPP_INCLUDE_DIRS}>
)
target_compile_options(${PROJECT_NAME} PRIVATE ${PCAP_CFLAGS_OTHER} ${GRPCPP_COMPILE_OPTS})

# ---- Formatting checks ----
find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format)
if(CLANG_FORMAT_EXECUTABLE)
  file(GLOB_RECURSE CLANG_FORMAT_SOURCES
    ${PROJECT_SOURCE_DIR}/include/*.h
    ${PROJECT_SOURCE_DIR}/source/*.cpp
    ${PROJECT_SOURCE_DIR}/standalone/source/*.cpp
    ${PROJECT_SOURCE_DIR}/standalone/source/*.h
    ${PROJECT_SOURCE_DIR}/test/source/*.cpp
    ${PROJECT_SOURCE_DIR}/test/source/*.h
  )
  if(CLANG_FORMAT_SOURCES)
    add_custom_target(check-clang-format
      COMMAND ${CLANG_FORMAT_EXECUTABLE} --dry-run --Werror ${CLANG_FORMAT_SOURCES}
      COMMENT "Checking C/C++ formatting with clang-format"
      WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    )
  endif()
endif()

find_program(CMAKE_FORMAT_EXECUTABLE NAMES cmake-format)
if(CMAKE_FORMAT_EXECUTABLE)
  set(CMAKE_FORMAT_FILES
    ${PROJECT_SOURCE_DIR}/CMakeLists.txt
    ${PROJECT_SOURCE_DIR}/standalone/CMakeLists.txt
    ${PROJECT_SOURCE_DIR}/test/CMakeLists.txt
    ${PROJECT_SOURCE_DIR}/cmake/tools.cmake
  )
  add_custom_target(check-cmake-format
    COMMAND ${CMAKE_FORMAT_EXECUTABLE} --config-files ${PROJECT_SOURCE_DIR}/cmake-format.yaml --check ${CMAKE_FORMAT_FILES}
    COMMENT "Checking CMake formatting with cmake-format"
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
  )
endif()

if(TARGET check-clang-format OR TARGET check-cmake-format)
  add_custom_target(check-format)
  if(TARGET check-clang-format)
    add_dependencies(check-format check-clang-format)
  endif()
  if(TARGET check-cmake-format)
    add_dependencies(check-format check-cmake-format)
  endif()
else()
  add_custom_target(check-format
    COMMAND ${CMAKE_COMMAND} -E echo "check-format skipped: clang-format and cmake-format not available"
  )
endif()
