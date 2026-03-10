cmake_minimum_required(VERSION 3.25)

project(RuneArena LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
set(PLATFORMFOLDERS_BUILD_TESTING OFF CACHE BOOL "" FORCE)

add_subdirectory(external/raylib)
add_subdirectory(external/enet)
add_subdirectory(external/PlatformFolders)

file(GLOB_RECURSE RUNE_ARENA_SOURCES CONFIGURE_DEPENDS
    src/*.cpp
)

add_executable(rune_arena ${RUNE_ARENA_SOURCES})

target_include_directories(rune_arena PRIVATE
    src
    external/raygui/src
    external/json/single_include
    external/enet/include
)

target_link_libraries(rune_arena PRIVATE
    raylib
    enet
    platform_folders
)

if(WIN32)
    target_link_libraries(rune_arena PRIVATE ws2_32)
    target_compile_definitions(rune_arena PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

add_custom_command(TARGET rune_arena POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/assets
            $<TARGET_FILE_DIR:rune_arena>/assets
)
