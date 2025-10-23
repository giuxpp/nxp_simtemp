cmake_minimum_required(VERSION 3.16)

project(simtemp_gui LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(QT_COMPONENTS Widgets Charts)
find_package(QT NAMES Qt6 Qt5 COMPONENTS ${QT_COMPONENTS} REQUIRED)
find_package(Qt${QT_VERSION_MAJOR} COMPONENTS ${QT_COMPONENTS} REQUIRED)

add_executable(simtemp_gui
    main.cpp
)

target_link_libraries(simtemp_gui
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Widgets
        Qt${QT_VERSION_MAJOR}::Charts
)

if(QT_VERSION_MAJOR EQUAL 5)
    target_compile_definitions(simtemp_gui PRIVATE USING_QT5)
endif()
