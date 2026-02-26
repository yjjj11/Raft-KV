cmake_minimum_required(VERSION 3.6)
# 项目名可自定义，不影响可执行程序生成
project(test_servers)

include_directories(${PROJECT_SOURCE_DIR}/src)

set(TEST_FILE1 ${PROJECT_SOURCE_DIR}/src/server/test_server.cpp)
set(TEST_FILE2 ${PROJECT_SOURCE_DIR}/src/server/other_server.cpp)

add_executable(test_server ${TEST_FILE1})
add_executable(other_server ${TEST_FILE2})

if(CMAKE_HOST_UNIX)
    target_link_libraries(test_server pthread)
    target_link_libraries(other_server pthread)
endif()