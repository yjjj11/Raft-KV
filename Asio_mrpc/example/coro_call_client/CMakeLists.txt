cmake_minimum_required(VERSION 3.6)
project(coro_call_client)

add_definitions(-DPROJECT_NAME="${PROJECT_NAME}" -D_USE_COROUTINE )
if(CMAKE_HOST_WIN32)
    add_definitions(/await)
else()
    add_definitions(-fcoroutines)
endif()

include_directories( ${PROJECT_SOURCE_DIR}/src/ )
file(GLOB SOURCE_FILES ./src/*.cpp ./src/*.h ./src/client/*.cpp ./src/client/*.h)

add_executable(${PROJECT_NAME} ${SOURCE_FILES})
if(CMAKE_HOST_UNIX)
    target_link_libraries(${PROJECT_NAME} pthread)
endif()
