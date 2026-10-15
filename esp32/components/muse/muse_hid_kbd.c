/* SPDX-License-Identifier: Apache-2.0 */
/* HID over GATT, US keyboard. One legacy advertising owner at a time;
 * see docs/KEYBOARD.md before changing the Muse coexistence hooks. */
#include "muse_hid_kbd.h"
#include <stdatomic.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/ble_store.h"
#include "services/gap/ble_svc_gap.h"
#include "nvs.h"
#include "muse_hid_text.h"
#include "muse_latency.h"
#include "muse_link.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_stt.h"

static const char *TAG = "muse_kbd";
#define NAME "StickS3 Keys"
#define NO_CONN BLE_HS_CONN_HANDLE_NONE
static atomic_bool s_enabled, s_adv, s_secure, s_subscribed;
static atomic_uint s_conn = NO_CONN, s_cancel;
static atomic_int s_tx_status;
static uint16_t s_input, s_boot_input;
static uint8_t s_leds, s_protocol = 1, s_suspend;
static SemaphoreHandle_t s_credit, s_lock;
static QueueHandle_t s_queue;
static nvs_handle_t s_nvs;
/* Metadata is separate from NimBLE's encrypted bond store. Only these two
 * peer identities are deleted by Forget keyboard hosts. Last is index 0. */
static struct { uint8_t count; ble_addr_t peers[2]; } s_hosts;
static bool s_directed_tried;
static char s_host[40];
static int64_t s_host_until;
typedef struct { char *text; uint32_t cancel, turn; uint16_t conn; } typing_t;

