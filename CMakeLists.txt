cmake_minimum_required(VERSION 3.20)

project(asio_forwarder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Protobuf REQUIRED)

file(GLOB PROTO_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/proto/*.proto")
list(SORT PROTO_FILES)
if(NOT PROTO_FILES)
  message(FATAL_ERROR "no .proto files under proto/")
endif()

protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})

add_library(market_proto STATIC ${PROTO_SRCS} ${PROTO_HDRS})
target_include_directories(market_proto PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(market_proto PUBLIC protobuf::libprotobuf)

add_executable(asio_forwarder
  src/main.cpp
)
target_include_directories(asio_forwarder PRIVATE include)
target_compile_options(asio_forwarder PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(asio_forwarder PRIVATE pthread boost_system)

add_executable(proto_send src/proto_send.cpp)
target_include_directories(proto_send PRIVATE include)
target_compile_options(proto_send PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(proto_send PRIVATE market_proto)

add_executable(proto_recv src/proto_recv.cpp)
target_include_directories(proto_recv PRIVATE include)
target_compile_options(proto_recv PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(proto_recv PRIVATE market_proto)

enable_testing()
add_test(
  NAME e2e_forwarder
  COMMAND "${CMAKE_SOURCE_DIR}/scripts/test_e2e.sh"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(e2e_forwarder PROPERTIES TIMEOUT 120)
