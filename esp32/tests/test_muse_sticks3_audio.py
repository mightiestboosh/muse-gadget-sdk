# SPDX-License-Identifier: Apache-2.0
"""Real volume policy and speaker gate: no battery overdrive or ADC amp."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
def function(text,name):
    start=text.index('static void '+name+'(') if 'static void '+name+'(' in text else text.index('void '+name+'(')
    at=text.index('{',start);depth=1;end=at+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
class StickS3Audio(unittest.TestCase):
    def test_battery_usb_and_failed_power_read(self):
        source=(ROOT/'components/muse/muse_audio.c').read_text()
        code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#define CONFIG_MUSE_BOARD_M5STACK_STICKS3 1
#define ESP_OK 0
#define ESP_CODEC_DEV_OK 0
typedef struct {bool usb;} muse_power_t;
static int power_error,applied=-1,s_applied_volume=-1,gate;
static bool usb,speaker=true;
static int read_power(muse_power_t *p){p->usb=usb;return power_error;}
static void speaker_enable(bool on){gate=on;}
static struct {int (*read_power)(muse_power_t *);void (*speaker_enable)(bool);} board={read_power,speaker_enable},*muse_board=&board;
static void lock(void){}
static void unlock(void){}
static bool muse_settings_speaker_on(void){return speaker;}
static int muse_settings_volume(void){return 100;}
static atomic_bool s_playback;
static int s_spk=1;
static bool codecs_reachable(void){return true;}
static int esp_codec_dev_set_out_vol(int spk,int volume){assert(spk==1);applied=volume;return 0;}
''' + function(source,'apply_volume') + '\n' + function(source,'muse_audio_playback') + r'''
int main(void){
 usb=false;apply_volume(100);assert(applied==75);
 usb=true;apply_volume(100);assert(applied==100);
 usb=false;apply_volume(100);assert(applied==75);
 usb=true;power_error=1;s_applied_volume=-1;apply_volume(100);assert(applied==75);
 power_error=0;usb=false;apply_volume(40);assert(applied==40);
 muse_audio_playback(true);assert(gate&&atomic_load(&s_playback));
 muse_audio_playback(false);assert(!gate&&!atomic_load(&s_playback));
 speaker=false;muse_audio_playback(true);assert(!gate);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            out=Path(tmp);(out/'test.c').write_text(code)
            subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=c11',str(out/'test.c'),'-o',str(out/'test')],check=True)
            subprocess.run([str(out/'test')],check=True)
