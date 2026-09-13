/* SPDX-FileCopyrightText: 2025 */
/* SPDX-License-Identifier: MIT */

#include "chapter_cache.h"

void chapter_cache_window(int total, int current, int *start, int *end)
{
    if (!start || !end) {
        return;
    }
    if (total <= 0) {
        *start = 0;
        *end = 0;
        return;
    }
    if (current < 1) {
        current = 1;
    } else if (current > total) {
        current = total;
    }

    int first = current - CHAPTER_CACHE_SIZE / 2;
    int last_first = total - CHAPTER_CACHE_SIZE + 1;
    if (first < 1 || last_first < 1) {
        first = 1;
    } else if (first > last_first) {
        first = last_first;
    }

    int last = first + CHAPTER_CACHE_SIZE - 1;
    if (last > total) {
        last = total;
    }
    *start = first;
    *end = last;
}
