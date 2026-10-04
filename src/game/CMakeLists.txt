# fut_game: decompiled game code (design 3.4, 5.2), linked into fut_host with WHOLE_ARCHIVE so that
# statically registered hooks (FUT_HOOK) are never dropped by the linker.
# New files are picked up automatically (GLOB CONFIGURE_DEPENDS), no CMake edits needed.
file(GLOB_RECURSE FUT_GAME_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
add_library(fut_game STATIC ${FUT_GAME_SOURCES})
target_link_libraries(fut_game PUBLIC fut_shared)
target_include_directories(fut_game PRIVATE ${FUT_SRC_DIR}/host)
fut_apply_host_options(fut_game)
