cmake_minimum_required(VERSION 3.20)
project(miniredis VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_library(MYSQL_CLIENT_LIB mysqlclient)
if(MYSQL_CLIENT_LIB)
    message(STATUS "Found mysqlclient: ${MYSQL_CLIENT_LIB}")
    set(HAVE_MYSQL ON)
else()
    message(STATUS "mysqlclient not found, persistence disabled")
    set(HAVE_MYSQL OFF)
endif()

include_directories(${CMAKE_SOURCE_DIR}/include)

# 明确列出所有源文件（main.cpp + src/*.cpp）
set(SERVER_SOURCES
    ${CMAKE_SOURCE_DIR}/main.cpp
    ${CMAKE_SOURCE_DIR}/src/cache_store.cpp
    ${CMAKE_SOURCE_DIR}/src/memory_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/scheduler.cpp
    ${CMAKE_SOURCE_DIR}/src/thread_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/resp_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/mysql_client.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/consistent_hash.cpp
    ${CMAKE_SOURCE_DIR}/src/stats.cpp
    ${CMAKE_SOURCE_DIR}/src/http_stats.cpp
)

add_executable(miniredis ${SERVER_SOURCES})

target_link_libraries(miniredis pthread)

if(HAVE_MYSQL)
    target_link_libraries(miniredis ${MYSQL_CLIENT_LIB})
    target_compile_definitions(miniredis PUBLIC HAVE_MYSQL)
endif()

if(BUILD_MONITOR)
    add_executable(miniredis_monitor ${CMAKE_SOURCE_DIR}/src/monitor.cpp)
    target_link_libraries(miniredis_monitor Qt6::Core Qt6::Widgets)
    target_compile_definitions(miniredis_monitor PUBLIC BUILD_MONITOR)
endif()