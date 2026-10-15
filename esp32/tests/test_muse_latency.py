# SPDX-License-Identifier: Apache-2.0
"""Real timing ring: stale-job isolation, last ten, grouped medians."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class Latency(unittest.TestCase):
    def test_history_and_grouped_medians(self):
        with tempfile.TemporaryDirectory() as tmp:
            out=Path(tmp);(out/'freertos').mkdir()
            (out/'sdkconfig.h').write_text('#define CONFIG_MUSE_LATENCY 1\n')
            (out/'esp_log.h').write_text('#include <stdio.h>\n#define ESP_LOGI(tag,fmt,...) printf(fmt "\\n", __VA_ARGS__)\n')
            (out/'esp_timer.h').write_text('#include <stdint.h>\nint64_t esp_timer_get_time(void);\n')
            (out/'freertos/FreeRTOS.h').write_text('typedef int portMUX_TYPE;\n#define portMUX_INITIALIZER_UNLOCKED 0\n#define portENTER_CRITICAL(x) ((void)(x))\n#define portEXIT_CRITICAL(x) ((void)(x))\n')
            (out/'main.c').write_text(r'''
#include <assert.h>
#include <stdio.h>
#include "muse_latency.h"
static int64_t now=1000000;
int64_t esp_timer_get_time(void){return now;}
int main(void){
 for(int i=1;i<=12;i++){
  uint32_t id=muse_latency_begin(false,i<=6);
  assert(id==(uint32_t)i);
  muse_latency_at(id,MUSE_LAT_MIC,now+i*1000);
  muse_latency_at(id,MUSE_LAT_MIC,now+999000); /* first stamp wins */
  muse_latency_end(id);muse_latency_end(id);now+=1000000;
 }
 muse_latency_at(1,MUSE_LAT_MIC,999999999); /* recycled ring slot: ignored */
 puts("HISTORY");muse_latency_print();
}
''')
            exe=out/'test'
            subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-I'+str(out),
                '-I'+str(ROOT/'components/muse'),str(out/'main.c'),str(ROOT/'components/muse/muse_latency.c'),'-o',str(exe)],check=True)
            text=subprocess.check_output([str(exe)],text=True)
            initial,history=text.split('HISTORY\n')
            self.assertEqual(initial.count('LAT turn='),12)
            self.assertEqual(history.count('LAT turn='),10)
            self.assertNotIn('LAT turn=1 ',history)
            self.assertIn('median mode=muse power=usb mic_ms=4 n=4',history)
            self.assertIn('median mode=muse power=battery mic_ms=9 n=6',history)