static const uint8_t REPORT_MAP[] = {
    0x05,0x01, 0x09,0x06, 0xa1,0x01, 0x85,0x01,
    0x05,0x07, 0x19,0xe0, 0x29,0xe7, 0x15,0x00, 0x25,0x01,
    0x75,0x01, 0x95,0x08, 0x81,0x02,
    0x95,0x01, 0x75,0x08, 0x81,0x01,
    0x95,0x05, 0x75,0x01, 0x05,0x08, 0x19,0x01, 0x29,0x05, 0x91,0x02,
    0x95,0x01, 0x75,0x03, 0x91,0x01,
    0x95,0x06, 0x75,0x08, 0x15,0x00, 0x25,0x65,
    0x05,0x07, 0x19,0x00, 0x29,0x65, 0x81,0x00, 0xc0
};
static int append(struct ble_gatt_access_ctxt *c, const void *p, size_t n)
{ return os_mbuf_append(c->om,p,n) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0; }
static int access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *c, void *arg)
{
    (void)attr;
    unsigned uuid = (unsigned)(uintptr_t)arg;
    static const uint8_t info[] = {0x11,0x01,0x00,0x02}; /* HID 1.11, normally connectable */
    /* Bluetooth SIG vendor ID 0xFFFF: unassigned development device, no
     * claim to an assigned M5Stack/Apple/USB vendor ID. */
    static const uint8_t pnp[] = {0x01,0xff,0xff,0x01,0x00,0x01,0x00};
    static const uint8_t zero[8];
    if (uuid == 0x2908) { const uint8_t ref[] = {1,1}; return append(c,ref,2); }
    if (uuid == 0x2909) { const uint8_t ref[] = {1,2}; return append(c,ref,2); }
    if (c->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        struct ble_gap_conn_desc d;
        if (conn != atomic_load(&s_conn) || ble_gap_conn_find(conn,&d) || !d.sec_state.encrypted || !d.sec_state.bonded)
            return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
        uint8_t value; uint16_t len;
        if (OS_MBUF_PKTLEN(c->om) != 1 || ble_hs_mbuf_to_flat(c->om,&value,1,&len)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (uuid == 0x2a4d || uuid == 0x2a32) s_leds = value;
        else if (uuid == 0x2a4e) { if (value > 1) return BLE_ATT_ERR_VALUE_NOT_ALLOWED; s_protocol = value; }
        else if (uuid == 0x2a4c) { if (value > 1) return BLE_ATT_ERR_VALUE_NOT_ALLOWED; s_suspend = !value; }
        return 0;
    }
    switch (uuid) {
    case 0x2a4a: return append(c,info,sizeof(info));
    case 0x2a4b: return append(c,REPORT_MAP,sizeof(REPORT_MAP));
    case 0x2a4e: return append(c,&s_protocol,1);
    case 0x2a4d: return append(c,&s_leds,1);
    case 0x2a32: return append(c,&s_leds,1);
    case 0x2a22: return append(c,zero,sizeof(zero));
    case 0x2a19: { int p = muse_state_power().battery_pct; uint8_t pct = p < 0 ? 0 : p; return append(c,&pct,1); }
    case 0x2a29: return append(c,"Muse community",14);
    case 0x2a24: return append(c,"StickS3 Keys",12);
    case 0x2a50: return append(c,pnp,sizeof(pnp));
    default: return BLE_ATT_ERR_UNLIKELY;
    }
}
#define UUID(n) BLE_UUID16_DECLARE(n)
#define READ (BLE_GATT_CHR_F_READ)
#define SEC_READ (READ | BLE_GATT_CHR_F_READ_ENC)
#define WRITE (BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC)
static const struct ble_gatt_svc_def SERVICES[] = {
    {.type=BLE_GATT_SVC_TYPE_PRIMARY, .uuid=UUID(0x1812), .characteristics=(struct ble_gatt_chr_def[]){
        {.uuid=UUID(0x2a4a),.access_cb=access,.arg=(void *)0x2a4a,.flags=READ},
        {.uuid=UUID(0x2a4b),.access_cb=access,.arg=(void *)0x2a4b,.flags=READ},
        {.uuid=UUID(0x2a4c),.access_cb=access,.arg=(void *)0x2a4c,.flags=WRITE|BLE_GATT_CHR_F_WRITE_NO_RSP},
        {.uuid=UUID(0x2a4e),.access_cb=access,.arg=(void *)0x2a4e,.flags=SEC_READ|WRITE|BLE_GATT_CHR_F_WRITE_NO_RSP},
        {.uuid=UUID(0x2a22),.access_cb=access,.arg=(void *)0x2a22,.flags=SEC_READ|BLE_GATT_CHR_F_NOTIFY,.val_handle=&s_boot_input},
        {.uuid=UUID(0x2a32),.access_cb=access,.arg=(void *)0x2a32,.flags=SEC_READ|WRITE|BLE_GATT_CHR_F_WRITE_NO_RSP},
        {.uuid=UUID(0x2a4d),.access_cb=access,.arg=(void *)0x2a22,.flags=SEC_READ|BLE_GATT_CHR_F_NOTIFY,.val_handle=&s_input,
         .descriptors=(struct ble_gatt_dsc_def[]){
             {.uuid=UUID(0x2908),.att_flags=BLE_ATT_F_READ,.access_cb=access,.arg=(void *)0x2908},{0}}},
        {.uuid=UUID(0x2a4d),.access_cb=access,.arg=(void *)0x2a4d,.flags=SEC_READ|WRITE|BLE_GATT_CHR_F_WRITE_NO_RSP,
         .descriptors=(struct ble_gatt_dsc_def[]){
             {.uuid=UUID(0x2908),.att_flags=BLE_ATT_F_READ,.access_cb=access,.arg=(void *)0x2909},{0}}},
        {0}}},
    {.type=BLE_GATT_SVC_TYPE_PRIMARY,.uuid=UUID(0x180f),.characteristics=(struct ble_gatt_chr_def[]){
        {.uuid=UUID(0x2a19),.access_cb=access,.arg=(void *)0x2a19,.flags=READ},{0}}},
    {.type=BLE_GATT_SVC_TYPE_PRIMARY,.uuid=UUID(0x180a),.characteristics=(struct ble_gatt_chr_def[]){
        {.uuid=UUID(0x2a29),.access_cb=access,.arg=(void *)0x2a29,.flags=READ},
        {.uuid=UUID(0x2a24),.access_cb=access,.arg=(void *)0x2a24,.flags=READ},
        {.uuid=UUID(0x2a50),.access_cb=access,.arg=(void *)0x2a50,.flags=READ},{0}}},
    {0}
};
#undef UUID

static bool same(const ble_addr_t *a, const ble_addr_t *b) { return a->type == b->type && !memcmp(a->val,b->val,6); }
static bool host_allowed(uint16_t conn)
{
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(conn,&d)) return false;
    xSemaphoreTake(s_lock,portMAX_DELAY);
    bool allowed=s_hosts.count<2;
    for (int i=0;i<s_hosts.count;i++) allowed |= same(&d.peer_id_addr,&s_hosts.peers[i]);
    xSemaphoreGive(s_lock);
    return allowed;
}
static void remember(const ble_addr_t *peer)
{
    xSemaphoreTake(s_lock,portMAX_DELAY);
    int i;
    for (i=0;i<s_hosts.count;i++) if (same(peer,&s_hosts.peers[i])) break;
    if (i < 2) {
        if (i == s_hosts.count) s_hosts.count++;
        if (i == 1) s_hosts.peers[1] = s_hosts.peers[0];
        s_hosts.peers[0] = *peer;
        nvs_set_blob(s_nvs,"hosts",&s_hosts,sizeof(s_hosts));
        nvs_commit(s_nvs);
    } else {
        /* No automatic eviction: two hosts were explicitly requested. */
        ble_store_util_delete_peer(peer);
        ble_gap_terminate(atomic_load(&s_conn),BLE_ERR_REM_USER_CONN_TERM);
        muse_state_set_caption("FORGET KEYBOARD HOSTS FIRST");
    }
    xSemaphoreGive(s_lock);
}
static int host_name(uint16_t conn, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg)
{
    (void)arg;
    if (conn != atomic_load(&s_conn) || error->status || !attr) return 0;
    char name[40]; uint16_t n;
    if (!ble_hs_mbuf_to_flat(attr->om,name,sizeof(name)-1,&n) && n) {
        name[n]=0;
        for (int i=0;i<n;i++) if ((unsigned char)name[i]<32 || (unsigned char)name[i]>126) name[i]='?';
        xSemaphoreTake(s_lock,portMAX_DELAY);
        strlcpy(s_host,name,sizeof(s_host)); s_host_until=esp_timer_get_time()+4000000;
        xSemaphoreGive(s_lock);
        muse_state_poke();
    }
    return 0;
}
static int gap(struct ble_gap_event *e, void *arg)
{
    (void)arg;
    switch (e->type) {
    case BLE_GAP_EVENT_CONNECT:
        atomic_store(&s_adv,false);
        if (!e->connect.status) {
            atomic_store(&s_conn,e->connect.conn_handle);
            s_suspend=0;
            if (!muse_hid_kbd_enabled() || muse_settings_ble_on() || !host_allowed(e->connect.conn_handle)) ble_gap_terminate(e->connect.conn_handle,BLE_ERR_REM_USER_CONN_TERM);
            else if (ble_gap_security_initiate(e->connect.conn_handle)) ble_gap_terminate(e->connect.conn_handle,BLE_ERR_REM_USER_CONN_TERM);
        } else muse_link_ble_apply();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        if (atomic_load(&s_conn)==e->disconnect.conn.conn_handle) {
            atomic_store(&s_conn,NO_CONN); atomic_store(&s_secure,false); atomic_store(&s_subscribed,false);
        }
        muse_hid_kbd_cancel(); muse_stt_cancel();
        s_directed_tried=false;
        muse_link_ble_apply();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        atomic_store(&s_adv,false); muse_link_ble_apply(); break;
    case BLE_GAP_EVENT_ENC_CHANGE: {
        struct ble_gap_conn_desc d;
        if (!e->enc_change.status && !ble_gap_conn_find(e->enc_change.conn_handle,&d) && d.sec_state.encrypted && d.sec_state.bonded) {
            atomic_store(&s_secure,true); remember(&d.peer_id_addr);
            xSemaphoreTake(s_lock,portMAX_DELAY);
            strlcpy(s_host,"KEYBOARD CONNECTED",sizeof(s_host)); s_host_until=esp_timer_get_time()+4000000;
            xSemaphoreGive(s_lock);
            const struct ble_gap_upd_params params={.itvl_min=24,.itvl_max=40,.latency=4,.supervision_timeout=600};
            ble_gap_update_params(e->enc_change.conn_handle,&params);
            ble_uuid16_t name=BLE_UUID16_INIT(0x2a00);
            ble_gattc_read_by_uuid(e->enc_change.conn_handle,1,0xffff,&name.u,host_name,NULL);
        } else ble_gap_terminate(e->enc_change.conn_handle,BLE_ERR_REM_USER_CONN_TERM);
        break;
    }
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (e->subscribe.attr_handle==(s_protocol ? s_input : s_boot_input)) atomic_store(&s_subscribed,e->subscribe.cur_notify);
        break;
    case BLE_GAP_EVENT_NOTIFY_TX:
        if (e->notify_tx.conn_handle==atomic_load(&s_conn) &&
            (e->notify_tx.attr_handle==s_input || e->notify_tx.attr_handle==s_boot_input)) {
            atomic_store(&s_tx_status,e->notify_tx.status);
            xSemaphoreGive(s_credit);
        }
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc d;
        if (!ble_gap_conn_find(e->repeat_pairing.conn_handle,&d)) ble_store_util_delete_peer(&d.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    default: break;
    }
    return 0;
}

void muse_hid_kbd_register(void)
{
    ESP_ERROR_CHECK(ble_gatts_count_cfg(SERVICES)==0 ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(ble_gatts_add_svcs(SERVICES)==0 ? ESP_OK : ESP_FAIL);
}
bool muse_hid_kbd_advertise(void)
{
    if (!muse_hid_kbd_enabled() || muse_settings_ble_on()) return false;
    if (atomic_load(&s_adv) || atomic_load(&s_conn)!=NO_CONN) return true;
    ble_hs_cfg.sm_io_cap=BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_mitm=0; ble_hs_cfg.sm_sc=1; ble_hs_cfg.sm_bonding=1;
    ble_svc_gap_device_name_set(NAME);
    struct ble_gap_adv_params params={.conn_mode=BLE_GAP_CONN_MODE_UND,.disc_mode=BLE_GAP_DISC_MODE_GEN,
        .itvl_min=BLE_GAP_ADV_ITVL_MS(1000),.itvl_max=BLE_GAP_ADV_ITVL_MS(1020)};
    int rc;
    xSemaphoreTake(s_lock,portMAX_DELAY);
    ble_addr_t last = s_hosts.peers[0]; bool direct = s_hosts.count && !s_directed_tried;
    s_directed_tried=true;
    xSemaphoreGive(s_lock);
    if (direct) {
        params.conn_mode=BLE_GAP_CONN_MODE_DIR; params.disc_mode=BLE_GAP_DISC_MODE_NON;
        rc=ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC,&last,5000,&params,gap,NULL);
        if (!rc) { atomic_store(&s_adv,true); return true; }
    }
    params.conn_mode=BLE_GAP_CONN_MODE_UND; params.disc_mode=BLE_GAP_DISC_MODE_GEN;
    ble_uuid16_t hid=BLE_UUID16_INIT(0x1812);
    struct ble_hs_adv_fields adv={.flags=BLE_HS_ADV_F_DISC_GEN|BLE_HS_ADV_F_BREDR_UNSUP,
        .uuids16=&hid,.num_uuids16=1,.uuids16_is_complete=1,.appearance=0x03c1,.appearance_is_present=1,
        .name=(uint8_t *)NAME,.name_len=sizeof(NAME)-1,.name_is_complete=1};
    struct ble_hs_adv_fields rsp={0};
    rc=ble_gap_adv_set_fields(&adv);
    if (!rc) rc=ble_gap_adv_rsp_set_fields(&rsp);
    if (!rc) rc=ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC,NULL,BLE_HS_FOREVER,&params,gap,NULL);
    if (!rc) atomic_store(&s_adv,true);
    else ESP_LOGW(TAG,"advertise rc=%d",rc);
    return true;
}

