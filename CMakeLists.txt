cmake_minimum_required(VERSION 3.20)
project(miniredis VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 项目通过 option 控制测试维度，实现“开发构建”和“CI 构建”分离。
option(MINIREDIS_ENABLE_INTEGRATION_TESTS "Enable redis-cli/curl based integration tests" ON)
option(MINIREDIS_ENABLE_MYSQL_DISCOVERY "Build optional MySQL-backed cluster discovery" ON)
option(MINIREDIS_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(MINIREDIS_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(MINIREDIS_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(MINIREDIS_DEFAULT_BUILD_SERVER ON)
    set(MINIREDIS_DEFAULT_BUILD_TESTS ON)
else()
    set(MINIREDIS_DEFAULT_BUILD_SERVER OFF)
    set(MINIREDIS_DEFAULT_BUILD_TESTS OFF)
endif()

option(MINIREDIS_BUILD_SERVER "Build the Linux MiniRedis server" ${MINIREDIS_DEFAULT_BUILD_SERVER})
option(MINIREDIS_BUILD_TESTS "Build MiniRedis unit/integration tests" ${MINIREDIS_DEFAULT_BUILD_TESTS})

if(MINIREDIS_BUILD_SERVER AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
        "MiniRedis server currently requires Linux because it uses epoll/eventfd/POSIX sockets. "
        "Build the independent desktop client in tools/desktop with Node.js instead."
    )
endif()

if(MINIREDIS_BUILD_TESTS AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
        "MiniRedis tests currently exercise Linux-only server/persistence code. "
        "Use -DMINIREDIS_BUILD_TESTS=OFF on non-Linux platforms."
    )
endif()

if(MINIREDIS_ENABLE_ASAN AND MINIREDIS_ENABLE_TSAN)
    message(FATAL_ERROR "AddressSanitizer and ThreadSanitizer cannot be enabled together")
endif()

function(miniredis_apply_sanitizers target_name)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        return()
    endif()

    if(MINIREDIS_ENABLE_ASAN)
        target_compile_options(${target_name} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(${target_name} PRIVATE -fsanitize=address)
    endif()
    if(MINIREDIS_ENABLE_UBSAN)
        target_compile_options(${target_name} PRIVATE -fsanitize=undefined -fno-omit-frame-pointer)
        target_link_options(${target_name} PRIVATE -fsanitize=undefined)
    endif()
    if(MINIREDIS_ENABLE_TSAN)
        target_compile_options(${target_name} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(${target_name} PRIVATE -fsanitize=thread)
    endif()
endfunction()

if(MINIREDIS_BUILD_SERVER OR MINIREDIS_BUILD_TESTS)
    find_package(Threads REQUIRED)
endif()

if((MINIREDIS_BUILD_SERVER OR MINIREDIS_BUILD_TESTS) AND MINIREDIS_ENABLE_MYSQL_DISCOVERY)
    find_library(MYSQL_CLIENT_LIB mysqlclient)
    if(MYSQL_CLIENT_LIB)
        message(STATUS "Found mysqlclient: ${MYSQL_CLIENT_LIB}")
        set(HAVE_MYSQL ON)
    else()
        message(STATUS "mysqlclient not found, MySQL cluster discovery disabled")
        set(HAVE_MYSQL OFF)
    endif()
elseif(MINIREDIS_ENABLE_MYSQL_DISCOVERY)
    set(HAVE_MYSQL OFF)
else()
    message(STATUS "MySQL cluster discovery disabled by configuration")
    set(HAVE_MYSQL OFF)
endif()

if(MINIREDIS_BUILD_SERVER OR MINIREDIS_BUILD_TESTS)
    set(MINIREDIS_RUNTIME_SOURCES
        src/core/cache_store.cpp
        src/core/logger.cpp
        src/core/memory_pool.cpp
        src/core/thread_pool.cpp
        src/net/scheduler.cpp
        src/net/resp_parser.cpp
        src/persistence/persistence_manager.cpp
        src/persistence/append_only_file.cpp
        src/persistence/file_persistence.cpp
        src/cluster/slot_map.cpp
        src/cluster/cluster_config_store.cpp
        src/cluster/cluster_utils.cpp
        src/metrics/stats.cpp
        src/metrics/http_stats.cpp
        src/server/config.cpp
        src/server/command_handler.cpp
        src/server/failover_coordinator.cpp
        src/server/replication_dispatcher.cpp
        src/server/replication_state.cpp
        src/server/server.cpp
    )

    if(HAVE_MYSQL)
        list(APPEND MINIREDIS_RUNTIME_SOURCES src/cluster/mysql_client.cpp)
    endif()

    add_library(miniredis_runtime STATIC ${MINIREDIS_RUNTIME_SOURCES})
    target_include_directories(miniredis_runtime PUBLIC ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(miniredis_runtime PUBLIC Threads::Threads)
    miniredis_apply_sanitizers(miniredis_runtime)

    if(HAVE_MYSQL)
        target_link_libraries(miniredis_runtime PUBLIC ${MYSQL_CLIENT_LIB})
        target_compile_definitions(miniredis_runtime PUBLIC HAVE_MYSQL)
    endif()
endif()

if(MINIREDIS_BUILD_SERVER)
    add_executable(miniredis src/app/main.cpp)
    target_link_libraries(miniredis PRIVATE miniredis_runtime)
    miniredis_apply_sanitizers(miniredis)
endif()

if(MINIREDIS_BUILD_TESTS)
    enable_testing()

    add_executable(miniredis_unit_tests tests/unit_tests.cpp)
    target_link_libraries(miniredis_unit_tests PRIVATE miniredis_runtime)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(miniredis_unit_tests PRIVATE -UNDEBUG)
    endif()
    miniredis_apply_sanitizers(miniredis_unit_tests)
    add_test(NAME miniredis_unit_tests COMMAND miniredis_unit_tests)
endif()

if(MINIREDIS_BUILD_TESTS AND MINIREDIS_ENABLE_INTEGRATION_TESTS AND MINIREDIS_BUILD_SERVER)
    find_program(BASH_PROGRAM bash)
    find_program(REDIS_CLI_PROGRAM redis-cli)
    find_program(CURL_PROGRAM curl)
    find_program(PYTHON3_PROGRAM python3)
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

        add_test(
            NAME miniredis_failover_smoke
            COMMAND ${CMAKE_COMMAND} -E env
                    SERVER=$<TARGET_FILE:miniredis>
                    REDIS_CLI=${REDIS_CLI_PROGRAM}
                    ${BASH_PROGRAM} ${CMAKE_SOURCE_DIR}/scripts/failover_demo.sh smoke
        )
        set_tests_properties(miniredis_failover_smoke PROPERTIES
            TIMEOUT 60
            RESOURCE_LOCK miniredis_ports
        )

    else()
        message(STATUS "Skipping integration tests: bash, redis-cli, or curl not found")
    endif()

    if(PYTHON3_PROGRAM)
        add_test(
            NAME miniredis_protocol_limits
            COMMAND ${PYTHON3_PROGRAM}
                    ${CMAKE_SOURCE_DIR}/tests/integration/protocol_limits.py
                    $<TARGET_FILE:miniredis>
        )
        set_tests_properties(miniredis_protocol_limits PROPERTIES
            TIMEOUT 45
            RESOURCE_LOCK miniredis_ports
        )
    else()
        message(STATUS "Skipping raw protocol limits test: python3 not found")
    endif()
endif()
