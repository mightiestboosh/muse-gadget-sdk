# StickS3 voice and Bluetooth keyboard

This branch adds ElevenLabs reply speech, hold/tap recording, and an opt-in
US-layout BLE dictation keyboard. **USB bench installation, audio playback,
encrypted storage and Keyboard setting persistence have been checked on a
StickS3. Cloud speech, dictation and keyboard host acceptance remain pending.**

## Build and initial setup

Read [the ESP32 instructions](../AGENTS.md) and [StickS3 hardware notes](../devices/README.md)
first. The base is upstream `f03f63f` on `main`. The two TTS commits were
cherry-picked with authorship and source commit references intact:
`187a570c9ed193bae62657adc49bc4a72d8de56e` and
`b23f61e28fcbec38016bc442ff0236f3f7dbf320`.

```sh
. ~/esp/esp-idf-v6/export.sh       # ESP-IDF v6.0.1 exactly
cd esp32                        # from the repository root
# Start with a fresh build-muse-m5stack-sticks3 directory after config changes.
tools/muse/board.sh build sticks3
idf.py -B build-muse-m5stack-sticks3 menuconfig
# ESP32 Device SDK > Muse Gadgets SDK token: set locally, never commit it.
tools/muse/board.sh build sticks3
tools/muse/board.sh flash sticks3 PORT
python tools/muse/monitor.py PORT 30
```

The helper alias is `sticks3`, its overlay is
`devices/sdkconfig.muse-m5stack-sticks3`, and its output is
`build-muse-m5stack-sticks3/muse-gadget.bin`. A token-free stock build was
completed before editing firmware; its image and log are retained locally in
`build-stock-sticks3/`. The physical baseline (pair, hold, see a reply and save
the serial log) remains to be performed before comparing modified hardware.
The user initially authorized build-only work, then supplied a StickS3 and
authorized installation and device testing.

The board profile enables HMAC-encrypted NVS with eFuse key slot 0. Before the
first encrypted boot, inspect the actual device's eFuse summary: slot 0 must
already hold the intended HMAC_UP key or be available for NVS provisioning.
Choose another available slot in menuconfig if necessary, then rebuild from
fresh configuration. Upstream provisions the HMAC key at first boot and fails
closed if it cannot initialize encryption. Secure Boot and flash encryption
remain disabled. Back up factory flash before replacing UiFlow2. An existing
plaintext NVS installation may need explicit NVS migration/recovery and
re-pairing; never erase identity or credentials automatically to recover it.

Set speech credentials over the USB serial console:

```text
>tts.key=YOUR_ELEVENLABS_KEY
>tts.voice=YOUR_ENGLISH_VOICE_ID
```

These same command names are available in the authenticated Muse BLE settings
service and `tools/muse/ble_setup.html`. The key setter refuses unencrypted
builds, invalid header characters, truncation, and NVS write/commit errors. The
key is in the encrypted `muse` namespace as `tts_key`; status reports only
whether a key exists. It is never compiled into an image, echoed, or shown on
screen. Remote HTTP error bodies are not logged. IDF HTTP/WebSocket header dumps are
compiled out even if runtime debug logging is later enabled. An empty key disables speech
and dictation. The initial voice is Rachel (`21m00Tcm4TlvDq8ikWAM`); replace it
with the user's chosen voice. Model: `eleven_flash_v2_5`, MP3: `mp3_22050_32`.

## Controls and indicators

The vendor and board driver agree: KEY1/front is GPIO11, KEY2/side is GPIO12.
The board's three 10 ms samples accept an awake edge 20 ms after its first
sample. StickS3 applies the same debounce to wake presses; other boards keep
their existing fast-wake behavior.

| Gesture | Action |
| --- | --- |
| Front down | Start capture immediately, including when waking |
| Release at or above 400 ms | Send the recording |
| Release below 400 ms | Continue recording; caption says `TAP TO SEND` with elapsed time |
| Next front down in tap mode | Send immediately; its release does nothing |
| Side during recording | Discard without sending, consume this menu press |
| 60 seconds from first press | Auto-send; ignore the held key until released |
| Front during a spoken reply | Gate the amp, invalidate the TTS job, capture a new turn |
| Side while typing | Cancel queued typing and release held modifiers/keys |

Thresholds are `CONFIG_MUSE_PTT_TAP_MS` and `CONFIG_MUSE_PTT_MAX_S`. Stop cues
are off unless `CONFIG_MUSE_PTT_CUES` is enabled. No cue delays capture start.
The existing ES8311 ADC, filter and pre-roll path is retained; the ADC remains
on while the screen is awake. On battery with the screen asleep, upstream
codec rest/light sleep remains enabled.

