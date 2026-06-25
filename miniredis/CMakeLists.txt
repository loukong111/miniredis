cmake_minimum_required(VERSION 3.20)
project(miniredis VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 项目通过 option 控制测试维度，实现“开发构建”和“CI 构建”分离。
option(MINIREDIS_ENABLE_INTEGRATION_TESTS "Enable redis-cli/curl based integration tests" ON)
option(MINIREDIS_BUILD_QT_CONSOLE "Build the optional Qt demo console" OFF)

find_library(MYSQL_CLIENT_LIB mysqlclient)
if(MYSQL_CLIENT_LIB)
    message(STATUS "Found mysqlclient: ${MYSQL_CLIENT_LIB}")
    set(HAVE_MYSQL ON)
else()
    message(STATUS "mysqlclient not found, persistence disabled")
    set(HAVE_MYSQL OFF)
endif()

include_directories(${CMAKE_SOURCE_DIR}/include)

set(MINIREDIS_LIB_SOURCES
    ${CMAKE_SOURCE_DIR}/src/core/cache_store.cpp
    ${CMAKE_SOURCE_DIR}/src/core/memory_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/core/thread_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/net/scheduler.cpp
    ${CMAKE_SOURCE_DIR}/src/net/resp_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/persistence_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/cluster/slot_map.cpp
    ${CMAKE_SOURCE_DIR}/src/cluster/cluster_utils.cpp
    ${CMAKE_SOURCE_DIR}/src/metrics/stats.cpp
    ${CMAKE_SOURCE_DIR}/src/metrics/http_stats.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/file_persistence.cpp
    ${CMAKE_SOURCE_DIR}/src/server/config.cpp
    ${CMAKE_SOURCE_DIR}/src/server/command_handler.cpp
    ${CMAKE_SOURCE_DIR}/src/server/server.cpp
)

if(HAVE_MYSQL)
    list(APPEND MINIREDIS_LIB_SOURCES ${CMAKE_SOURCE_DIR}/src/cluster/mysql_client.cpp)
endif()

set(SERVER_SOURCES
    ${CMAKE_SOURCE_DIR}/main.cpp
    ${MINIREDIS_LIB_SOURCES}
)

add_executable(miniredis ${SERVER_SOURCES})

target_link_libraries(miniredis pthread)

if(HAVE_MYSQL)
    target_link_libraries(miniredis ${MYSQL_CLIENT_LIB})
    target_compile_definitions(miniredis PUBLIC HAVE_MYSQL)
endif()

if(MINIREDIS_BUILD_QT_CONSOLE)
    add_subdirectory(tools/qt_console)
endif()

enable_testing()

add_executable(miniredis_unit_tests
    ${CMAKE_SOURCE_DIR}/tests/unit_tests.cpp
    ${CMAKE_SOURCE_DIR}/src/core/cache_store.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/file_persistence.cpp
    ${CMAKE_SOURCE_DIR}/src/core/memory_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/core/thread_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/net/resp_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/cluster/slot_map.cpp
    ${CMAKE_SOURCE_DIR}/src/cluster/cluster_utils.cpp
    ${CMAKE_SOURCE_DIR}/src/metrics/stats.cpp
    ${CMAKE_SOURCE_DIR}/src/server/command_handler.cpp
)
target_link_libraries(miniredis_unit_tests pthread)
add_test(NAME miniredis_unit_tests COMMAND miniredis_unit_tests)

if(MINIREDIS_ENABLE_INTEGRATION_TESTS)
    find_program(BASH_PROGRAM bash)
    find_program(REDIS_CLI_PROGRAM redis-cli)
    find_program(CURL_PROGRAM curl)
    if(BASH_PROGRAM AND REDIS_CLI_PROGRAM AND CURL_PROGRAM)
        add_test(
            NAME miniredis_integration_smoke
            COMMAND ${BASH_PROGRAM}
                    ${CMAKE_SOURCE_DIR}/tests/integration/smoke.sh
                    $<TARGET_FILE:miniredis>
        )
        set_tests_properties(miniredis_integration_smoke PROPERTIES
            TIMEOUT 60
            RESOURCE_LOCK miniredis_ports
            ENVIRONMENT "REDIS_CLI=${REDIS_CLI_PROGRAM};CURL=${CURL_PROGRAM}"
        )
    else()
        message(STATUS "Skipping integration tests: bash, redis-cli, or curl not found")
    endif()
endif()
