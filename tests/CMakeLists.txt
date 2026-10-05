# fut_tests: host unit tests (design 5.1/5.2: doctest, run without the XBE).
# Built only if third_party/doctest/doctest.h exists and tests/*.cpp are present. Test files are
# linked against all host objects (fut_host_obj), so internal host APIs can be tested directly.
# One test file must contain:  #define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN  before including doctest.h.
set(FUT_DOCTEST_DIR ${CMAKE_SOURCE_DIR}/third_party/doctest)
file(GLOB FUT_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
if(EXISTS ${FUT_DOCTEST_DIR}/doctest.h AND FUT_TEST_SOURCES)
    add_executable(fut_tests ${FUT_TEST_SOURCES})
    fut_apply_host_options(fut_tests)
    target_include_directories(fut_tests PRIVATE ${FUT_DOCTEST_DIR})
    target_link_libraries(fut_tests PRIVATE fut_host_obj "$<LINK_LIBRARY:WHOLE_ARCHIVE,fut_game>")
    target_link_options(fut_tests PRIVATE /SAFESEH:NO)
    # enable_testing() is in the top-level CMakeLists.txt (ctest from the build root).
    add_test(NAME fut_tests COMMAND fut_tests)
endif()
