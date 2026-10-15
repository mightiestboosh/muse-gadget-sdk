# SPDX-License-Identifier: Apache-2.0
"""Committed-only transcript policy and production 200 ms audio serializer."""
import ctypes as C
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
JSON=Path(os.environ.get('CJSON_SOURCE_DIR',ROOT/'managed_components/espressif__cjson/cJSON'))
class SttProtocol(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.tmp.cleanup)
        lib=Path(cls.tmp.name)/'stt.so'
        subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-shared','-fPIC',
            '-I'+str(JSON),str(ROOT/'components/muse/muse_stt_protocol.c'),str(JSON/'cJSON.c'),'-o',str(lib)],check=True)
        cls.lib=C.CDLL(str(lib));cls.lib.muse_stt_parse.argtypes=[C.c_char_p,C.c_size_t,C.c_bool,C.c_char_p,C.c_size_t]
    def parse(self,json,commit=True,cap=100):
        raw=json.encode();out=C.create_string_buffer(cap);out.value=b'untouched'
        result=self.lib.muse_stt_parse(raw,len(raw),commit,out,cap)
        return result,out.value
    def test_never_types_partial(self):
        self.assertEqual(self.parse('{"message_type":"partial_transcript","text":"wrong"}'),(0,b'untouched'))
    def test_committed_only_after_manual_commit(self):
        message='{"message_type":"committed_transcript","text":"hello"}'
        self.assertEqual(self.parse(message),(2,b'hello'))
        self.assertEqual(self.parse(message,False),(3,b'untouched'))
    def test_errors_and_overflow_do_not_type(self):
        for msg in ['{"message_type":"auth_error","error":"private"}',
                    '{"message_type":"rate_limited"}', '{"message_type":"committed_transcript","text":null}',
                    '{broken', '{"message_type":"committed_transcript","text":"01234567890"}']:
            self.assertEqual(self.parse(msg,cap=11),(3,b'untouched'))
    def test_session_and_empty_commit(self):
        self.assertEqual(self.parse('{"message_type":"session_started"}')[0],1)
        self.assertEqual(self.parse('{"message_type":"committed_transcript","text":""}'),(2,b''))
    def test_transport_serializes_pcm_and_commit(self):
        source=(ROOT/'components/muse/muse_stt.c').read_text()
        fn=source[source.index('static bool send_audio('):source.index('static void run(')]
        code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "mbedtls/base64.h"
#include "cJSON.h"
#define CHUNK_BYTES 6400
#define pdMS_TO_TICKS(x) (x)
typedef void *esp_websocket_client_handle_t;
static char sent[14000]; static int short_write;
static int esp_websocket_client_send_text(void *c,const char *data,int n,int timeout) {
 (void)c; assert(timeout==500); memcpy(sent,data,n);sent[n]=0;return n-short_write;
}
''' + fn + r'''
int main(void) {
 uint8_t pcm[6400],decoded[6401];char body[14000];
 for(int i=0;i<6400;i++)pcm[i]=(uint8_t)i;
 assert(send_audio(NULL,pcm,6400,false,body));
 cJSON *o=cJSON_Parse(sent);assert(o);
 assert(!cJSON_IsTrue(cJSON_GetObjectItem(o,"commit")));
 assert(cJSON_GetObjectItem(o,"sample_rate")->valueint==16000);
 const char *s=cJSON_GetObjectItem(o,"audio_base_64")->valuestring;size_t n;
 assert(!mbedtls_base64_decode(decoded,sizeof(decoded),&n,(const unsigned char *)s,strlen(s)));
 assert(n==6400 && !memcmp(pcm,decoded,n));cJSON_Delete(o);
 assert(send_audio(NULL,pcm,0,true,body));o=cJSON_Parse(sent);
 assert(cJSON_IsTrue(cJSON_GetObjectItem(o,"commit")));cJSON_Delete(o);
 short_write=1;assert(!send_audio(NULL,pcm,100,false,body));
}
'''
        out=Path(self.tmp.name);(out/'send.c').write_text(code)
        # Same host mbedcrypto that the existing crypto harness uses.
        flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','mbedcrypto'],text=True))
        subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-I'+str(JSON),str(out/'send.c'),
            str(JSON/'cJSON.c'),*flags,'-o',str(out/'send')],check=True)
        subprocess.run([str(out/'send')],check=True)