In the side menu, **Keyboard: On/Off** selects dictation or normal Muse voice
chat. It persists in NVS as namespace `kbd`, key `enabled` (`kbd.enabled`).
**Forget hosts** removes just the two recorded keyboard host identities and
their NimBLE bonds. Turning Keyboard off disconnects and stops HID advertising
but preserves those bonds. Turning it on attempts a five-second directed
reconnect to the last host, then advertises normally for either host.

The keyboard glyph is dark when off, blinks while advertising, is dim while
waiting for Muse setup to finish, and turns purple on an encrypted connection.
The host's GATT Device Name is shown for four seconds when readable; hosts that
do not expose it show `KEYBOARD CONNECTED`. Host display names are not logged.

## BLE coexistence decision

**One legacy advertising owner at a time; no extended advertising.** HID is
advertised only after Muse app setup is complete and with Phone setup off.
Phone setup or unpaired Muse setup takes priority and disconnects HID. Turn
Phone setup off again to resume keyboard advertising. The menu Keyboard
setting stays on throughout. Muse chat itself uses Wi-Fi and remains available
with the Keyboard setting either on or off (front-button input is dictation
while Keyboard is on). To ask Muse, switch Keyboard off.

The upstream audit before HID implementation found:

| Source | BLE ownership |
| --- | --- |
| `main/ble_server.c` | Initializes NimBLE; sole legacy advertiser; setup and persistent companion services. Start, stop, interval change, disconnect and sync all converge on `start_advertising()`. |
| `components/muse/muse_ble.c` | Companion GATT settings and passkey callbacks; no separate advertiser. |
| `main/muse_glue.c`, `main/app.c` | Setup-complete, runtime companion and BLE command policies call that server. |
| `main/ble_basic.c` | Alternative companion for builds without the full UI. |
| `main/ble_client.c`, `main/ble_central.c` | Outbound central connections; neither advertises a peripheral. |

Two optional companion hooks register an advertising override and shutdown
handler. HID registers its services before GATT starts, shares the existing
NimBLE host and NVS bond store, and owns a separate GAP callback/connection
handle. It never overwrites the Muse setup connection handle. Muse phone
settings keep their passkey/MITM policy. HID uses Secure Connections, bonding,
and Just Works; reads/writes and typing require encryption and a bond.

The profile allows two connections for safe handoff, four total stored bonds
(two keyboard identities plus room for Muse settings), and accepts at most
two distinct keyboard hosts. It rejects a third before pairing instead of
evicting another host. The keyboard name is `StickS3 Keys`, appearance
`0x03C1`. Services: HID `0x1812` (report and boot keyboard, LED output), Battery
`0x180F` (M5PM1 percentage), Device Information `0x180A` (development PnP ID,
Bluetooth vendor source `0xFFFF`, not an assigned production vendor ID).
Advertising is 1000–1020 ms; requested connection interval is 30–50 ms,
peripheral latency 4, supervision timeout 6 s. Hosts can negotiate other values.
GATT client support is enabled to read the host name; it does not initiate
outbound BLE connections for the keyboard.

The official WebSocket client is pinned to Espressif commit
`5c4650d0ab6f9fde165ae1e3c1ef16714be0a619`. Its PSRAM structure/task-stack options
are needed here and are absent from the registry's older 1.8.0 package.
All board memory/sleep settings otherwise remain as upstream: octal PSRAM at
80 MHz, code/rodata and radio allocations in PSRAM, battery light sleep, and
no ignoring a missing PSRAM chip. New tasks block without PM locks when idle.
TLS warm-ups close their sockets and retain session tickets, not live idle
connections. The speaker gate enables M5PM1 GPIO3 only for playback, drains
the final DMA frames before normal shutdown, and mutes immediately on barge-in.
USB VIN is read before applying playback volume; missing USB or a failed
power read caps it at 75%.

## Dictation and host setup

Capture feeds a 256 KiB PSRAM backlog while the STT task connects/resumes TLS.
It sends 200 ms chunks of 16 kHz mono s16le PCM, base64 encoded, using
`scribe_v2_realtime`, `audio_format=pcm_16000`, `commit_strategy=manual`.
Release/second press commits the final chunk, waits for `committed_transcript`,
then closes. Partial transcripts are ignored. Overflow, timeout, missing key,
authentication failure or transport failure rejects the whole dictation;
nothing from an unsuccessful request is queued to type.

