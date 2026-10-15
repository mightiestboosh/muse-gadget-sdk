/* SPDX-License-Identifier: Apache-2.0 */
/* Scribe v2 Realtime, explicit manual commit. Capture never waits for TLS:
 * a PSRAM stream buffer bridges connection setup, and overflow fails the
 * entire dictation. No partial transcript is ever handed to HID. */
#include "muse_stt.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/stream_buffer.h"
#include "mbedtls/base64.h"
#include "muse_hid_kbd.h"
#include "muse_latency.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_stt_protocol.h"

#define HOST "api.elevenlabs.io"
#define PATH "/v1/speech-to-text/realtime?model_id=scribe_v2_realtime&audio_format=pcm_16000&commit_strategy=manual"
#define CHUNK_BYTES 6400  /* 200 ms, 16 kHz mono s16le */
#define PCM_BUFFER (256 * 1024) /* 8 seconds of cold TLS/network setup */
#define MESSAGE_MAX 32768
#define TEXT_MAX 16384
static StreamBufferHandle_t s_pcm;
static TaskHandle_t s_task;
static atomic_bool s_busy, s_start, s_warm, s_commit, s_failed;
static atomic_uint s_job;
static uint32_t s_turn;
static unsigned s_start_job;
/* Only the STT worker owns this, including during warm-up. Closed sockets
 * release radio/PM locks; the transport keeps just the TLS session ticket. */
static esp_transport_handle_t s_ssl;
typedef struct {
    unsigned job; uint32_t turn;
    atomic_bool started, failed, committed, committing;
    char *message, *text;
    size_t length, frame_offset;
    bool text_frame;
} response_t;

