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
# clang-cl (second build for warnings, build.cmd ... clang): /Oy- keeps frame pointers everywhere,
# because KeBugCheck reads the guest caller's EBP next to its return address and clang has no
# equivalent of MSVC's #pragma optimize("y", off).
function(fut_apply_host_options target)
    target_compile_options(${target} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus /Zc:inline /FI${FUT_SRC_DIR}/shared/win32.h>
        $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:Clang>>:/Oy->
        $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<BOOL:${FUT_WERROR}>>:/WX>)
    target_compile_definitions(${target} PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
    target_include_directories(${target} PRIVATE ${FUT_SRC_DIR} ${FUT_SRC_DIR}/host)
endfunction()

# ctest from the build root (review finding 10). Tests: fut_tests (unit tests, tests/CMakeLists.txt)
# and, when FUT_TEST_CONFIG names a test INI with the user's ISO and a userdata directory under run/,
# fut_selftest (design 5.2: futurama.exe --selftest --stop-at=main) and fut_m1_check (tools/m1_check.py,
# all six M1 exit criteria over 10 runs). FUT_M2_TEST_CONFIG (an INI without stop_at, e.g.
# run/m2/futurama.ini) adds fut_m2_check (tools/m2_check.py, all seven M2 exit criteria over 3 runs of
# 3000 frames, about 65 s each). FUT_M2_EXTRA_TEST_CONFIGS adds one-run variants (e.g. english).
enable_testing()
set(FUT_TEST_CONFIG "" CACHE FILEPATH "Test INI for fut_selftest/fut_m1_check ([paths] iso + userdata under run/); empty = not registered")
set(FUT_M2_TEST_CONFIG "" CACHE FILEPATH "Test INI for fut_m2_check (no stop_at; [paths] iso + userdata under run/); empty = not registered")

add_subdirectory(src/shared)
add_subdirectory(src/game)
add_subdirectory(src/host)
add_subdirectory(src/launcher)
add_subdirectory(tests)

find_package(Python3 COMPONENTS Interpreter QUIET)

# fut_selftest / fut_m1_check (see enable_testing above).
if(FUT_TEST_CONFIG)
    add_test(NAME fut_selftest
        COMMAND fut_launcher --config=${FUT_TEST_CONFIG} --selftest --stop-at=main)
    set_tests_properties(fut_selftest PROPERTIES TIMEOUT 120)
    if(Python3_FOUND)
        # Optional local copy of the original XBE (never in the repo) for the TitleImage comparison.
        set(fut_m1_xbe_args "")
        if(EXISTS ${CMAKE_SOURCE_DIR}/orig/retail_eu/default.xbe)
            set(fut_m1_xbe_args --xbe ${CMAKE_SOURCE_DIR}/orig/retail_eu/default.xbe)
        endif()
        add_test(NAME fut_m1_check
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/m1_check.py
                    --exe $<TARGET_FILE:fut_launcher> --config ${FUT_TEST_CONFIG} --runs 10 ${fut_m1_xbe_args})
        set_tests_properties(fut_m1_check PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE)
        # Both tests write into the same userdata directory.
        set_tests_properties(fut_selftest PROPERTIES RUN_SERIAL TRUE)
    endif()
endif()

# fut_m2_check (M2 exit criteria, design section 6): runs past main until the frame limit.
if(FUT_M2_TEST_CONFIG AND Python3_FOUND)
    add_test(NAME fut_m2_check
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/m2_check.py
                --exe $<TARGET_FILE:fut_launcher> --config ${FUT_M2_TEST_CONFIG} --runs 3 --fresh)
    set_tests_properties(fut_m2_check PROPERTIES TIMEOUT 900 RUN_SERIAL TRUE)
endif()
# Variants of the M2 check (M2 review finding 3), e.g. english / no [settings] language: one test
# fut_m2_check_<name of the INI's directory> per entry, 1 run each.
set(FUT_M2_EXTRA_TEST_CONFIGS "" CACHE STRING "More test INIs for fut_m2_check_<dir> (list; 1 run each); empty = none")
if(Python3_FOUND)
    foreach(fut_m2_cfg IN LISTS FUT_M2_EXTRA_TEST_CONFIGS)
        get_filename_component(fut_m2_dir "${fut_m2_cfg}" DIRECTORY)
        get_filename_component(fut_m2_name "${fut_m2_dir}" NAME)
        add_test(NAME fut_m2_check_${fut_m2_name}
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/m2_check.py
                    --exe $<TARGET_FILE:fut_launcher> --config ${fut_m2_cfg} --runs 1 --fresh)
        set_tests_properties(fut_m2_check_${fut_m2_name} PROPERTIES TIMEOUT 400 RUN_SERIAL TRUE)
    endforeach()
endif()

# fut_gen: verify that the checked-in generated files are current (needs Python; not part of ALL).
if(Python3_FOUND)
    add_custom_target(fut_gen
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen/gen_all.py --check
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tools/gen
        COMMENT "Checking generated files (tools/gen/*.py --check)"
        VERBATIM)
endif()