Typing converts printable ASCII to a US-layout HID usage plus Shift. Curly
quotes, dashes and ellipses get ASCII stand-ins; other Unicode scalars are
dropped with a count/size diagnostic. Newlines/tabs become spaces. Each result
gets exactly one trailing space and never Enter. Reports retain their queue
position on notification allocation/backpressure failures, with 10 ms between
key edges. Side cancel sends a release-all report. A lost host connection
cancels remaining text rather than typing it into a different host later.

- **iPhone:** Settings > Bluetooth > StickS3 Keys. Set Settings > General >
  Keyboard > Hardware Keyboard to U.S. iOS hides its on-screen keyboard while
  a hardware keyboard is connected.
- **Mac:** System Settings > Bluetooth > StickS3 Keys. Choose ANSI if Keyboard
  Setup Assistant appears. Use a U.S. input source.
- Turn **Phone setup off** on StickS3 before looking for the HID name.
- If the report map changes, forget the device on the host and pair again.
- Test an iPhone hotspot with Maximize Compatibility enabled.

## Timing and battery measurements

`>latency` prints the last ten turns and medians grouped by input mode and
USB/battery. Each completed turn also emits one `LAT` log line. Times `t0..t9`
are monotonic microseconds from `esp_timer_get_time()`:

| Field | Software observation |
| --- | --- |
| t0 | Accepted front press / start of turn |
| t1 | First mic read completion while recording (includes preserved pre-roll) |
| t2 | Send gesture / timeout |
| t3 | First Muse reply text |
| t4 | First ElevenLabs MP3 byte |
| t5 | First reply PCM handed toward the speaker, only when TTS returned audio |
| t6 | Barge-in press |
| t7 | Amplifier-disable call returned |
| t8 | Committed dictation transcript |
| t9 | First HID key-down queued successfully |

Missing metrics are `-1`, never zero measurements. Microphone DMA availability,
first PCM submission, and amp-disable return are software proxies; a real
microphone/speaker capture or logic analyzer must establish the first newly
sampled mic frame, first audible word and physical silence. Captions-only
turns do not claim an audible response. STT/HID jobs carry a turn ID so stale
jobs cannot contaminate a replacement turn's timestamps.

**End-to-end latency and battery measurements remain pending.** Two offline
USB bench presses produced mic-read proxies of 14 ms and 9 ms; these were
missing-key dictation attempts, not completed turns or measurements of a newly
sampled frame. They do not establish the latency targets below. Run at
least ten turns per mode in each row, in a separate log per condition; do not
mix Wi-Fi environments in a median. Record `>power` before/after idle periods
with Keyboard off, advertising, and connected (screen off) to measure the
keyboard's incremental drain and confirm light sleep. Use an approved hardware
fixture and the repository's [power measurement procedure](power/README.md)
for current measurements. Do not invent mA figures from software state.

| Condition | Muse sound latency | Dictation release → first key | Keyboard idle battery cost |
| --- | --- | --- | --- |
| USB / home Wi-Fi, ≥10 turns | Pending hardware | Pending hardware | Pending hardware |
| Battery / home Wi-Fi, ≥10 turns | Pending hardware | Pending hardware | Pending hardware |
| USB / compatible iPhone hotspot, ≥10 turns | Pending hardware | Pending hardware | Pending hardware |
| Battery / compatible iPhone hotspot, ≥10 turns | Pending hardware | Pending hardware | Pending hardware |

Targets: mic ≤50 ms; resumed text→MP3 ≤400 ms; cold ≤900 ms; MP3→sound ≤150 ms;
text→sound ≤700 ms; barge-in→silence ≤100 ms; dictation send→first key ≤800 ms.
Button release→Muse first text is recorded, not optimized. The brief's PR
measurements are reference data from another board, not StickS3 results.
Raw PCM TTS and WebSocket text streaming experiments are deferred until the
HTTP/MP3 path has real hardware measurements.

## Validation checklist

Verified on macOS, 2026-10-15, with ESP-IDF `v6.0.1` (clean tag,
`8c19b156084a0753687347cca1f5355782893533`):

