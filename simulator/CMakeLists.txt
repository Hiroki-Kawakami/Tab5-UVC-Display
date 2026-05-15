cmake_minimum_required(VERSION 3.16)
include(FetchContent)

set(CMAKE_C_COMPILER gcc)
set(CMAKE_CXX_COMPILER g++)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_COMPILER_LAUNCHER ccache)
set(CMAKE_CXX_COMPILER_LAUNCHER ccache)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

project(simulator C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Load Dependencies
find_package(SDL2 REQUIRED)
find_package(JPEG REQUIRED)

# Load LVGL
FetchContent_Declare(
    lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG        7d376cc5c4301f6dd1789a8bf3b1e56cdc44106a
)
FetchContent_MakeAvailable(lvgl)

# Import components/* and an app directory into a target
# Usage: target_add_components(TARGET APP_DIR)
function(target_add_components TARGET APP_DIR)
    file(GLOB COMPONENTS LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/../components/*")
    list(APPEND COMPONENTS ${APP_DIR})
    message(STATUS "COMPONENTS=${COMPONENTS}")

    foreach(COMPONENT ${COMPONENTS})
        if(IS_DIRECTORY ${COMPONENT})
            file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS ${COMPONENT}/*.c ${COMPONENT}/*.cpp)
            target_sources(${TARGET} PRIVATE ${SOURCES})
            target_include_directories(${TARGET} PRIVATE ${COMPONENT})
        endif()
    endforeach()
endfunction()

add_executable(simulator)
file(GLOB_RECURSE DEVICE_SOURCES CONFIGURE_DEPENDS src/*.c src/*.cpp)
target_compile_definitions(simulator PRIVATE SDL_DISABLE_ARM_NEON_H)
target_sources(simulator PRIVATE ${DEVICE_SOURCES})
target_include_directories(simulator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(simulator SDL2::SDL2 lvgl JPEG::JPEG)
target_add_components(simulator ${CMAKE_CURRENT_SOURCE_DIR}/../app)