static bool ready(uint16_t conn)
{ return conn==atomic_load(&s_conn) && atomic_load(&s_secure) && atomic_load(&s_subscribed) && !s_suspend; }
static bool report(const typing_t *t, const uint8_t r[8], bool release)
{
    while (ready(t->conn) && (release || t->cancel==atomic_load(&s_cancel))) {
        xSemaphoreTake(s_credit,0); /* drain a prior report's completion */
        atomic_store(&s_tx_status,INT_MIN);
        struct os_mbuf *om=ble_hs_mbuf_from_flat(r,8);
        int rc=om ? ble_gatts_notify_custom(t->conn,s_protocol ? s_input : s_boot_input,om) : BLE_HS_ENOMEM;
        if (!rc) {
            /* Do not advance just because an arbitrary timeout expired.
             * The exact report waits for its TX result; failed TX retries
             * this same edge. Disconnect/cancel abandons the old host. */
            while (ready(t->conn) && (release || t->cancel==atomic_load(&s_cancel)) && atomic_load(&s_tx_status)==INT_MIN)
                xSemaphoreTake(s_credit,pdMS_TO_TICKS(20));
            if (atomic_load(&s_tx_status)==0) {
                vTaskDelay(pdMS_TO_TICKS(10));
                return true;
            }
            if (!ready(t->conn) || (!release && t->cancel!=atomic_load(&s_cancel))) return false;
            continue;
        }
        if (rc!=BLE_HS_ENOMEM && rc!=BLE_HS_EBUSY && rc!=BLE_HS_EAGAIN) return false;
        xSemaphoreTake(s_credit,pdMS_TO_TICKS(20)); /* retain this exact character */
    }
    return false;
}
static void typing_task(void *arg)
{
    (void)arg;
    for (;;) {
        typing_t t;
        xQueueReceive(s_queue,&t,portMAX_DELAY);
        bool first=true;
        for (char *p=t.text; *p && t.cancel==atomic_load(&s_cancel); p++) {
            uint8_t r[8];
            if (!muse_hid_ascii(*p,r) || !report(&t,r,false)) break;
            if (first) { muse_latency_mark(t.turn,MUSE_LAT_TYPED); first=false; }
            memset(r,0,8);
            if (!report(&t,r,true)) break;
        }
        const uint8_t up[8]={0};
        report(&t,up,true); /* also release modifiers on side-button cancel */
        free(t.text);
        muse_latency_end(t.turn);
    }
}
bool muse_hid_kbd_type(const char *text, uint32_t turn)
{
    uint16_t conn=atomic_load(&s_conn);
    if (!s_queue || !muse_hid_kbd_enabled() || !ready(conn)) return false;
    size_t len=strlen(text), pos=0, dropped=0;
    if (len>16384) return false; /* reject whole transcript, never truncate */
    char *out=heap_caps_malloc(len+2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!out) return false;
    for (size_t at=0;at<len;) {
        char repl[3]; size_t used;
        size_t n=muse_hid_normalize(text+at,len-at,repl,&used);
        if (!n) { ESP_LOGW(TAG,"dropped unsupported UTF-8 scalar (%u bytes)",(unsigned)used); dropped++; }
        memcpy(out+pos,repl,n); pos+=n; at+=used;
    }
    (void)dropped;
    while (pos && out[pos-1]==' ') --pos;
    if (!pos) { free(out); return false; }
    out[pos++]=' '; out[pos]=0;
    typing_t t={.text=out,.conn=conn,.turn=turn,.cancel=atomic_load(&s_cancel)};
    if (xQueueSend(s_queue,&t,0)!=pdTRUE) { free(out); return false; }
    return true;
}
void muse_hid_kbd_cancel(void) { atomic_fetch_add(&s_cancel,1); if (s_credit) xSemaphoreGive(s_credit); }
bool muse_hid_kbd_enabled(void) { return atomic_load(&s_enabled); }
muse_kbd_state_t muse_hid_kbd_state(void)
{
    if (!muse_hid_kbd_enabled()) return MUSE_KBD_OFF;
    if (atomic_load(&s_secure)) return MUSE_KBD_CONNECTED;
    return atomic_load(&s_adv) ? MUSE_KBD_ADVERTISING : MUSE_KBD_WAITING;
}
void muse_hid_kbd_host(char out[40])
{
    out[0]=0;
    if (!s_lock) return;
    xSemaphoreTake(s_lock,portMAX_DELAY);
    if (esp_timer_get_time()<s_host_until) strlcpy(out,s_host,40);
    xSemaphoreGive(s_lock);
}
void muse_hid_kbd_shutdown(void)
{
    muse_hid_kbd_cancel(); muse_stt_cancel();
    if (atomic_exchange(&s_adv,false)) ble_gap_adv_stop();
    unsigned conn=atomic_exchange(&s_conn,NO_CONN);
    atomic_store(&s_secure,false); atomic_store(&s_subscribed,false);
    if (conn!=NO_CONN) ble_gap_terminate(conn,BLE_ERR_REM_USER_CONN_TERM);
}
void muse_hid_kbd_apply(void)
{
    if (!muse_hid_kbd_enabled() || muse_settings_ble_on()) muse_hid_kbd_shutdown();
    s_directed_tried=false;
    muse_link_ble_apply();
}
esp_err_t muse_hid_kbd_set_enabled(bool on)
{
    if (!s_nvs) return ESP_ERR_INVALID_STATE;
    esp_err_t err=nvs_set_u8(s_nvs,"enabled",on);
    if (err==ESP_OK) err=nvs_commit(s_nvs);
    if (err!=ESP_OK) return err;
    atomic_store(&s_enabled,on);
    muse_hid_kbd_apply();
    if (on) muse_stt_prewarm();
    return ESP_OK;
}
void muse_hid_kbd_forget(void)
{
    muse_hid_kbd_shutdown();
    xSemaphoreTake(s_lock,portMAX_DELAY);
    for (int i=0;i<s_hosts.count;i++) ble_store_util_delete_peer(&s_hosts.peers[i]);
    memset(&s_hosts,0,sizeof(s_hosts)); nvs_erase_key(s_nvs,"hosts"); nvs_commit(s_nvs);
    xSemaphoreGive(s_lock);
    muse_hid_kbd_apply();
}
esp_err_t muse_hid_kbd_init(void)
{
    if (s_queue) return ESP_OK;
    esp_err_t err=nvs_open("kbd",NVS_READWRITE,&s_nvs);
    if (err!=ESP_OK) return err;
    uint8_t on=0; nvs_get_u8(s_nvs,"enabled",&on);
    size_t size=sizeof(s_hosts);
    if (nvs_get_blob(s_nvs,"hosts",&s_hosts,&size)!=ESP_OK || size!=sizeof(s_hosts) || s_hosts.count>2) memset(&s_hosts,0,sizeof(s_hosts));
    s_credit=xSemaphoreCreateBinary(); s_lock=xSemaphoreCreateMutex(); s_queue=xQueueCreate(1,sizeof(typing_t));
    if (!s_credit || !s_lock || !s_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCoreWithCaps(typing_task,"hid_type",4096,NULL,4,NULL,0,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS) return ESP_ERR_NO_MEM;
    atomic_store(&s_enabled,on!=0); /* publish only after host callbacks can use the locks */
    return ESP_OK;
}
