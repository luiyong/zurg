cmake_minimum_required(VERSION 3.14...3.22)

project(GreeterStandalone LANGUAGES CXX)

# --- Import tools ----

include(../cmake/tools.cmake)

# ---- Dependencies ----

include(../cmake/CPM.cmake)

CPMAddPackage(
  GITHUB_REPOSITORY jarro2783/cxxopts
  VERSION 3.0.0
  OPTIONS "CXXOPTS_BUILD_EXAMPLES NO" "CXXOPTS_BUILD_TESTS NO" "CXXOPTS_ENABLE_INSTALL YES"
)

CPMAddPackage(NAME Greeter SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/..)

# ---- Create standalone executables ----

set(sources ${CMAKE_CURRENT_SOURCE_DIR}/source/main.cpp)

add_executable(${PROJECT_NAME} ${sources})

set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 17 OUTPUT_NAME "Greeter")

target_link_libraries(${PROJECT_NAME} Greeter::Greeter cxxopts)

# ---- gNOI File Server (gRPC callback) ----

find_package(gRPC QUIET)
find_package(OpenSSL QUIET)

find_program(Protobuf_PROTOC_EXECUTABLE NAMES protoc)
find_program(GRPC_CPP_PLUGIN_EXECUTABLE NAMES grpc_cpp_plugin)

