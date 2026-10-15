/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include <stdbool.h>
typedef enum { MUSE_STT_MSG_IGNORE, MUSE_STT_MSG_STARTED, MUSE_STT_MSG_COMMITTED, MUSE_STT_MSG_ERROR } muse_stt_message_t;
/* Only manual-commit results may fill transcript. Errors never expose the
 * remote error body (it can contain sensitive request data). */
muse_stt_message_t muse_stt_parse(const char *json, size_t len, bool committed,
                                 char *transcript, size_t cap);
