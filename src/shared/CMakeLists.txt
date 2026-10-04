# fut_shared: header-only guest structures shared by launcher, host and game code (design 5.2).
add_library(fut_shared INTERFACE)
target_include_directories(fut_shared INTERFACE ${FUT_SRC_DIR})