if (gRPC_FOUND AND Protobuf_PROTOC_EXECUTABLE)
  message(STATUS "Building gNOI File Server (Protobuf and gRPC found)")

  set(PROTO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../third_party)
  get_filename_component(PROTO_ROOT ${PROTO_ROOT} REALPATH)
  set(GNOI_PROTO_DIR ${PROTO_ROOT}/github.com/openconfig/gnoi)

  set(PROTO_FILES
    ${GNOI_PROTO_DIR}/types/types.proto
    ${GNOI_PROTO_DIR}/common/common.proto
    ${GNOI_PROTO_DIR}/file/file.proto
  )

  # Generated files output dir
  set(GEN_SRC_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
  file(MAKE_DIRECTORY ${GEN_SRC_DIR})

  # Generate protobuf sources
  # 解析 grpc_cpp_plugin 路径
  if (TARGET grpc_cpp_plugin)
    set(GRPC_CPP_PLUGIN_ARG --plugin=protoc-gen-grpc=$<TARGET_FILE:grpc_cpp_plugin>)
  elseif (TARGET gRPC::grpc_cpp_plugin)
    set(GRPC_CPP_PLUGIN_ARG --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>)
  elseif (GRPC_CPP_PLUGIN_EXECUTABLE)
    set(GRPC_CPP_PLUGIN_ARG --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXECUTABLE})
  else()
    message(STATUS "grpc_cpp_plugin not found as target; will rely on PATH")
    set(GRPC_CPP_PLUGIN_ARG)
  endif()

  foreach(proto ${PROTO_FILES})
    get_filename_component(fname ${proto} NAME_WE)
    get_filename_component(dir ${proto} DIRECTORY)
  endforeach()

  set(PROTO_INCLUDES -I ${PROTO_ROOT})

  # file.proto needs grpc generation; others only pb
  set(GNOI_PB_SRCS)
  set(GNOI_PB_HDRS)
  set(GNOI_GRPC_SRCS)
  set(GNOI_GRPC_HDRS)

  foreach(proto ${PROTO_FILES})
    get_filename_component(abs ${proto} ABSOLUTE)
    get_filename_component(fname ${proto} NAME_WE)
    file(RELATIVE_PATH relpath ${PROTO_ROOT} ${abs})
    get_filename_component(reldir ${relpath} DIRECTORY)
    set(pb_cc ${GEN_SRC_DIR}/${reldir}/${fname}.pb.cc)
    set(pb_h  ${GEN_SRC_DIR}/${reldir}/${fname}.pb.h)
    add_custom_command(
      OUTPUT ${pb_cc} ${pb_h}
      COMMAND ${Protobuf_PROTOC_EXECUTABLE}
      ARGS --cpp_out=${GEN_SRC_DIR} ${PROTO_INCLUDES} ${abs}
      DEPENDS ${abs}
      COMMENT "Generating C++ protobuf for ${proto}"
      VERBATIM)
    list(APPEND GNOI_PB_SRCS ${pb_cc})
    list(APPEND GNOI_PB_HDRS ${pb_h})
    if (fname STREQUAL "file")
      set(grpc_cc ${GEN_SRC_DIR}/${reldir}/${fname}.grpc.pb.cc)
      set(grpc_h  ${GEN_SRC_DIR}/${reldir}/${fname}.grpc.pb.h)
      add_custom_command(
        OUTPUT ${grpc_cc} ${grpc_h}
        COMMAND ${Protobuf_PROTOC_EXECUTABLE}
        ARGS --grpc_out=${GEN_SRC_DIR} ${GRPC_CPP_PLUGIN_ARG} ${PROTO_INCLUDES} ${abs}
        DEPENDS ${abs}
        COMMENT "Generating gRPC C++ for ${proto}"
        VERBATIM)
      list(APPEND GNOI_GRPC_SRCS ${grpc_cc})
      list(APPEND GNOI_GRPC_HDRS ${grpc_h})
    endif()
  endforeach()

  add_library(gnoi_file_protos ${GNOI_PB_SRCS} ${GNOI_GRPC_SRCS})
  target_include_directories(gnoi_file_protos PUBLIC ${GEN_SRC_DIR} ${PROTO_ROOT})
  target_link_libraries(gnoi_file_protos PUBLIC gRPC::grpc++
    $<IF:$<BOOL:${OpenSSL_FOUND}>,OpenSSL::SSL,>
    $<IF:$<BOOL:${OpenSSL_FOUND}>,OpenSSL::Crypto,>
  )

  option(ZURG_BUILD_GNOI_SERVER "Build gNOI server binaries" OFF)
  if (ZURG_BUILD_GNOI_SERVER)
    add_executable(GnoiFileServer
      ${CMAKE_CURRENT_LIST_DIR}/source/gnoi_file_server.cpp
      ${CMAKE_CURRENT_LIST_DIR}/source/gnoi/file_service.cpp
    )
    target_include_directories(GnoiFileServer PRIVATE ${GEN_SRC_DIR} ${CMAKE_CURRENT_LIST_DIR}/../include)
    target_compile_definitions(GnoiFileServer PRIVATE GRPC_CALLBACK_API_NONEXPERIMENTAL=1)
    target_link_libraries(GnoiFileServer PRIVATE gnoi_file_protos gRPC::grpc++ gRPC::grpc++_reflection fmt::fmt spdlog::spdlog cxxopts)
    if (OpenSSL_FOUND)
      target_link_libraries(GnoiFileServer PRIVATE OpenSSL::Crypto)
    endif()

    add_executable(GnoiFileSelfTest
      ${CMAKE_CURRENT_LIST_DIR}/source/gnoi/gnoi_file_selftest.cpp
      ${CMAKE_CURRENT_LIST_DIR}/source/gnoi/file_service.cpp
    )
    target_include_directories(GnoiFileSelfTest PRIVATE ${GEN_SRC_DIR} ${CMAKE_CURRENT_LIST_DIR}/../include)
    target_compile_definitions(GnoiFileSelfTest PRIVATE GRPC_CALLBACK_API_NONEXPERIMENTAL=1)
    target_link_libraries(GnoiFileSelfTest PRIVATE gnoi_file_protos gRPC::grpc++ fmt::fmt spdlog::spdlog)
    if (OpenSSL_FOUND)
      target_link_libraries(GnoiFileSelfTest PRIVATE OpenSSL::Crypto)
      target_compile_definitions(GnoiFileSelfTest PRIVATE ZURG_HAVE_OPENSSL=1)
    endif()
  endif()
endif()
