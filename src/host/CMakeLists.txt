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
    d3d11.lib dxgi.lib xaudio2.lib xinput.lib
    # M6: CoTaskMemFree for the WMVideo Decoder DMO media types (video/wmv_decoder.cpp)
    ole32.lib
    # M6b: Media Foundation source reader for movie packs (video/movie_pack_reader.cpp; part of Windows)
    mfplat.lib mfreadwrite.lib mfuuid.lib)

# ---- gfx: HLSL shaders compiled at build time (M3, GFX_CONTRACT.md 6; owned by the shader agent) ----------
# fxc comes with the Windows SDK (vcvars32 puts its x86 bin directory on PATH). Each fut_add_shader call
# compiles one entry point into <build>/src/host/gen_shaders/<name>.h (`const BYTE g_<name>[]`, DXBC), which
# is a source of fut_host_obj, so it is generated before the C++ files that include it. The headers stay in
# the build tree (no checked-in generated file, nothing for tools/gen/gen_all.py); the runtime needs no
# d3dcompiler DLL. Shader model 4.0 (vs_4_0/ps_4_0) = D3D feature level 10_0 and higher.
#   fut_add_shader(<name> <file in gfx/shaders> <entry> <profile> [DEFINE=VALUE ...])
find_program(FUT_FXC NAMES fxc
    HINTS "$ENV{WindowsSdkVerBinPath}x86" "$ENV{WindowsSdkVerBinPath}x64"
          "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x86"
    REQUIRED)
set(FUT_SHADER_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen_shaders)
file(MAKE_DIRECTORY ${FUT_SHADER_GEN_DIR})
file(GLOB FUT_SHADER_DEPS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/gfx/shaders/*.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/gfx/shaders/*.hlsli
    ${CMAKE_CURRENT_SOURCE_DIR}/gfx/shaders/*.h)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(FUT_FXC_CONFIG_FLAGS /Od /Zi)
else()
    set(FUT_FXC_CONFIG_FLAGS /O3 /Qstrip_debug /Qstrip_reflect)
endif()
function(fut_add_shader name file entry profile)
    set(out ${FUT_SHADER_GEN_DIR}/${name}.h)
    set(defines "")
    foreach(define IN LISTS ARGN)
        list(APPEND defines /D${define})
    endforeach()
    add_custom_command(OUTPUT ${out}
        COMMAND ${FUT_FXC} /nologo /WX /T ${profile} /E ${entry} /Vn g_${name} ${FUT_FXC_CONFIG_FLAGS} ${defines}
                /Fh ${out} ${CMAKE_CURRENT_SOURCE_DIR}/gfx/shaders/${file}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/gfx/shaders/${file} ${FUT_SHADER_DEPS}
        COMMENT "fxc gfx/shaders/${file} ${entry} (${profile}) -> gen_shaders/${name}.h"
        VERBATIM)
    target_sources(fut_host_obj PRIVATE ${out})
endfunction()
target_include_directories(fut_host_obj PRIVATE ${FUT_SHADER_GEN_DIR})
# Backend agent (M3 phase 2, gfx/rhi/d3d11/**): built-in shaders of the D3D11 backend (rectangle
# depth/stencil clear, diagnostic shaders of the headless RHI tests, the present pass of M3 phase 5);
# `file` is relative to gfx/shaders.
fut_add_shader(d3d11_clear_vs ../rhi/d3d11/d3d11_builtin.hlsl ClearVS vs_4_0)
fut_add_shader(d3d11_diag_vs ../rhi/d3d11/d3d11_builtin.hlsl DiagVS vs_4_0)
fut_add_shader(d3d11_diag_color_ps ../rhi/d3d11/d3d11_builtin.hlsl DiagColorPS ps_4_0)
fut_add_shader(d3d11_diag_texture_ps ../rhi/d3d11/d3d11_builtin.hlsl DiagTexturePS ps_4_0)
fut_add_shader(d3d11_diag_tint_ps ../rhi/d3d11/d3d11_builtin.hlsl DiagTintPS ps_4_0)
fut_add_shader(d3d11_present_vs ../rhi/d3d11/d3d11_builtin.hlsl PresentVS vs_4_0)
fut_add_shader(d3d11_present_ps ../rhi/d3d11/d3d11_builtin.hlsl PresentPS ps_4_0)
# Renderer agent (M3c): box / Mitchell-Netravali downsample (render_scale, image larger than the window).
fut_add_shader(d3d11_resample_ps ../rhi/d3d11/d3d11_builtin.hlsl ResamplePS ps_4_0)
# Phase-1 stubs, kept for the RHI tests (gfx::ff::StubShaderBytecode); moved to ff_stub.hlsl in phase 2.
fut_add_shader(ff_stub_vs ff_stub.hlsl VSMain vs_4_0)
fut_add_shader(ff_stub_ps ff_stub.hlsl PSMain ps_4_0)
# Fixed-function ubershader permutations (FF_FEATURE_VS/PS_COMPILE_MASK in gfx/shaders/ff_shared.h; selected
# by gfx::ff::VertexShaderBytecode / PixelShaderBytecode in gfx/ff_pipeline.cpp).
fut_add_shader(ff_vs_pretransformed ff_ubershader.hlsl VSMain vs_4_0 FF_PERM_PRETRANSFORMED=1)
fut_add_shader(ff_vs_transformed ff_ubershader.hlsl VSMain vs_4_0 FF_PERM_LIGHTING=0)
fut_add_shader(ff_vs_lit ff_ubershader.hlsl VSMain vs_4_0 FF_PERM_LIGHTING=1)
fut_add_shader(ff_ps_smooth ff_ubershader.hlsl PSMain ps_4_0 FF_PERM_FLAT=0)
fut_add_shader(ff_ps_flat ff_ubershader.hlsl PSMain ps_4_0 FF_PERM_FLAT=1)

add_library(fut_host SHARED ${CMAKE_CURRENT_SOURCE_DIR}/futurama_host.def)
set_target_properties(fut_host PROPERTIES OUTPUT_NAME futurama_host LINKER_LANGUAGE CXX)
target_link_libraries(fut_host PRIVATE fut_host_obj "$<LINK_LIBRARY:WHOLE_ARCHIVE,fut_game>")
# /SAFESEH:NO: MASM objects carry no SafeSEH table (design 5.2).
target_link_options(fut_host PRIVATE /SAFESEH:NO)
