cmake_minimum_required(VERSION 3.16)
project(hevc_shrinker LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Sql)

add_library(hevc_core STATIC
    videoencoder.h
    videoencoder.cpp
    avisynthpipeline.h
    avisynthpipeline.cpp
    databasemanager.h
    databasemanager.cpp
    filetransaction.h
    filetransaction.cpp
    mediautils.h
    mediautils.cpp
    scanworker.h
    scanworker.cpp
    transcodeworker.h
    transcodeworker.cpp
)
target_include_directories(hevc_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hevc_core PUBLIC Qt6::Core Qt6::Sql)

add_executable(hevc_shrinker
    main.cpp
    mainwindow.h
    mainwindow.cpp
    avisynthdialog.h
    avisynthdialog.cpp
    resources.qrc
)

if(WIN32)
    target_sources(hevc_shrinker PRIVATE resources.rc)
endif()

target_link_libraries(hevc_shrinker PRIVATE
    hevc_core
    Qt6::Core
    Qt6::Widgets
    Qt6::Sql
)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_executable(process_fixture tests/process_fixture.cpp)
    target_link_libraries(process_fixture PRIVATE Qt6::Core)
    add_executable(regression_tests tests/regression_tests.cpp mainwindow.cpp mainwindow.h avisynthdialog.cpp avisynthdialog.h resources.qrc)
    target_link_libraries(regression_tests PRIVATE hevc_core Qt6::Widgets Qt6::Test)
    add_dependencies(regression_tests process_fixture)
    add_test(NAME regressions COMMAND regression_tests)
    set_tests_properties(regressions PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 180)
endif()

# On Windows, copy standard runtime libs or set properties
if(WIN32)
    set_target_properties(hevc_shrinker PROPERTIES
        WIN32_EXECUTABLE ON
    )
endif()

# On macOS, build as a native App Bundle so it can receive GUI focus and show in the dock
if(APPLE)
    set_target_properties(hevc_shrinker PROPERTIES
        MACOSX_BUNDLE ON
    )
endif()

# Copy app icon to the output binary directory
add_custom_command(TARGET hevc_shrinker POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
    "${CMAKE_CURRENT_SOURCE_DIR}/app_icon.png"
    "$<TARGET_FILE_DIR:hevc_shrinker>/app_icon.png"
)
