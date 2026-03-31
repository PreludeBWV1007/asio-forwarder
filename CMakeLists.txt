cmake_minimum_required(VERSION 3.20)

project(asio_forwarder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(asio_forwarder
  src/main.cpp
)

target_include_directories(asio_forwarder PRIVATE include)
target_compile_options(asio_forwarder PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(asio_forwarder PRIVATE pthread boost_system)

