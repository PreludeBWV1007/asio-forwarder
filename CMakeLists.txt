cmake_minimum_required(VERSION 3.20)

project(asio_forwarder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(DELIVER_ROOT "${CMAKE_SOURCE_DIR}/deliver")
set(LOCAL_ROOT "${CMAKE_SOURCE_DIR}/local")

include(FetchContent)
FetchContent_Declare(
  msgpack_c
  URL https://github.com/msgpack/msgpack-c/archive/refs/tags/cpp-6.1.1.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(MSGPACK_USE_BOOST OFF CACHE INTERNAL "")
set(MSGPACK_CXX20 ON CACHE INTERNAL "")
set(MSGPACK_BUILD_DOCS OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(msgpack_c)

find_path(MYSQL_INCLUDE_DIR NAMES mysql/mysql.h PATHS /usr/include/mysql /usr/include/mariadb)
find_library(MYSQL_LIBRARY NAMES mysqlclient mariadb PATHS /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib)
if (NOT MYSQL_INCLUDE_DIR OR NOT MYSQL_LIBRARY)
  message(FATAL_ERROR "MySQL client dev required: install libmysqlclient-dev or mariadb-connector-c-devel (mysql.h + libmysqlclient)")
endif()

add_executable(asio_forwarder
  "${DELIVER_ROOT}/server/src/main.cpp"
)
target_include_directories(asio_forwarder PRIVATE
  "${DELIVER_ROOT}/client/include"
  "${DELIVER_ROOT}/server/include"
)
target_compile_options(asio_forwarder PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(asio_forwarder PRIVATE pthread boost_system msgpack-cxx "${MYSQL_LIBRARY}")
target_compile_definitions(asio_forwarder PRIVATE MSGPACK_NO_BOOST)
target_include_directories(asio_forwarder SYSTEM PRIVATE "${MYSQL_INCLUDE_DIR}")

add_library(asio_forwarder_sdk
  "${DELIVER_ROOT}/client/src/forwarder_sdk.cpp"
)
target_include_directories(asio_forwarder_sdk PUBLIC "${DELIVER_ROOT}/client/include")
target_compile_options(asio_forwarder_sdk PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(asio_forwarder_sdk PUBLIC pthread boost_system msgpack-cxx)
target_compile_definitions(asio_forwarder_sdk PUBLIC MSGPACK_NO_BOOST)

enable_testing()
add_test(
  NAME e2e_forwarder
  COMMAND "${LOCAL_ROOT}/tests/run_e2e.sh"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(e2e_forwarder PROPERTIES TIMEOUT 300)

add_executable(forwarder_perf "${LOCAL_ROOT}/tests/perf_basic.cpp")
target_link_libraries(forwarder_perf PRIVATE asio_forwarder_sdk)
target_include_directories(forwarder_perf PRIVATE "${DELIVER_ROOT}/client/include")

add_executable(usage_instruction "${LOCAL_ROOT}/examples/usage_instruction.cpp")
target_link_libraries(usage_instruction PRIVATE asio_forwarder_sdk)
target_include_directories(usage_instruction PRIVATE "${DELIVER_ROOT}/client/include")
