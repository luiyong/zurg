cmake_minimum_required(VERSION 3.14...3.22)

# ---- Project ----

# Note: update this to your new project's name and version
project(
  Greeter
  VERSION 1.0
  LANGUAGES CXX
)

# ---- Include guards ----

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(
    FATAL_ERROR
      "In-source builds not allowed. Please make a new directory (called a build directory) and run CMake from there."
  )
endif()

# ---- Add dependencies via CPM ----
# see https://github.com/TheLartians/CPM.cmake for more info

include(cmake/CPM.cmake)

# PackageProject.cmake will be used to make our target installable
CPMAddPackage("gh:TheLartians/PackageProject.cmake@1.8.0")

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

# Add eventpp dependency
CPMAddPackage(
  NAME eventpp
  GIT_TAG v0.1.3
  GITHUB_REPOSITORY wqking/eventpp
)

find_package(Protobuf REQUIRED)

set(PROTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
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
  COMMENT "Generating C++ sources from os.proto"
  VERBATIM
)

# ---- Add source files ----

# Note: globbing sources is considered bad practice as CMake's generators may not detect new files
# automatically. Keep that in mind when changing files, or explicitly mention them here.
file(GLOB_RECURSE headers CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h")
file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/source/*.cpp")

list(APPEND headers ${OS_PROTO_HDR})
list(APPEND sources ${OS_PROTO_SRC})

# ---- Create library ----

# Note: for header-only libraries change all PUBLIC flags to INTERFACE and create an interface
# target: add_library(${PROJECT_NAME} INTERFACE)
add_library(${PROJECT_NAME} ${headers} ${sources})
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 17)

# being a cross-platform target, we enforce standards conformance on MSVC
target_compile_options(${PROJECT_NAME} PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/permissive->")

# Link dependencies
target_link_libraries(${PROJECT_NAME} PRIVATE fmt::fmt protobuf::libprotobuf)

# Include directories for header-only dependencies
target_include_directories(
  ${PROJECT_NAME} PUBLIC 
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/${PROJECT_NAME}-${PROJECT_VERSION}>
    $<BUILD_INTERFACE:${spdlog_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${eventpp_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${GENERATED_PROTO_DIR}>
)

# ---- Create an installable target ----
# this allows users to install and find the library via `find_package()`.

# the location where the project's version header will be placed should match the project's regular
# header paths
string(TOLOWER ${PROJECT_NAME}/version.h VERSION_HEADER_LOCATION)

packageProject(
  NAME ${PROJECT_NAME}
  VERSION ${PROJECT_VERSION}
  NAMESPACE ${PROJECT_NAME}
  BINARY_DIR ${PROJECT_BINARY_DIR}
  INCLUDE_DIR ${PROJECT_SOURCE_DIR}/include
  INCLUDE_DESTINATION include/${PROJECT_NAME}-${PROJECT_VERSION}
  VERSION_HEADER "${VERSION_HEADER_LOCATION}"
  COMPATIBILITY SameMajorVersion
  DEPENDENCIES "fmt 10.2.1;protobuf"
)

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
