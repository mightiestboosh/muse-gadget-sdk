/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "sdkconfig.h"
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
typedef enum { MUSE_KBD_OFF, MUSE_KBD_WAITING, MUSE_KBD_ADVERTISING, MUSE_KBD_CONNECTED } muse_kbd_state_t;
#if CONFIG_MUSE_KBD_ENABLE
esp_err_t muse_hid_kbd_init(void);
void muse_hid_kbd_register(void); /* BLE host configuration, before GATT starts */
bool muse_hid_kbd_advertise(void); /* host queue, only when Muse setup inactive */
void muse_hid_kbd_apply(void);
bool muse_hid_kbd_enabled(void);
esp_err_t muse_hid_kbd_set_enabled(bool enabled);
void muse_hid_kbd_forget(void);
void muse_hid_kbd_cancel(void);
void muse_hid_kbd_shutdown(void);
bool muse_hid_kbd_type(const char *text, uint32_t turn);
muse_kbd_state_t muse_hid_kbd_state(void);
void muse_hid_kbd_host(char out[40]);
#else
static inline bool muse_hid_kbd_enabled(void) { return false; }
static inline void muse_hid_kbd_apply(void) {}
static inline void muse_hid_kbd_cancel(void) {}
static inline muse_kbd_state_t muse_hid_kbd_state(void) { return MUSE_KBD_OFF; }
#endif
