# SPDX-License-Identifier: Apache-2.0
"""Use production batching: first sentence alone; later batches keep order."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class TtsQueue(unittest.TestCase):
    def test_probe_discards_only_its_own_job_and_respects_cancellation(self):
        source=(ROOT/'components/muse/muse_tts.c').read_text()
        fn=source[source.index('static bool push('):source.index('static char *request_body(')]
        code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#define pdMS_TO_TICKS(x) (x)
static atomic_uint s_probe_job;
static unsigned current=1, writes;
static int s_sb;
static bool cancelled(unsigned job) { return job!=current; }
static size_t xStreamBufferSend(int unused,const uint8_t *p,size_t n,int timeout) {
    writes++; return n;
}
''' + fn + r'''
int main(void) {
    uint8_t bytes[4]={0};
    assert(push(1,bytes,sizeof(bytes))); assert(writes==1);
    s_probe_job=1;
    assert(push(1,bytes,sizeof(bytes))); assert(writes==1);
    current=2;
    assert(!push(1,bytes,sizeof(bytes))); assert(writes==1);
    assert(push(2,bytes,sizeof(bytes))); assert(writes==2);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            out=Path(tmp);(out/'test.c').write_text(code)
            subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=c11',str(out/'test.c'),'-o',str(out/'test')],check=True)
            subprocess.run([str(out/'test')],check=True)

    def test_first_sentence_and_batching_and_cancellation(self):
        source=(ROOT/'components/muse/muse_tts.c').read_text()
        fn=source[source.index('static char *next_text('):source.index('static esp_http_client_handle_t client(')]
        code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define TTS_IDLE_MS 100
#define TTS_BATCH_MAX 1100
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define heap_caps_realloc(p,n,c) realloc(p,n)
#define heap_caps_free free
static atomic_bool s_waiting;
static int s_q,head,tail;static unsigned current=4;static char *q[8];
static bool cancelled(unsigned job){return job!=current;}
static int uxQueueMessagesWaiting(int unused){return tail-head;}
static int xQueueReceive(int unused,char **p,int timeout){if(head==tail)return 0;*p=q[head++];return 1;}
static int xQueuePeek(int unused,char **p,int timeout){if(head==tail)return 0;*p=q[head];return 1;}
static void add(const char *s){size_t n=strlen(s)+1;q[tail]=malloc(n);memcpy(q[tail++],s,n);}
''' + fn + r'''
int main(void){
 add("Hi. ");add("A later sentence. ");add("And its continuation. ");q[tail++]=NULL;
 bool last;char *text=next_text(4,&last,false);
 assert(!last&&!strcmp(text,"Hi. "));free(text);assert(head==1);
 text=next_text(4,&last,true);assert(last&&!strcmp(text,"A later sentence. And its continuation. "));free(text);
 assert(!next_text(3,&last,true));
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            out=Path(tmp);(out/'test.c').write_text(code)
            subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=c11',str(out/'test.c'),'-o',str(out/'test')],check=True)
            subprocess.run([str(out/'test')],check=True)
