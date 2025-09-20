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

find_package(PkgConfig REQUIRED)
pkg_check_modules(GRPCPP QUIET grpc++)
pkg_check_modules(GRPCPP_REFLECTION QUIET grpc++_reflection)
pkg_check_modules(PCAP QUIET libpcap)

find_program(Protobuf_PROTOC_EXECUTABLE NAMES protoc)
find_program(GRPC_CPP_PLUGIN_EXECUTABLE NAMES grpc_cpp_plugin)

if (GRPCPP_FOUND AND PCAP_FOUND AND Protobuf_PROTOC_EXECUTABLE)
  message(STATUS "Building gNOI File Server (Protobuf and gRPC found)")

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
    ${CMAKE_CURRENT_LIST_DIR}/../third_party/os.proto
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
    if (fname STREQUAL "file" OR fname STREQUAL "packet_capture" OR fname STREQUAL "os")
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
  target_include_directories(gnoi_file_protos PUBLIC ${GEN_SRC_DIR} ${PROTO_ROOT} ${GRPCPP_INCLUDE_DIRS} ${Protobuf_INCLUDE_DIRS})
  target_compile_options(gnoi_file_protos PUBLIC ${GRPCPP_CFLAGS_OTHER})
  target_link_libraries(gnoi_file_protos PUBLIC ${GRPCPP_LIBRARIES} ${Protobuf_LIBRARIES})

  option(ZURG_BUILD_GNOI_SERVER "Build gNOI server binaries" OFF)
  if (ZURG_BUILD_GNOI_SERVER)
    # Unified server: File + PacketCapture
    add_executable(GnoiServer
      ${CMAKE_CURRENT_LIST_DIR}/source/gnoi_server.cpp
      ${CMAKE_CURRENT_LIST_DIR}/../source/zurg/gnoi/file_service.cpp
      ${CMAKE_CURRENT_LIST_DIR}/../source/zurg/gnoi/packet_capture_service.cpp
    )
    add_dependencies(GnoiServer gnoi_file_protos)
    target_include_directories(GnoiServer PRIVATE ${GEN_SRC_DIR} ${CMAKE_CURRENT_LIST_DIR}/../include)
    target_compile_definitions(GnoiServer PRIVATE
      GRPC_CALLBACK_API_NONEXPERIMENTAL=1
      ZURG_HAS_GNOI_FILE_SERVICE=1
      ZURG_HAS_GNOI_PCAP_SERVICE=1
    )
    target_include_directories(GnoiServer PRIVATE ${GRPCPP_INCLUDE_DIRS} ${Protobuf_INCLUDE_DIRS} ${PCAP_INCLUDE_DIRS})
    target_compile_options(GnoiServer PRIVATE ${GRPCPP_CFLAGS_OTHER} ${PCAP_CFLAGS_OTHER})
    target_link_libraries(GnoiServer PRIVATE gnoi_file_protos ${GRPCPP_LIBRARIES} ${GRPCPP_REFLECTION_LIBRARIES} fmt::fmt spdlog::spdlog cxxopts OpenSSL::Crypto ${PCAP_LIBRARIES})
    target_compile_definitions(GnoiServer PRIVATE ZURG_HAVE_PCAP=1 ZURG_HAVE_OPENSSL=1)

    add_executable(GnoiFileServer
      ${CMAKE_CURRENT_LIST_DIR}/source/gnoi_file_server.cpp
      ${CMAKE_CURRENT_LIST_DIR}/../source/zurg/gnoi/file_service.cpp
    )
    add_dependencies(GnoiFileServer gnoi_file_protos)
    target_include_directories(GnoiFileServer PRIVATE ${GEN_SRC_DIR} ${CMAKE_CURRENT_LIST_DIR}/../include)
    target_compile_definitions(GnoiFileServer PRIVATE
      GRPC_CALLBACK_API_NONEXPERIMENTAL=1
      ZURG_HAS_GNOI_FILE_SERVICE=1
    )
    target_include_directories(GnoiFileServer PRIVATE ${GRPCPP_INCLUDE_DIRS} ${Protobuf_INCLUDE_DIRS})
    target_compile_options(GnoiFileServer PRIVATE ${GRPCPP_CFLAGS_OTHER})
    target_link_libraries(GnoiFileServer PRIVATE gnoi_file_protos ${GRPCPP_LIBRARIES} ${GRPCPP_REFLECTION_LIBRARIES} fmt::fmt spdlog::spdlog cxxopts OpenSSL::Crypto)
    target_compile_definitions(GnoiFileServer PRIVATE ZURG_HAVE_OPENSSL=1)

    add_executable(GnoiFileSelfTest
      ${CMAKE_CURRENT_LIST_DIR}/source/gnoi/gnoi_file_selftest.cpp
      ${CMAKE_CURRENT_LIST_DIR}/../source/zurg/gnoi/file_service.cpp
    )
    add_dependencies(GnoiFileSelfTest gnoi_file_protos)
    target_include_directories(GnoiFileSelfTest PRIVATE ${GEN_SRC_DIR} ${CMAKE_CURRENT_LIST_DIR}/../include)
    target_compile_definitions(GnoiFileSelfTest PRIVATE
      GRPC_CALLBACK_API_NONEXPERIMENTAL=1
      ZURG_HAS_GNOI_FILE_SERVICE=1
    )
    target_include_directories(GnoiFileSelfTest PRIVATE ${GRPCPP_INCLUDE_DIRS} ${Protobuf_INCLUDE_DIRS})
    target_compile_options(GnoiFileSelfTest PRIVATE ${GRPCPP_CFLAGS_OTHER})
    target_link_libraries(GnoiFileSelfTest PRIVATE gnoi_file_protos ${GRPCPP_LIBRARIES} fmt::fmt spdlog::spdlog OpenSSL::Crypto)
    target_compile_definitions(GnoiFileSelfTest PRIVATE ZURG_HAVE_OPENSSL=1)
    # Packet capture server (callback API skeleton)
    # Optional: standalone PacketCapture server (kept for dev use)
    add_executable(GnoiPcapServer
      ${CMAKE_CURRENT_LIST_DIR}/source/gnoi_pcap_server.cpp
      ${CMAKE_CURRENT_LIST_DIR}/../source/zurg/gnoi/packet_capture_service.cpp
    )
    add_dependencies(GnoiPcapServer gnoi_file_protos)
    target_include_directories(GnoiPcapServer PRIVATE ${GEN_SRC_DIR} ${CMAKE_CURRENT_LIST_DIR}/../include)
    target_compile_definitions(GnoiPcapServer PRIVATE
      GRPC_CALLBACK_API_NONEXPERIMENTAL=1
      ZURG_HAS_GNOI_PCAP_SERVICE=1
    )
    target_include_directories(GnoiPcapServer PRIVATE ${GRPCPP_INCLUDE_DIRS} ${Protobuf_INCLUDE_DIRS} ${PCAP_INCLUDE_DIRS})
    target_compile_options(GnoiPcapServer PRIVATE ${GRPCPP_CFLAGS_OTHER} ${PCAP_CFLAGS_OTHER})
    target_link_libraries(GnoiPcapServer PRIVATE gnoi_file_protos ${GRPCPP_LIBRARIES} ${GRPCPP_REFLECTION_LIBRARIES} fmt::fmt spdlog::spdlog cxxopts OpenSSL::Crypto ${PCAP_LIBRARIES})
    target_compile_definitions(GnoiPcapServer PRIVATE ZURG_HAVE_PCAP=1 ZURG_HAVE_OPENSSL=1)
  endif()

  # Agent (client) that connects to Control.Connect (ops.v1) via bidi stream
  add_executable(GnoiAgent
    ${CMAKE_CURRENT_LIST_DIR}/source/agent_main.cpp
    ${CMAKE_CURRENT_LIST_DIR}/source/agent/agent_impl.cpp
  )
  add_dependencies(GnoiAgent gnoi_file_protos)
  target_include_directories(GnoiAgent PRIVATE
    ${GEN_SRC_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../include
    ${CMAKE_CURRENT_LIST_DIR}/source
    ${CMAKE_CURRENT_LIST_DIR}/source/agent)
  target_compile_definitions(GnoiAgent PRIVATE GRPC_CALLBACK_API_NONEXPERIMENTAL=1)
  target_include_directories(GnoiAgent PRIVATE ${GRPCPP_INCLUDE_DIRS} ${Protobuf_INCLUDE_DIRS} ${PCAP_INCLUDE_DIRS})
  target_compile_options(GnoiAgent PRIVATE ${GRPCPP_CFLAGS_OTHER} ${PCAP_CFLAGS_OTHER})
  target_link_libraries(GnoiAgent PRIVATE gnoi_file_protos ${GRPCPP_LIBRARIES} fmt::fmt cxxopts OpenSSL::Crypto ${PCAP_LIBRARIES})
  target_compile_definitions(GnoiAgent PRIVATE ZURG_HAVE_OPENSSL=1 ZURG_HAVE_PCAP=1)
endif()
