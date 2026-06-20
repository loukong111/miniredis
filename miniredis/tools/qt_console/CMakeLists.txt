cmake_minimum_required(VERSION 3.20)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)

set(CMAKE_AUTOMOC ON)

add_executable(miniredis_qt_console
    main.cpp
    monitor_window.hpp
    monitor_window.cpp
    resp_client.hpp
    resp_client.cpp
    stats_client.hpp
    stats_client.cpp
)

target_link_libraries(miniredis_qt_console
    PRIVATE
        Qt6::Core
        Qt6::Widgets
        Qt6::Network
)

target_compile_features(miniredis_qt_console PRIVATE cxx_std_20)
