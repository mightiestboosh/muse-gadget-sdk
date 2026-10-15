# SPDX-License-Identifier: Apache-2.0
"""US layout mapping and safe Unicode normalization; real C module."""
import ctypes as C
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
class HidText(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(); cls.addClassCleanup(cls.tmp.cleanup)
        lib=Path(cls.tmp.name)/'hid.so'
        subprocess.run([*shlex.split(os.environ.get('CC','cc')), '-std=c11','-shared','-fPIC',
                        str(ROOT/'components/muse/muse_hid_text.c'),'-o',str(lib)],check=True)
        cls.lib=C.CDLL(str(lib))
        cls.lib.muse_hid_ascii.argtypes=[C.c_uint8,C.POINTER(C.c_uint8)]
        cls.lib.muse_hid_ascii.restype=C.c_bool
        cls.lib.muse_hid_normalize.argtypes=[C.c_char_p,C.c_size_t,C.c_char_p,C.POINTER(C.c_size_t)]
        cls.lib.muse_hid_normalize.restype=C.c_size_t
    def test_all_printable_ascii_and_shift(self):
        reports=set()
        for c in range(32,127):
            r=(C.c_uint8*8)()
            self.assertTrue(self.lib.muse_hid_ascii(c,r),chr(c))
            self.assertNotIn(r[2],[0,40])  # no empty key or Enter
            self.assertEqual(list(r)[3:],[0]*5)
            reports.add(bytes(r))
        self.assertEqual(len(reports),95)
        for c,modifier,usage in [('a',0,4),('A',2,4),('!',2,30),('0',0,39),(')',2,39),(' ',0,44),('?',2,56)]:
            r=(C.c_uint8*8)(); self.lib.muse_hid_ascii(ord(c),r)
            self.assertEqual((r[0],r[2]),(modifier,usage))
    def test_controls_never_become_enter(self):
        for c in list(range(32))+list(range(127,256)):
            r=(C.c_uint8*8)(); self.assertFalse(self.lib.muse_hid_ascii(c,r))
    def normalize(self,data):
        out=b''
        while data:
            repl=C.create_string_buffer(3); used=C.c_size_t()
            n=self.lib.muse_hid_normalize(data,len(data),repl,C.byref(used))
            self.assertGreater(used.value,0)
            out+=repl.raw[:n]; data=data[used.value:]
        return out
    def test_punctuation_and_unsupported_scalars(self):
        self.assertEqual(self.normalize('“Hi”—it’s…\n中文🙂'.encode()),b'"Hi"-it\'s... ')
        self.assertEqual(self.normalize(b'\xe2\x80'),b'')
        self.assertEqual(self.normalize(b'\xffa'),b'a')
