#include <assert.h>
#include <stdio.h>

#include "chapter_cache.h"

static void expect_window(int total, int current, int expected_start,
                          int expected_end)
{
    int start = -1;
    int end = -1;
    chapter_cache_window(total, current, &start, &end);
    assert(start == expected_start);
    assert(end == expected_end);
}

int main(void)
{
    expect_window(20, 1, 1, 5);
    expect_window(20, 2, 1, 5);
    expect_window(20, 3, 1, 5);
    expect_window(20, 4, 2, 6);
    expect_window(20, 5, 3, 7);
    expect_window(20, 19, 16, 20);
    expect_window(20, 20, 16, 20);
    expect_window(3, 2, 1, 3);
    expect_window(0, 1, 0, 0);
    puts("CHAPTER CACHE SELFTEST PASSED");
    return 0;
}
