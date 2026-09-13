/* SPDX-FileCopyrightText: 2025 */
/* SPDX-License-Identifier: MIT */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define CHAPTER_CACHE_SIZE 5

/* Computes the inclusive five-chapter cache window around current. */
void chapter_cache_window(int total, int current, int *start, int *end);

#ifdef __cplusplus
}
#endif
