/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Accepts arbitrarily split GBK byte chunks and writes UTF-8. */
bool gbk_write_utf8(FILE *file, uint8_t *pending_lead,
                    const uint8_t *data, size_t len);
bool gbk_write_utf8_finish(FILE *file, uint8_t *pending_lead);

/* UTF-8 probe used before choosing whether a TXT needs conversion. */
bool text_looks_utf8(const uint8_t *data, size_t len);
