cmake_minimum_required(VERSION 3.24)
project(obs-dock-layout VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

add_library(obs-dock-layout MODULE
    src/plugin-main.cpp
)

target_link_libraries(obs-dock-layout PRIVATE
    OBS::libobs
    OBS::obs-frontend-api
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

target_compile_definitions(obs-dock-layout PRIVATE
    PLUGIN_VERSION="${PROJECT_VERSION}"
)

set_target_properties(obs-dock-layout PROPERTIES
    PREFIX ""
    OUTPUT_NAME "obs-dock-layout"
)

install(TARGETS obs-dock-layout
    LIBRARY DESTINATION obs-plugins/64bit
    RUNTIME DESTINATION obs-plugins/64bit
)
