/* SPDX-License-Identifier: Apache-2.0 */
#include "muse_stt_protocol.h"
#include <string.h>
#include "cJSON.h"
muse_stt_message_t muse_stt_parse(const char *json, size_t len, bool committed, char *out, size_t cap)
{
    cJSON *o = cJSON_ParseWithLength(json,len);
    if (!o) return MUSE_STT_MSG_ERROR;
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(o,"message_type");
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(o,"text");
    muse_stt_message_t result = MUSE_STT_MSG_IGNORE;
    if (!cJSON_IsString(kind)) result=MUSE_STT_MSG_ERROR;
    else if (!strcmp(kind->valuestring,"session_started")) result=MUSE_STT_MSG_STARTED;
    else if (!strcmp(kind->valuestring,"committed_transcript")) {
        if (!committed || !cJSON_IsString(text) || strlen(text->valuestring)>=cap) result=MUSE_STT_MSG_ERROR;
        else { memcpy(out,text->valuestring,strlen(text->valuestring)+1); result=MUSE_STT_MSG_COMMITTED; }
    } else if (cJSON_GetObjectItemCaseSensitive(o,"error") || strstr(kind->valuestring,"error") ||
               !strcmp(kind->valuestring,"rate_limited") || !strcmp(kind->valuestring,"quota_exceeded")) result=MUSE_STT_MSG_ERROR;
    cJSON_Delete(o);
    return result;
}