static bool cancelled(unsigned job) { return job!=atomic_load(&s_job); }
static void event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)base;
    response_t *r=arg;
    if (cancelled(r->job)) return;
    if (id==WEBSOCKET_EVENT_CONNECTED) {
#if CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
        esp_transport_ssl_session_ticket_operation(s_ssl,ESP_TRANSPORT_SESSION_TICKET_SAVE);
#endif
    } else if (id==WEBSOCKET_EVENT_ERROR || id==WEBSOCKET_EVENT_DISCONNECTED || id==WEBSOCKET_EVENT_CLOSED) {
        if (!atomic_load(&r->committed)) atomic_store(&r->failed,true);
    } else if (id==WEBSOCKET_EVENT_DATA) {
        esp_websocket_event_data_t *e=data;
        if (e->op_code!=1 && e->op_code!=0) return; /* ping/pong/close */
        if (!e->payload_offset) {
            r->frame_offset=0;
            if (e->op_code==1) { r->length=0; r->text_frame=true; }
        }
        if (!r->text_frame || e->data_len<0 || e->payload_offset<0 ||
            (size_t)e->payload_offset!=r->frame_offset || r->length+(size_t)e->data_len>=MESSAGE_MAX) {
            atomic_store(&r->failed,true); return;
        }
        memcpy(r->message+r->length,e->data_ptr,e->data_len);
        r->length+=e->data_len; r->frame_offset+=e->data_len;
        if (e->fin && r->frame_offset==(size_t)e->payload_len) {
            r->message[r->length]=0;
            muse_stt_message_t kind=muse_stt_parse(r->message,r->length,atomic_load(&r->committing),r->text,TEXT_MAX+1);
            if (kind==MUSE_STT_MSG_STARTED) atomic_store(&r->started,true);
            else if (kind==MUSE_STT_MSG_ERROR) atomic_store(&r->failed,true);
            else if (kind==MUSE_STT_MSG_COMMITTED) {
                muse_latency_mark(r->turn,MUSE_LAT_TRANSCRIPT);
                atomic_store(&r->committed,true);
            }
            r->length=0; r->text_frame=false;
        }
    }
}
static bool transport(void)
{
    if (!s_ssl) {
        s_ssl=esp_transport_ssl_init();
        if (s_ssl) {
            esp_transport_set_default_port(s_ssl,443);
            esp_transport_ssl_crt_bundle_attach(s_ssl,esp_crt_bundle_attach);
        }
    }
    return s_ssl!=NULL;
}
static void warm(void)
{
    if (!muse_settings_tts_key_len() || !transport()) return;
#if CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
    esp_transport_ssl_session_ticket_operation(s_ssl,ESP_TRANSPORT_SESSION_TICKET_USE);
#endif
    if (esp_transport_connect(s_ssl,HOST,443,1500)>=0) {
#if CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
        esp_transport_ssl_session_ticket_operation(s_ssl,ESP_TRANSPORT_SESSION_TICKET_SAVE);
#endif
    }
    esp_transport_close(s_ssl);
}
static bool send_audio(esp_websocket_client_handle_t c, const uint8_t *pcm, size_t n, bool commit, char *body)
{
    size_t encoded;
    const char *prefix="{\"message_type\":\"input_audio_chunk\",\"sample_rate\":16000,\"audio_base_64\":\"";
    size_t head=strlen(prefix);
    memcpy(body,prefix,head);
    if (mbedtls_base64_encode((unsigned char *)body+head,CHUNK_BYTES*2,&encoded,pcm,n)) return false;
    int tail=sprintf(body+head+encoded,"\",\"commit\":%s}",commit?"true":"false");
    size_t size=head+encoded+tail;
    return esp_websocket_client_send_text(c,body,(int)size,pdMS_TO_TICKS(500))==(int)size;
}
static void run(unsigned job, uint32_t turn)
{
    if (cancelled(job)) return;
    response_t *r=heap_caps_calloc(1,sizeof(*r),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    uint8_t *pcm=heap_caps_malloc(CHUNK_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    char *body=heap_caps_malloc(CHUNK_BYTES*2+256,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    esp_transport_handle_t ws=NULL;
    esp_websocket_client_handle_t c=NULL;
    bool accepted=false;
    if (!r || !pcm || !body || !transport()) goto done;
    r->job=job; r->turn=turn;
    r->message=heap_caps_malloc(MESSAGE_MAX,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    r->text=heap_caps_malloc(TEXT_MAX+1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!r->message || !r->text) goto done;
    ws=esp_transport_ws_init(s_ssl);
    if (!ws) goto done;
    esp_transport_set_default_port(ws,443);
    esp_transport_ws_set_path(ws,PATH);
    char key[MUSE_TTS_KEY_MAX+1], headers[MUSE_TTS_KEY_MAX+32];
    muse_settings_tts_key(key);
    snprintf(headers,sizeof(headers),"xi-api-key: %s\r\n",key);
    esp_err_t err=esp_transport_ws_set_headers(ws,headers);
    memset(key,0,sizeof(key)); memset(headers,0,sizeof(headers));
    if (err!=ESP_OK) goto done;
#if CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
    esp_transport_ssl_session_ticket_operation(s_ssl,ESP_TRANSPORT_SESSION_TICKET_USE);
#endif
    const esp_websocket_client_config_t cfg={
        .uri="wss://" HOST PATH, .ext_transport=ws,.disable_auto_reconnect=true,
        .network_timeout_ms=3000,.task_stack=8192,.task_prio=4,.task_name="stt_ws",.buffer_size=2048,
    };
    c=esp_websocket_client_init(&cfg);
    if (!c || esp_websocket_register_events(c,WEBSOCKET_EVENT_ANY,event,r)!=ESP_OK || esp_websocket_client_start(c)!=ESP_OK) goto done;
    int64_t until=esp_timer_get_time()+8000000;
    while (!atomic_load(&r->started) && !atomic_load(&r->failed) && !cancelled(job) && !atomic_load(&s_failed) && esp_timer_get_time()<until) vTaskDelay(pdMS_TO_TICKS(10));
    if (!atomic_load(&r->started)) goto done;
    size_t n=0;
    while (!cancelled(job) && !atomic_load(&r->failed) && !atomic_load(&s_failed)) {
        n+=xStreamBufferReceive(s_pcm,pcm+n,CHUNK_BYTES-n,pdMS_TO_TICKS(20));
        bool commit=atomic_load(&s_commit) && xStreamBufferBytesAvailable(s_pcm)==0;
        if (n==CHUNK_BYTES || commit) {
            if (commit) atomic_store(&r->committing,true);
            if (!send_audio(c,pcm,n,commit,body)) goto done;
            n=0;
            if (commit) break;
        }
    }
    until=esp_timer_get_time()+5000000;
    while (!atomic_load(&r->committed) && !atomic_load(&r->failed) && !cancelled(job) && !atomic_load(&s_failed) && esp_timer_get_time()<until) vTaskDelay(pdMS_TO_TICKS(10));
    /* Cleanup the socket before typing: any late transport failure must
     * still make this dictation type nothing. */
    if (!cancelled(job) && !atomic_load(&r->failed) && !atomic_load(&s_failed) && atomic_load(&r->committed)) {
        accepted=muse_hid_kbd_type(r->text,turn);
    }
done:
    if (c) { esp_websocket_client_stop(c); esp_websocket_client_destroy(c); }
    if (ws) esp_transport_destroy(ws);
    if (s_ssl) esp_transport_close(s_ssl);
    if (!cancelled(job)) {
        atomic_store(&s_failed,!accepted);
        if (!accepted) { muse_state_set_caption("DICTATION FAILED - NOTHING TYPED"); muse_latency_end(turn); }
    }
    if (r) { free(r->message); free(r->text); free(r); }
    free(pcm); free(body);
}
static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        if (atomic_exchange(&s_start,false)) {
            run(s_start_job,s_turn);
            atomic_store(&s_busy,false);
        } else if (atomic_exchange(&s_warm,false)) warm();
    }
}
esp_err_t muse_stt_init(void)
{
    if (s_task) return ESP_OK;
    s_pcm=xStreamBufferCreateWithCaps(PCM_BUFFER,1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!s_pcm) return ESP_ERR_NO_MEM;
    return xTaskCreatePinnedToCoreWithCaps(worker,"muse_stt",16384,NULL,4,&s_task,0,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)==pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
esp_err_t muse_stt_start(uint32_t turn)
{
    if (!s_task || !muse_settings_tts_key_len()) return ESP_ERR_INVALID_STATE;
    bool idle=false;
    if (!atomic_compare_exchange_strong(&s_busy,&idle,true)) return ESP_ERR_INVALID_STATE;
    xStreamBufferReset(s_pcm);
    s_start_job=atomic_fetch_add(&s_job,1)+1; s_turn=turn;
    atomic_store(&s_failed,false); atomic_store(&s_commit,false); atomic_store(&s_start,true);
    xTaskNotifyGive(s_task);
    return ESP_OK;
}
bool muse_stt_audio(const int16_t *pcm, size_t frames)
{
    if (!atomic_load(&s_busy) || atomic_load(&s_failed)) return false;
    size_t bytes=frames*sizeof(*pcm);
    if (xStreamBufferSpacesAvailable(s_pcm)<bytes || xStreamBufferSend(s_pcm,pcm,bytes,0)!=bytes) {
        atomic_store(&s_failed,true); return false;
    }
    return true;
}
void muse_stt_finish(void) { atomic_store(&s_commit,true); }
void muse_stt_cancel(void) { atomic_fetch_add(&s_job,1); }
void muse_stt_prewarm(void) { if (s_task) { atomic_store(&s_warm,true); xTaskNotifyGive(s_task); } }
bool muse_stt_busy(void) { return atomic_load(&s_busy); }
bool muse_stt_failed(void) { return atomic_load(&s_failed); }
