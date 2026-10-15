# SPDX-License-Identifier: Apache-2.0
"""Run the actual typing worker against notification-credit backpressure."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class HidTyping(unittest.TestCase):
    def test_500_characters_and_cancel_release_all_keys(self):
        source=(ROOT/'components/muse/muse_hid_kbd.c').read_text()
        functions=source[source.index('static bool ready('):source.index('bool muse_hid_kbd_type(')]
        code=r'''
#include <assert.h>
#include <limits.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "muse_hid_text.h"
#define pdMS_TO_TICKS(x) (x)
#define portMAX_DELAY 0xffffffff
#define pdTRUE 1
#define BLE_HS_ENOMEM 6
#define BLE_HS_EBUSY 7
#define BLE_HS_EAGAIN 8
#define MUSE_LAT_TYPED 9
static atomic_uint s_conn=42,s_cancel;
static atomic_int s_tx_status;
static atomic_bool s_secure=true,s_subscribed=true;
static uint8_t s_suspend;
static int s_credit,s_queue;
static uint16_t s_input=88,s_boot_input=89;
static uint8_t s_protocol=1;
typedef struct {char *text;uint32_t cancel,turn;uint16_t conn;} typing_t;
static typing_t queued;
struct os_mbuf {uint8_t data[8];};
static uint8_t reports[1200][8];static int count,attempts,reads,typed,ended,cancel_at;
static jmp_buf done;
static struct os_mbuf *ble_hs_mbuf_from_flat(const void *p,int n) {
 assert(n==8);struct os_mbuf *m=malloc(sizeof(*m));memcpy(m->data,p,n);return m;
}
static int ble_gatts_notify_custom(int conn,int attr,struct os_mbuf *m) {
 assert(conn==42 && attr==88);
 if(++attempts%17==0){free(m);return BLE_HS_ENOMEM;}
 if(attempts%23==0){free(m);atomic_store(&s_tx_status,BLE_HS_EBUSY);return 0;}
 memcpy(reports[count++],m->data,8);free(m);atomic_store(&s_tx_status,0);
 if(cancel_at && count==cancel_at)atomic_fetch_add(&s_cancel,1);
 return 0;
}
static int xSemaphoreTake(int s,unsigned timeout){(void)s;(void)timeout;return 1;}
static void vTaskDelay(unsigned ticks){assert(ticks==10);}
static int xQueueReceive(int q,typing_t *t,unsigned timeout){
 (void)q;(void)timeout;if(reads++)longjmp(done,1);*t=queued;return 1;
}
static void muse_latency_mark(uint32_t t,int stage){assert(t==7&&stage==9);typed++;}
static void muse_latency_end(uint32_t t){assert(t==7);ended++;}
''' + functions + r'''
static void run(int cancel) {
 count=attempts=reads=typed=ended=0;cancel_at=cancel;atomic_store(&s_cancel,0);
 char *text=malloc(502);for(int i=0;i<500;i++)text[i]="aB1!"[i%4];text[500]=' ';text[501]=0;
 queued=(typing_t){text,0,7,42};
 if(!setjmp(done))typing_task(NULL);
 assert(typed==1&&ended==1);
 uint8_t zero[8]={0};assert(!memcmp(reports[count-1],zero,8));
 if(!cancel) {
  assert(count==1003); /* 501 key downs + ups, final release-all */
  for(int i=0;i<501;i++) {
   uint8_t expected[8];assert(muse_hid_ascii(i==500?' ':"aB1!"[i%4],expected));
   assert(!memcmp(reports[i*2],expected,8));assert(!memcmp(reports[i*2+1],zero,8));
  }
 } else assert(count<10);
}
int main(void){run(0);run(3);}
'''
        with tempfile.TemporaryDirectory() as tmp:
            out=Path(tmp);(out/'test.c').write_text(code)
            subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-I'+str(ROOT/'components/muse'),
                str(out/'test.c'),str(ROOT/'components/muse/muse_hid_text.c'),'-o',str(out/'test')],check=True)
            subprocess.run([str(out/'test')],check=True)
