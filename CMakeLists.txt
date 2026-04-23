cmake_minimum_required(VERSION 3.20)

project(asio_forwarder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

add_executable(asio_forwarder
  src/main.cpp
)
target_include_directories(asio_forwarder PRIVATE include)
target_compile_options(asio_forwarder PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(asio_forwarder PRIVATE pthread boost_system msgpack-cxx)
target_compile_definitions(asio_forwarder PRIVATE MSGPACK_NO_BOOST)

add_library(asio_forwarder_sdk
  src/relay_client.cpp
  src/asio_forwarder_client.cpp
)
target_include_directories(asio_forwarder_sdk PUBLIC include)
target_compile_options(asio_forwarder_sdk PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(asio_forwarder_sdk PUBLIC pthread boost_system msgpack-cxx)
target_compile_definitions(asio_forwarder_sdk PUBLIC MSGPACK_NO_BOOST)

enable_testing()
add_test(
  NAME e2e_forwarder
  COMMAND "${CMAKE_SOURCE_DIR}/scripts/test_e2e.sh"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(e2e_forwarder PROPERTIES TIMEOUT 300)

add_executable(dispatcher_cpp examples/realistic_scenario_cpp/dispatcher.cpp)
target_link_libraries(dispatcher_cpp PRIVATE asio_forwarder_sdk)
target_include_directories(dispatcher_cpp PRIVATE include)

add_executable(worker_cpp examples/realistic_scenario_cpp/worker.cpp)
target_link_libraries(worker_cpp PRIVATE asio_forwarder_sdk)
target_include_directories(worker_cpp PRIVATE include)

add_executable(admin_cpp examples/realistic_scenario_cpp/admin.cpp)
target_link_libraries(admin_cpp PRIVATE asio_forwarder_sdk)
target_include_directories(admin_cpp PRIVATE include)

# 黑盒一体化：单文件内含服务端自启动 + 客户端基础/压力/稳定性（可与 ctest 联跑 --connect）
add_executable(asio_forwarder_client_smoke examples/asio_forwarder_client_smoke/smoke.cpp)
target_link_libraries(asio_forwarder_client_smoke PRIVATE asio_forwarder_sdk)
target_include_directories(asio_forwarder_client_smoke PRIVATE include examples/realistic_scenario_cpp)
