cmake_minimum_required(VERSION 3.30)
project(snow_toolbox_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.12 REQUIRED COMPONENTS Core Widgets Network Concurrent)
set(toolbox_dir "${CMAKE_CURRENT_SOURCE_DIR}/../../src/toolbox")
add_library(toolbox-compile OBJECT
    "${toolbox_dir}/toolboxcontroller.cpp" "${toolbox_dir}/toolboxcontroller.h")
target_include_directories(toolbox-compile PRIVATE ../../include)
target_link_libraries(toolbox-compile PRIVATE Qt6::Widgets Qt6::Network Qt6::Concurrent)
add_library(translation-patch-compile OBJECT ../../src/app/translationservice.cpp)
target_include_directories(translation-patch-compile PRIVATE ../../include)
target_link_libraries(translation-patch-compile PRIVATE Qt6::Widgets Qt6::Network)
add_executable(snow-toolbox-core-tests toolbox_core_tests.cpp
    "${toolbox_dir}/quotaservice.cpp" "${toolbox_dir}/quotaservice.h"
    "${toolbox_dir}/memoryservice.cpp" "${toolbox_dir}/memoryservice.h"
    "${toolbox_dir}/offlinetranslator.cpp" "${toolbox_dir}/offlinetranslator.h")
target_include_directories(snow-toolbox-core-tests PRIVATE "${toolbox_dir}" ../../include)
target_link_libraries(snow-toolbox-core-tests PRIVATE Qt6::Core Qt6::Widgets Qt6::Concurrent psapi shell32 advapi32)
if(MSVC)
    add_compile_options(/utf-8)
    target_compile_options(snow-toolbox-core-tests PRIVATE /utf-8 /W4 /WX)
    target_compile_options(toolbox-compile PRIVATE /utf-8 /W4 /WX)
endif()
enable_testing()
add_test(NAME snow-toolbox-core-tests COMMAND snow-toolbox-core-tests)
add_executable(snow-toolbox-ui-render toolbox_ui_render.cpp
    "${toolbox_dir}/toolboxcontroller.cpp" "${toolbox_dir}/toolboxcontroller.h"
    "${toolbox_dir}/quotaservice.cpp" "${toolbox_dir}/quotaservice.h"
    "${toolbox_dir}/memoryservice.cpp" "${toolbox_dir}/memoryservice.h"
    "${toolbox_dir}/offlinetranslator.cpp" "${toolbox_dir}/offlinetranslator.h")
target_compile_definitions(snow-toolbox-ui-render PRIVATE SNOW_TOOLBOX_TEST_NO_STORAGE)
target_include_directories(snow-toolbox-ui-render PRIVATE "${toolbox_dir}")
target_link_libraries(snow-toolbox-ui-render PRIVATE Qt6::Widgets Qt6::Concurrent psapi shell32 advapi32)
qt_add_resources(snow-toolbox-ui-render toolbox_test_icons PREFIX "/toolbox"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/../../resources/toolbox"
    FILES ../../resources/toolbox/reset.svg ../../resources/toolbox/dragon/avatar.png
          ../../resources/toolbox/dragon/app.ico)
add_test(NAME snow-toolbox-ui-state-tests COMMAND snow-toolbox-ui-render --verify)
set_tests_properties(snow-toolbox-core-tests snow-toolbox-ui-state-tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 15)

add_executable(snow-toolbox-feedback-tests toolbox_feedback_tests.cpp)
target_include_directories(snow-toolbox-feedback-tests PRIVATE "${toolbox_dir}")
target_link_libraries(snow-toolbox-feedback-tests PRIVATE Qt6::Widgets)
add_test(NAME snow-toolbox-feedback-tests COMMAND snow-toolbox-feedback-tests)
set_tests_properties(snow-toolbox-feedback-tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 10)
