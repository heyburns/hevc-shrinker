cmake_minimum_required(VERSION 3.16)
project(hevc_shrinker LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Sql)

add_executable(hevc_shrinker
    main.cpp
    databasemanager.h
    databasemanager.cpp
    transcodeworker.h
    transcodeworker.cpp
    mainwindow.h
    mainwindow.cpp
)

if(WIN32)
    target_sources(hevc_shrinker PRIVATE resources.rc)
endif()

target_link_libraries(hevc_shrinker PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Sql
)

# On Windows, copy standard runtime libs or set properties
if(WIN32)
    set_target_properties(hevc_shrinker PROPERTIES
        WIN32_EXECUTABLE ON
    )
endif()

# Copy app icon to the output binary directory
add_custom_command(TARGET hevc_shrinker POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
    "${CMAKE_CURRENT_SOURCE_DIR}/app_icon.png"
    "$<TARGET_FILE_DIR:hevc_shrinker>/app_icon.png"
)
