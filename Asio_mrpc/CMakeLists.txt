cmake_minimum_required(VERSION 3.6)

project(mrpc)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Generate compile_commands.json for clangd" FORCE)
add_definitions(-DASIO_STANDALONE -DRPC_USE_LOG -D_LOG_CONSOLE)

add_definitions(-DSPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_OFF)
if(CMAKE_HOST_WIN32)
    add_definitions(-D_WIN32_WINNT=0x0701 -DNTDDI_VERSION=NTDDI_WIN7 )
    add_definitions(/std:c++17 /utf-8)
else()
    add_definitions(-g -O0 -Wall -std=c++2a)
    # 查找pthread线程库（Linux/macOS）
    find_package(Threads REQUIRED)
    # 查找ZK多线程库
    find_library(ZK_LIB zookeeper_mt PATHS /usr/lib /usr/local/lib)
    if(NOT ZK_LIB)
        message(FATAL_ERROR "zookeeper_mt library not found! Please install zookeeper dev package first.")
    endif()
endif()

set(EXECUTABLE_OUTPUT_PATH      ${CMAKE_CURRENT_BINARY_DIR}/bin/  PARENT_SCOPE)
set(RPC_INCLUDE_DIR             ${CMAKE_CURRENT_SOURCE_DIR}/include/  PARENT_SCOPE)
set(ASIO_INCLUDE_DIR            ${CMAKE_CURRENT_SOURCE_DIR}/third/asio-1.18.1/include/  PARENT_SCOPE)   
set(SPDLOG_INCLUDE_DIR          ${CMAKE_CURRENT_SOURCE_DIR}/third/spdlog/include/  PARENT_SCOPE)
set(WLOG_INCLUDE_DIR            ${CMAKE_CURRENT_SOURCE_DIR}/third/wlog/include/  PARENT_SCOPE)
set(NLOHMANN_INCLUDE_DIR        ${CMAKE_CURRENT_SOURCE_DIR}/third/nlohmann/include/  PARENT_SCOPE)

message(VERBOSE "rpc        directory: " ${RPC_INCLUDE_DIR})
message(VERBOSE "asio       directory: " ${ASIO_INCLUDE_DIR})
message(VERBOSE "spdlog     directory: " ${SPDLOG_INCLUDE_DIR})
message(VERBOSE "wlog       directory: " ${WLOG_INCLUDE_DIR})
message(VERBOSE "nlohmann   directory: " ${NLOHMANN_INCLUDE_DIR})
message(VERBOSE "zookeeper  library: " ${ZK_LIB})

include_directories(
    ${RPC_INCLUDE_DIR}
    ${ASIO_INCLUDE_DIR}
    ${SPDLOG_INCLUDE_DIR}
    ${WLOG_INCLUDE_DIR}
    ${NLOHMANN_INCLUDE_DIR}
)

link_libraries(
        ${ZK_LIB}
        Threads::Threads
)

# 递归查找example下的所有子项目
# add_subdirectory(${CMAKE_HOME_DIRECTORY}/example/async_call_client)
# add_subdirectory(${CMAKE_HOME_DIRECTORY}/example/block_call_client)
# add_subdirectory(${CMAKE_HOME_DIRECTORY}/example/coro_call_client)
# add_subdirectory(${CMAKE_HOME_DIRECTORY}/example/server_register)
# add_subdirectory(${CMAKE_HOME_DIRECTORY}/example/two_way_call)
