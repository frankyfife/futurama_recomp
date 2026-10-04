# Futurama PC port - loader (docs/loader_design.md, section 5).
# Build: build.cmd [builddir] [Debug|RelWithDebInfo] [target]   (MSVC x86 + Ninja)
cmake_minimum_required(VERSION 3.28)
project(FuturamaPC LANGUAGES C CXX ASM_MASM)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "The loader must be built for 32-bit x86 (use vcvars32 / the x86 presets).")
endif()
if(NOT MSVC)
    message(FATAL_ERROR "MSVC (cl.exe) or clang-cl is required.")
endif()

option(FUT_WERROR "Treat compiler warnings as errors (CI)" OFF)

set(FUT_SRC_DIR ${CMAKE_SOURCE_DIR}/src)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_PDB_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static CRT for the host (no VC++ redistributable needed); the launcher has no CRT at all.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# Debug info embedded in the objects (/Z7): no PDB contention with parallel Ninja jobs.
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")

# No incremental linking anywhere: it inserts jump thunks/padding (design 1.3 item 1) and would make
# host function addresses (thunk slots, hook targets) point at ILT stubs.
foreach(kind EXE SHARED MODULE)
    foreach(cfg DEBUG RELWITHDEBINFO RELEASE MINSIZEREL)
        string(REGEX REPLACE "/INCREMENTAL(:NO)?" "" CMAKE_${kind}_LINKER_FLAGS_${cfg} "${CMAKE_${kind}_LINKER_FLAGS_${cfg}}")
        string(APPEND CMAKE_${kind}_LINKER_FLAGS_${cfg} " /INCREMENTAL:NO")
    endforeach()
endforeach()
# C is only used by the CRT-less launcher: no runtime checks (/RTC1 needs the CRT), no default libs.
string(REGEX REPLACE "/RTC[1csu]+" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
set(CMAKE_C_STANDARD_LIBRARIES "")

# Compiler options of all host-side C++ targets (design 5.2/5.3): C++20, /W4 /permissive- /utf-8 /EHsc.
# shared/win32.h is force-included so that every TU sees the same <windows.h>/<ntstatus.h> setup.
function(fut_apply_host_options target)
    target_compile_options(${target} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus /Zc:inline /FI${FUT_SRC_DIR}/shared/win32.h>
        $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<BOOL:${FUT_WERROR}>>:/WX>)
    target_compile_definitions(${target} PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
    target_include_directories(${target} PRIVATE ${FUT_SRC_DIR} ${FUT_SRC_DIR}/host)
endfunction()

add_subdirectory(src/shared)
add_subdirectory(src/game)
add_subdirectory(src/host)
add_subdirectory(src/launcher)
add_subdirectory(tests)

# fut_gen: verify that the checked-in generated files are current (needs Python; not part of ALL).
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    add_custom_target(fut_gen
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen/gen_all.py --check
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tools/gen
        COMMENT "Checking generated files (tools/gen/*.py --check)"
        VERBATIM)
endif()