| Check | Result |
| --- | --- |
| Stock StickS3, before firmware edits | Pass; `0x211000` image, 47% app slot free |
| Feature StickS3, fresh build directory via `tools/muse/board.sh build sticks3` | Pass |
| Final StickS3 source build | Pass; `0x221000` image, 45% app slot free |
| AIPI full-UI compatibility build | Pass; 48% app slot free |
| Default `idf.py build` (ESP32-C5) | Pass; 19% app slot free |
| Full host suite | **612 tests pass, no skips** |
| Every StickS3 board-overlay setting applied | Pass |
| HTTP/WebSocket header-dump strings absent from StickS3 image | Pass |
| StickS3 USB installation and reboot | Pass; no panic or unexpected reboot in captured runs |
| ES8311 microphone self-test | Pass; live left-channel samples, about 16.14 kHz capture |
| Built-in MP3 playback through onboard speaker | Pass; user confirmed sound and display |
| HMAC-encrypted NVS first boot and reboot | Pass; slot 0 was empty, provisioned successfully, reused on reboot |
| Runtime Keyboard toggle survives reboot | Pass; dictation mode observed after restart, restored Off afterward |
| Muse BLE setup handshake | Reached device confirmation; account/Wi-Fi setup still pending |
| Cloud speech, dictation, host pairing and battery acceptance | Pending credentials/setup and further hardware tests |

Local verification logs are retained in `build-validation-sticks3/`; they
contain no configured SDK token or ElevenLabs key. The test environment
includes host Mbed TLS 3 and the pinned optional `ppk2-api==0.9.2`, used only
for its synthetic host test (no hardware opened). On this Mac, select host
crypto with `PKG_CONFIG_PATH=/opt/homebrew/opt/mbedtls@3/lib/pkgconfig` when
running tests. Re-run:

```sh
. ~/esp/esp-idf-v6/export.sh
# board.sh removes managed_components after building. A direct idf.py build
# with the same -B/target/defaults rehydrates it for the existing host tests.
python3 -m unittest discover -s tests -p 'test_*.py'
```

New host harnesses exercise gesture transitions, sentence boundaries and first
request batching, all ASCII mappings and Unicode normalization, 500-character
typing with simulated notification pressure/cancel, STT serialization and
committed-only parsing, battery volume policy, and the timing ring/medians.

USB bench logs and the private 8 MB factory UiFlow2 backup are retained locally
in ignored `build-hardware-sticks3/`; never commit the backup, which may hold
factory credentials. The factory partition table placed NVS at `0x9000`,
whereas Muse uses `0x11000..0x1cfff`. Only the new Muse NVS range, previously
factory application code, was initialized before flashing. Existing factory
NVS was not erased. No Secure Boot or flash encryption was enabled. The full
612-test host suite passed again before installation.

Still unchecked on hardware:

- [ ] Stock pairing, hold-to-talk and serial baseline.
- [ ] Hold, tap, side cancel and 60 s auto-send on actual buttons.
- [ ] Gapless multi-sentence speech, synchronized captions and barge-in timing.
- [ ] Missing key, HTTP 401 and no-network caption fallback ends normally.
- [ ] Battery playback stays capped without rebooting; no pops or idle amp drain.
- [ ] iPhone and Mac pairing, reconnect, reboot persistence and forgetting hosts.
- [ ] Muse pairing/voice chat coexistence with Keyboard on and off.
- [ ] 500 characters into Notes without loss, reordering or Enter.
- [ ] Home Wi-Fi and compatible iPhone hotspot, on USB and battery.
- [ ] At least ten turns per test condition; latency gaps reported with numbers.
- [ ] Keyboard advertising/connected battery cost and actual light sleep measured.
- [x] Encrypted NVS first boot and reboot verified.
- [ ] ElevenLabs key configured and verified absent from serial logs during live requests.

## References

- [Original TTS PR and board-specific reference measurements](https://github.com/longzhi/muse-gadget-sdk/pull/2)
- [ElevenLabs streaming TTS](https://elevenlabs.io/docs/api-reference/text-to-speech/stream)
- [Scribe Realtime protocol](https://elevenlabs.io/docs/api-reference/speech-to-text/v-1-speech-to-text-realtime)
- [Manual commits](https://elevenlabs.io/docs/eleven-api/guides/how-to/speech-to-text/realtime/transcripts-and-commit-strategies)
- [M5Stack StickS3](https://docs.m5stack.com/en/core/StickS3)
- Vendor code checked from `m5stack/M5Unified`: `src/M5Unified.inl` (StickS3
  button pins and PA GPIO3 setup), `src/utility/Power_Class.inl` and
  `M5PM1_Class` (power); `m5stack/M5GFX/src/M5GFX.cpp` (panel).
