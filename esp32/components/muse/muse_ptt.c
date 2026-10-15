/* SPDX-License-Identifier: Apache-2.0 */
#include "muse_ptt.h"

void muse_ptt_init(muse_ptt_sm_t *s, uint32_t tap_ms, uint32_t max_s)
{
    *s = (muse_ptt_sm_t){ .tap_ms = tap_ms, .max_ms = max_s * 1000 };
}

bool muse_ptt_recording(const muse_ptt_sm_t *s)
{
    return s->phase == MUSE_PTT_FIRST || s->phase == MUSE_PTT_LATCHED;
}

unsigned muse_ptt_step(muse_ptt_sm_t *s, muse_ptt_edge_t edge, uint64_t now)
{
    /* Cancel wins even at the timeout boundary. Wait for a held key's
     * release, so neither cancel nor auto-send can start a phantom turn. */
    if (edge == MUSE_PTT_EDGE_CANCEL) {
        bool active = muse_ptt_recording(s);
        s->phase = s->down ? MUSE_PTT_WAIT_UP : MUSE_PTT_IDLE;
        return active ? MUSE_PTT_ACT_CANCEL : 0;
    }
    if (edge == MUSE_PTT_EDGE_UP) {
        if (!s->down) return 0;
        s->down = false;
        if (s->phase == MUSE_PTT_WAIT_UP) {
            s->phase = MUSE_PTT_IDLE;
            return 0;
        }
        if (s->phase == MUSE_PTT_FIRST && now - s->started_ms < s->tap_ms) {
            s->phase = MUSE_PTT_LATCHED;
            return MUSE_PTT_ACT_TAP;
        }
        if (s->phase == MUSE_PTT_FIRST) {
            s->phase = MUSE_PTT_IDLE;
            return MUSE_PTT_ACT_SEND;
        }
    } else if (edge == MUSE_PTT_EDGE_DOWN) {
        if (s->down) return 0;
        s->down = true;
        if (s->phase == MUSE_PTT_LATCHED) {
            s->phase = MUSE_PTT_WAIT_UP;
            return MUSE_PTT_ACT_SEND;
        }
        if (s->phase == MUSE_PTT_IDLE) {
            s->started_ms = now;
            s->phase = MUSE_PTT_FIRST;
            return MUSE_PTT_ACT_START;
        }
    }
    if (muse_ptt_recording(s) && now - s->started_ms >= s->max_ms) {
        s->phase = s->down ? MUSE_PTT_WAIT_UP : MUSE_PTT_IDLE;
        return MUSE_PTT_ACT_SEND;
    }
    return 0;
}
