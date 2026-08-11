cmake_minimum_required(VERSION 3.16)

project(gar_stream_rx_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(gar_stream_rx_core STATIC
    src/receiver_state.cpp
    src/ili9341.cpp
    src/ky040.cpp
    src/linux_gpio.cpp
    src/source_browser.cpp
    src/source_protocol.cpp
)
target_include_directories(gar_stream_rx_core PUBLIC include)
target_compile_options(gar_stream_rx_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
find_package(Threads REQUIRED)
target_link_libraries(gar_stream_rx_core PUBLIC Threads::Threads)

option(GAR_RX_BUILD_APP "Build the native GStreamer/Linux device application" ON)
if(GAR_RX_BUILD_APP)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(
        GSTREAMER
        REQUIRED
        IMPORTED_TARGET
        gstreamer-1.0
        gstreamer-app-1.0
        glib-2.0
    )
    add_executable(gar-stream-rx src/main.cpp)
    target_link_libraries(gar-stream-rx PRIVATE gar_stream_rx_core PkgConfig::GSTREAMER)
    target_compile_options(gar-stream-rx PRIVATE -Wall -Wextra -Wpedantic -Werror)
    install(TARGETS gar-stream-rx RUNTIME DESTINATION bin)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(test_receiver_state tests/test_receiver_state.cpp)
    target_link_libraries(test_receiver_state PRIVATE gar_stream_rx_core)
    target_compile_options(test_receiver_state PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME receiver_state COMMAND test_receiver_state)

    add_executable(test_ky040 tests/test_ky040.cpp)
    target_link_libraries(test_ky040 PRIVATE gar_stream_rx_core)
    target_compile_options(test_ky040 PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME ky040 COMMAND test_ky040)

    add_executable(test_source_protocol tests/test_source_protocol.cpp)
    target_link_libraries(test_source_protocol PRIVATE gar_stream_rx_core)
    target_compile_options(test_source_protocol PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME source_protocol COMMAND test_source_protocol)

    add_executable(test_source_browser tests/test_source_browser.cpp)
    target_link_libraries(test_source_browser PRIVATE gar_stream_rx_core)
    target_compile_options(test_source_browser PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME source_browser COMMAND test_source_browser)
endif()
