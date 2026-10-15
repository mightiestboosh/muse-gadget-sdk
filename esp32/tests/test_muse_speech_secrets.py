# SPDX-License-Identifier: Apache-2.0
"""Credential guards across NVS, command dispatch and IDF request logging."""
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[1]
class SpeechSecrets(unittest.TestCase):
    def test_transport_header_dumps_are_compiled_out(self):
        cmake=(ROOT/'components/muse/CMakeLists.txt').read_text()
        self.assertIn('foreach(secret_transport esp_http_client tcp_transport)',cmake)
        self.assertIn('PRIVATE LOG_LOCAL_LEVEL=ESP_LOG_INFO',cmake)
        for file in ['muse_tts.c','muse_stt.c']:
            source=(ROOT/'components/muse'/file).read_text()
            self.assertNotIn('HTTP %d %.*s',source)
    def test_key_storage_is_encryption_gated_and_committed_before_publish(self):
        source=(ROOT/'components/muse/muse_settings.c').read_text()
        setter=source[source.index('esp_err_t muse_settings_set_tts_key('):source.index('void muse_settings_set_tts_voice(')]
        self.assertIn('#if !CONFIG_HOMEHUB_NVS_ENCRYPTION',setter)
        self.assertIn('return ESP_ERR_NOT_SUPPORTED',setter)
        self.assertLess(setter.index('nvs_commit'),setter.index('strlcpy(s.tts_key'))
        ble=(ROOT/'components/muse/muse_ble.c').read_text()
        self.assertIn('!strcmp(cmd, "tts.key")',ble[ble.index('bool secret ='):])
        overlay=(ROOT/'devices/sdkconfig.muse-m5stack-sticks3').read_text()
        self.assertIn('CONFIG_HOMEHUB_NVS_ENCRYPTION=y',overlay)
        self.assertNotIn('CONFIG_SECURE_BOOT=y',overlay)
