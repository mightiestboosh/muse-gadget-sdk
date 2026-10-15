/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "sdkconfig.h"
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#if CONFIG_MUSE_KBD_ENABLE
esp_err_t muse_stt_init(void);
esp_err_t muse_stt_start(uint32_t turn);
bool muse_stt_audio(const int16_t *pcm, size_t frames);
void muse_stt_finish(void);
void muse_stt_cancel(void);
void muse_stt_prewarm(void);
bool muse_stt_busy(void);
bool muse_stt_failed(void);
#else
static inline void muse_stt_cancel(void) {}
static inline void muse_stt_prewarm(void) {}
#endif
