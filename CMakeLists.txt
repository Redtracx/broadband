cmake_minimum_required(VERSION 3.19)

project(broadband
    VERSION 1.0.0
    DESCRIPTION "Simple bandwidth and internet speed test with a Qt 6 GUI"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(BROADBAND_BUILD_TESTS "Build the unit tests" ON)

find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets Network)

function(broadband_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# Measurement engines, LAN test server and Excel report, shared by the app and the tests.
add_library(broadband_core STATIC
    src/Format.h
    src/HttpCommon.cpp
    src/HttpCommon.h
    src/LoadTest.cpp
    src/LoadTest.h
    src/Report.cpp
    src/Report.h
    src/SpeedTest.cpp
    src/SpeedTest.h
    src/Statistics.h
    src/TestServer.cpp
    src/TestServer.h
    src/XlsxWriter.cpp
    src/XlsxWriter.h
)
target_include_directories(broadband_core PUBLIC src)
target_compile_definitions(broadband_core PUBLIC BROADBAND_VERSION="${PROJECT_VERSION}")
target_link_libraries(broadband_core PUBLIC Qt6::Network)
broadband_set_warnings(broadband_core)

qt_add_executable(broadband
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/ThroughputChart.cpp
    src/ThroughputChart.h
)
target_link_libraries(broadband PRIVATE broadband_core Qt6::Widgets)
broadband_set_warnings(broadband)

set_target_properties(broadband PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)

if(WIN32)
    # Version information shown in the file properties on Windows
    enable_language(RC)
    configure_file(src/broadband.rc.in ${CMAKE_CURRENT_BINARY_DIR}/broadband.rc @ONLY)
    target_sources(broadband PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/broadband.rc)
endif()

if(BROADBAND_BUILD_TESTS)
    find_package(Qt6 6.2 QUIET COMPONENTS Test)
    if(Qt6Test_FOUND)
        enable_testing()
        add_subdirectory(tests)
    else()
        message(STATUS "Qt6::Test not found - skipping tests")
    endif()
endif()

include(GNUInstallDirs)
install(TARGETS broadband
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
