// fut_tests entry point (doctest). This is the ONLY test file that defines
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN; every other tests\*.cpp just includes "doctest.h".
//
// Run:  build\<dir>\bin\fut_tests.exe            (all tests)
//       build\<dir>\bin\fut_tests.exe -tc="xdvdfs*"   (filter, see doctest --help)
// Tests that need the user's disc image read its path from the environment variable FUT_TEST_ISO
// (default: see tests\vfs_tests.cpp) and are skipped with a message when the file does not exist.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
