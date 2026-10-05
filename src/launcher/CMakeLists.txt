# fut_launcher -> futurama.exe (design 1.3, 5.2). No CRT, fixed base 0x10000, MASM placeholder first.
# futurama.manifest (longPathAware) is merged into the default manifest by CMake.
add_executable(fut_launcher placeholder.asm launcher.c futurama.manifest)
set_target_properties(fut_launcher PROPERTIES
    OUTPUT_NAME futurama
    LINKER_LANGUAGE C)
target_include_directories(fut_launcher PRIVATE ${FUT_SRC_DIR})
target_compile_definitions(fut_launcher PRIVATE WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
# No CRT: no buffer security checks (needs __security_cookie), no runtime checks (needs _RTC_*).
target_compile_options(fut_launcher PRIVATE
    $<$<COMPILE_LANGUAGE:C>:/W4 /GS- /Gs1000000 /utf-8>)
# Exact link options of design 5.2 on the linker command line (LNK4229: #pragma ignores /FIXED etc.).
target_link_options(fut_launcher PRIVATE
    /BASE:0x10000 /FIXED /DYNAMICBASE:NO /LARGEADDRESSAWARE /SAFESEH:NO /INCREMENTAL:NO
    /NODEFAULTLIB /ENTRY:LauncherEntry /SUBSYSTEM:WINDOWS)
target_link_libraries(fut_launcher PRIVATE kernel32.lib user32.lib)
# The host DLL must sit next to the exe (LoadLibraryExW(<exe dir>\futurama_host.dll)).
add_dependencies(fut_launcher fut_host)
