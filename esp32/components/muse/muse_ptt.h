/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Input edges are debounced by the board: three 10 ms samples = 20 ms
 * between the first and accepted sample. All times are monotonic ms. */
typedef enum { MUSE_PTT_EDGE_DOWN, MUSE_PTT_EDGE_UP, MUSE_PTT_EDGE_CANCEL, MUSE_PTT_TICK } muse_ptt_edge_t;
enum { MUSE_PTT_ACT_START = 1, MUSE_PTT_ACT_SEND = 2, MUSE_PTT_ACT_CANCEL = 4, MUSE_PTT_ACT_TAP = 8 };
typedef enum { MUSE_PTT_IDLE, MUSE_PTT_FIRST, MUSE_PTT_LATCHED, MUSE_PTT_WAIT_UP } muse_ptt_phase_t;
typedef struct {
    muse_ptt_phase_t phase;
    bool down;
    uint32_t tap_ms, max_ms;
    uint64_t started_ms;
} muse_ptt_sm_t;
void muse_ptt_init(muse_ptt_sm_t *s, uint32_t tap_ms, uint32_t max_s);
unsigned muse_ptt_step(muse_ptt_sm_t *s, muse_ptt_edge_t edge, uint64_t now_ms);
bool muse_ptt_recording(const muse_ptt_sm_t *s);
