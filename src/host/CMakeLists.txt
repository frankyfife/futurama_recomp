# fut_host -> futurama_host.dll (design 5.1/5.2), exports HostMain (futurama_host.def).
#
# Sources are collected per module directory with GLOB_RECURSE CONFIGURE_DEPENDS: adding or removing
# files in these directories needs NO CMake edit (Ninja re-runs the configure step automatically).
# All objects go into the OBJECT library fut_host_obj so that statically registered kernel exports,
# hooks and self-tests are always linked (a static library would drop unreferenced registrations).
set(FUT_HOST_MODULES core kernel vfs xapi platform asm gen gfx audio video input)
set(FUT_HOST_SOURCES "")
foreach(module IN LISTS FUT_HOST_MODULES)
    file(GLOB_RECURSE module_sources CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/${module}/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/${module}/*.h
        ${CMAKE_CURRENT_SOURCE_DIR}/${module}/*.inc
        ${CMAKE_CURRENT_SOURCE_DIR}/${module}/*.asm)
    list(APPEND FUT_HOST_SOURCES ${module_sources})
endforeach()

add_library(fut_host_obj OBJECT ${FUT_HOST_SOURCES})
fut_apply_host_options(fut_host_obj)
target_link_libraries(fut_host_obj PUBLIC
    fut_shared
    ntdll.lib dbghelp.lib shell32.lib user32.lib advapi32.lib
    # design 5.2 (used from M3/M4/M5 on; unused import libraries add no runtime dependency)
    d3d11.lib dxgi.lib xaudio2.lib xinput.lib)

add_library(fut_host SHARED ${CMAKE_CURRENT_SOURCE_DIR}/futurama_host.def)
set_target_properties(fut_host PROPERTIES OUTPUT_NAME futurama_host LINKER_LANGUAGE CXX)
target_link_libraries(fut_host PRIVATE fut_host_obj "$<LINK_LIBRARY:WHOLE_ARCHIVE,fut_game>")
# /SAFESEH:NO: MASM objects carry no SafeSEH table (design 5.2).
target_link_options(fut_host PRIVATE /SAFESEH:NO)
