# SPDX-License-Identifier: Apache-2.0
"""Production gesture machine: every row in the StickS3 input contract."""
import ctypes as C
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
class State(C.Structure):
    _fields_ = [('phase', C.c_int), ('down', C.c_bool), ('tap_ms', C.c_uint32),
                ('max_ms', C.c_uint32), ('started_ms', C.c_uint64)]
class Ptt(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(); cls.addClassCleanup(cls.tmp.cleanup)
        lib = Path(cls.tmp.name) / 'ptt.so'
        subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-shared', '-fPIC',
                        str(ROOT/'components/muse/muse_ptt.c'), '-o', str(lib)], check=True)
        cls.lib = C.CDLL(str(lib))
        cls.lib.muse_ptt_init.argtypes = [C.POINTER(State), C.c_uint32, C.c_uint32]
        cls.lib.muse_ptt_step.argtypes = [C.POINTER(State), C.c_int, C.c_uint64]
    def setUp(self):
        self.s = State(); self.lib.muse_ptt_init(C.byref(self.s), 400, 60)
    def step(self, edge, ms): return self.lib.muse_ptt_step(C.byref(self.s), edge, ms)
    def test_hold_starts_on_down_sends_on_release(self):
        self.assertEqual(self.step(0, 100), 1)
        self.assertEqual(self.step(3, 499), 0)
        self.assertEqual(self.step(1, 500), 2)
        self.assertEqual(self.step(1, 501), 0)
    def test_tap_latches_next_press_sends(self):
        self.assertEqual(self.step(0, 100), 1)
        self.assertEqual(self.step(1, 499), 8)
        self.assertEqual(self.s.phase, 2)
        self.assertEqual(self.step(0, 700), 2)
        self.assertEqual(self.step(0, 710), 0)
        self.assertEqual(self.step(1, 720), 0)
        self.assertEqual(self.step(0, 800), 1)
    def test_side_cancels_held_and_tap(self):
        for latched in [False, True]:
            with self.subTest(latched=latched):
                self.setUp(); self.step(0, 100)
                if latched: self.step(1, 150)
                self.assertEqual(self.step(2, 300), 4)
                self.assertEqual(self.step(1, 600), 0)
                self.assertEqual(self.step(3, 70000), 0)
                self.assertEqual(self.step(0, 71000), 1)
    def test_timeout_held_and_tap(self):
        for latched in [False, True]:
            with self.subTest(latched=latched):
                self.setUp(); self.step(0, 100)
                if latched: self.step(1, 150)
                self.assertEqual(self.step(3, 60099), 0)
                self.assertEqual(self.step(3, 60100), 2)
                self.assertEqual(self.step(3, 60101), 0)
                self.assertEqual(self.step(1, 60102), 0)
    def test_cancel_wins_at_timeout(self):
        self.step(0, 0)
        self.assertEqual(self.step(2, 60000), 4)
        self.assertEqual(self.step(3, 60000), 0)
    def test_duplicate_edges_and_custom_threshold(self):
        self.lib.muse_ptt_init(C.byref(self.s), 250, 1)
        self.assertEqual(self.step(1, 1), 0)
        self.assertEqual(self.step(0, 10), 1)
        self.assertEqual(self.step(0, 20), 0)
        self.assertEqual(self.step(1, 260), 2)
        self.assertEqual(self.step(0, 270), 1)  # also the barge-in start action
        self.assertEqual(self.step(3, 1270), 2)

    def test_debounce_is_twenty_ms_including_wake(self):
        source=(ROOT/'components/muse/muse_board.c').read_text()
        fn=source[source.index('unsigned muse_gpio_button_poll('):source.index('void muse_gpio_buttons_wait(')]
        code=r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define DEBOUNCE_SAMPLES 3
#define MUSE_BTN_TALK_PRESS 1
#define MUSE_BTN_TALK_RELEASE 2
typedef struct {bool pressed;uint8_t stable;bool debounce_wake;} muse_gpio_button_t;
static bool raw,sleeping=true;
static bool raw_pressed(const muse_gpio_button_t *b){(void)b;return raw;}
static bool muse_state_asleep(void){return sleeping;}
"""+fn+r"""
int main(void){
 muse_gpio_button_t b={.debounce_wake=true};raw=true;
 assert(!muse_gpio_button_poll(&b)); /* first sample at 0 ms */
 assert(!muse_gpio_button_poll(&b)); /* 10 ms */
 assert(muse_gpio_button_poll(&b)==1); /* 20 ms */
 raw=false;assert(!muse_gpio_button_poll(&b));assert(!muse_gpio_button_poll(&b));
 assert(muse_gpio_button_poll(&b)==2);
 b=(muse_gpio_button_t){0};raw=true;assert(muse_gpio_button_poll(&b)==1); /* other boards */
}
"""
        out=Path(self.tmp.name);(out/'debounce.c').write_text(code)
        subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=c11',str(out/'debounce.c'),'-o',str(out/'debounce')],check=True)
        subprocess.run([str(out/'debounce')],check=True)
