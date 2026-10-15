/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
    MUSE_LAT_DOWN, MUSE_LAT_MIC, MUSE_LAT_RELEASE, MUSE_LAT_TEXT,
    MUSE_LAT_TTS_BYTE, MUSE_LAT_SOUND, MUSE_LAT_BARGE, MUSE_LAT_SILENCE,
    MUSE_LAT_TRANSCRIPT, MUSE_LAT_TYPED, MUSE_LAT_COUNT
} muse_latency_stage_t;
#if CONFIG_MUSE_LATENCY
uint32_t muse_latency_begin(bool keyboard, bool usb);
uint32_t muse_latency_turn(void);
void muse_latency_at(uint32_t turn, muse_latency_stage_t stage, int64_t at_us);
void muse_latency_mark(uint32_t turn, muse_latency_stage_t stage);
void muse_latency_end(uint32_t turn);
void muse_latency_print(void);
#else
static inline uint32_t muse_latency_begin(bool k, bool u) { (void)k; (void)u; return 0; }
static inline uint32_t muse_latency_turn(void) { return 0; }
static inline void muse_latency_at(uint32_t t, muse_latency_stage_t s, int64_t a) { (void)t; (void)s; (void)a; }
static inline void muse_latency_mark(uint32_t t, muse_latency_stage_t s) { (void)t; (void)s; }
static inline void muse_latency_end(uint32_t t) { (void)t; }
static inline void muse_latency_print(void) {}
#endif
#ifdef __cplusplus
}
#endif
