/* SPDX-License-Identifier: Apache-2.0 */
#include "muse_latency.h"
#if CONFIG_MUSE_LATENCY
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define HISTORY 10
typedef struct { uint32_t id; bool keyboard, usb, ended; int64_t us[MUSE_LAT_COUNT]; } turn_t;
static turn_t s_turns[HISTORY];
static uint32_t s_id;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static const char *TAG = "latency";
static const char *const NAMES[] = {"mic", "server", "tts", "decode", "speech", "barge", "dictation"};
static const uint8_t FROM[] = {MUSE_LAT_DOWN,MUSE_LAT_RELEASE,MUSE_LAT_TEXT,MUSE_LAT_TTS_BYTE,MUSE_LAT_TEXT,MUSE_LAT_BARGE,MUSE_LAT_RELEASE};
static const uint8_t TO[] = {MUSE_LAT_MIC,MUSE_LAT_TEXT,MUSE_LAT_TTS_BYTE,MUSE_LAT_SOUND,MUSE_LAT_SOUND,MUSE_LAT_SILENCE,MUSE_LAT_TYPED};
static int64_t delta(const turn_t *t, unsigned metric)
{
    int64_t a = t->us[FROM[metric]], b = t->us[TO[metric]];
    return a && b >= a ? (b - a) / 1000 : -1;
}
static void print_turn(const turn_t *t)
{
    char line[500];
    int n = snprintf(line, sizeof(line), "LAT turn=%" PRIu32 " mode=%s power=%s", t->id,
                     t->keyboard ? "dictation" : "muse", t->usb ? "usb" : "battery");
    /* Absolute timestamps allow host tools to recompute every stage. */
    for (int i = 0; i < MUSE_LAT_COUNT && n < (int)sizeof(line); i++) n += snprintf(line+n, sizeof(line)-n, " t%d=%" PRId64, i, t->us[i]);
    for (unsigned i = 0; i < sizeof(FROM) && n < (int)sizeof(line); i++) n += snprintf(line+n, sizeof(line)-n, " %s_ms=%" PRId64, NAMES[i], delta(t,i));
    ESP_LOGI(TAG, "%s", line);
}
uint32_t muse_latency_begin(bool k, bool usb)
{
    uint32_t old = muse_latency_turn();
    muse_latency_end(old);
    portENTER_CRITICAL(&s_lock);
    if (++s_id == 0) ++s_id;
    s_turns[s_id % HISTORY] = (turn_t){.id=s_id, .keyboard=k, .usb=usb};
    uint32_t id = s_id;
    portEXIT_CRITICAL(&s_lock);
    muse_latency_mark(id, MUSE_LAT_DOWN);
    return id;
}
uint32_t muse_latency_turn(void)
{
    portENTER_CRITICAL(&s_lock);
    uint32_t id = s_id;
    portEXIT_CRITICAL(&s_lock);
    return id;
}
void muse_latency_at(uint32_t id, muse_latency_stage_t stage, int64_t us)
{
    if (!id || stage >= MUSE_LAT_COUNT) return;
    portENTER_CRITICAL(&s_lock);
    turn_t *t = &s_turns[id % HISTORY];
    if (t->id == id && !t->ended && !t->us[stage]) t->us[stage] = us;
    portEXIT_CRITICAL(&s_lock);
}
void muse_latency_mark(uint32_t id, muse_latency_stage_t stage) { muse_latency_at(id, stage, esp_timer_get_time()); }
bool muse_latency_seen(uint32_t id, muse_latency_stage_t stage)
{
    if (!id || stage>=MUSE_LAT_COUNT) return false;
    portENTER_CRITICAL(&s_lock);
    turn_t *t=&s_turns[id % HISTORY];
    bool seen=t->id==id && !t->ended && t->us[stage]!=0;
    portEXIT_CRITICAL(&s_lock);
    return seen;
}
void muse_latency_end(uint32_t id)
{
    if (!id) return;
    turn_t copy;
    portENTER_CRITICAL(&s_lock);
    turn_t *t = &s_turns[id % HISTORY];
    bool emit = t->id == id && !t->ended;
    if (emit) { t->ended = true; copy = *t; }
    portEXIT_CRITICAL(&s_lock);
    if (emit) print_turn(&copy);
}
void muse_latency_print(void)
{
    turn_t turns[HISTORY];
    portENTER_CRITICAL(&s_lock);
    memcpy(turns, s_turns, sizeof(turns));
    uint32_t end = s_id;
    portEXIT_CRITICAL(&s_lock);
    for (uint32_t id = end > HISTORY ? end - HISTORY + 1 : 1; id && id <= end; id++)
        if (turns[id % HISTORY].id == id) print_turn(&turns[id % HISTORY]);
    /* Separate mode/power groups: never blend USB and battery medians. */
    for (int group = 0; group < 4; group++) for (unsigned m = 0; m < sizeof(FROM); m++) {
        int64_t vals[HISTORY]; int count = 0;
        for (int i = 0; i < HISTORY; i++) {
            turn_t *t = &turns[i];
            int64_t v = delta(t,m);
            if (t->id && t->ended && t->keyboard == !!(group & 2) && t->usb == !!(group & 1) && v >= 0) {
                int j = count++;
                while (j && vals[j-1] > v) { vals[j] = vals[j-1]; --j; }
                vals[j] = v;
            }
        }
        if (count) ESP_LOGI(TAG, "LAT median mode=%s power=%s %s_ms=%" PRId64 " n=%d",
            group & 2 ? "dictation" : "muse", group & 1 ? "usb" : "battery", NAMES[m],
            (vals[(count-1)/2] + vals[count/2])/2, count);
    }
}
#endif
