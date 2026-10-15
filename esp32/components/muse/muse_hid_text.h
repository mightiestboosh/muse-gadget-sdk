/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
/* US keyboard, single-key boot-compatible report. Never emits Enter. */
bool muse_hid_ascii(uint8_t ch, uint8_t report[8]);
/* One UTF-8 scalar. Printable ASCII or punctuation stand-in into out[3].
 * Controls become spaces. Unsupported scalars consume input and emit zero. */
size_t muse_hid_normalize(const char *s, size_t left, char out[3], size_t *consumed);
