#include "common/test.h"

#include <tbb/concurrent_once_flag.h>

//! \file test_concurrent_once_flag.cpp
//! \brief Test for concurrent_once_flag

//-----------------------------------------------------------------------------
// Concurrent concurrent_once_flag Tests
//-----------------------------------------------------------------------------

//! \brief \ref error_guessing
TEST_CASE("concurrent_once_flag simple") {
    tbb::concurrent_once_flag flag;
    std::atomic<int> count{0};
    for (int i = 0; i != 10; ++i) {
        call_once(flag, [&count]() { ++count; });
        CHECK(count == 1);
    }
}
