# Firmware changes — what to patch in a stock Muse Gadget SDK checkout

Everything here applies to the `esp32/` tree of
[facebookincubator/muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk)
(Apache-2.0). Two of the pieces are **new files you copy in**; the rest are
small edits to stock files, provided as unified diffs in
[`../patches/`](../patches/) so you can read every line before applying it.

## 1. Copy in the new files (ours, Apache-2.0)

| File | Destination |
| --- | --- |
| `firmware/muse_tts.c` | `esp32/components/muse/muse_tts.c` |
| `firmware/muse_tts.h` | `esp32/components/muse/muse_tts.h` |

`muse_tts` is a small local-TTS client. The chat session hands it a reply's
text; a worker task POSTs the text as JSON to the configured speech endpoint
(an OpenAI-style `/v1/audio/speech` service — for example the Kokoro server in
[`../server/`](../server/)) and pushes the returned MP3 into a PSRAM stream
buffer as it arrives. The session drains that buffer into the **same decoder
path the stock firmware already uses** for streamed speech. Nothing blocks
the session task: a failed or unconfigured service simply makes the start
call fail, and the session falls back to its original behaviour.

## 2. Apply `patches/01-local-tts-wiring.patch`

From the root of your SDK checkout:

```sh
git apply patches/01-local-tts-wiring.patch
# or: patch -p1 < patches/01-local-tts-wiring.patch
```

It changes four files, all inside `esp32/components/muse/`:

- **`Kconfig`** — adds the `MUSE_TTS_*` option block:
  `MUSE_TTS_HTTP` (master switch), `MUSE_TTS_URL`, `MUSE_TTS_VOICE`
  (default `af_heart`), `MUSE_TTS_MODEL` (default `kokoro`),
  `MUSE_TTS_LANG_CODE` (optional), and `MUSE_TTS_STREAM` (only for
  third-party providers that require `"stream": true` in the request —
  the included server streams on its own and does not need it).
- **`CMakeLists.txt`** — compiles `muse_tts.c` when
  `CONFIG_MUSE_TTS_HTTP` is set (both the gadget and Voice PE source lists).
- **`muse_chat_priv.h`** — adds `MUSE_VOICE_NOTE_INSTRUCTION`: a plain-text
  rule ("never answer in markdown format or with emojis…") that travels in
  the `message` field of every voice note, so spoken replies don't read
  markup aloud.
- **`muse_chat_session.cpp`** — the actual hook-up:
  - `start_tts()` now tries the local service first:
    `muse_tts_start(reply text)`. If that returns 0 (not configured, busy,
    or failed), the stock path runs unchanged — Muse's own TTS stream,
    then reading-pace text.
  - A new `pump_local_tts()` runs each session-loop pass and moves MP3 from
    the worker's buffer into the decoder with the same backpressure as the
    stream path. On failure *before any audio arrived* it falls back to
    Muse's speech stream for that message, then to text.
  - Cancels any in-flight local fetch when a turn resets or a new voice
    note arrives, so stale speech never plays over a new reply.

## 3. Apply `patches/02-dac-volume-curve.patch` (optional — loudness)

From the root of your SDK checkout:

```sh
git apply patches/02-dac-volume-curve.patch
```

One hunk in `esp32/components/muse/muse_audio.c`, right after the speaker
codec opens: it installs a custom volume curve with
`esp_codec_dev_set_vol_curve()`. The stock curve maps the settings volume
0–100 to −50…0 dB *requested*, which leaves the ES8311 DAC register at
roughly +3.5 dB of its +32 dB ceiling — the gadget can be at "100%" and
still be quiet. The patch keeps the same shape (volume 0 still hard-mutes,
which esp_codec_dev handles internally) but lets the top of the range ask
for **+5 dB**, landing the DAC near **+8.5 dB**. If the call fails, the
stock curve is kept and a warning is logged. Size the top value by ear:
with already-hot server audio, pushing much past +5 dB requested mostly
buys distortion from the small fixed-gain amp, not loudness.

## 4. Configure, build, flash

Append [`sdkconfig.tts.example`](sdkconfig.tts.example) to your build's
sdkconfig with your server's LAN address, then build and flash as usual —
but flash **app-only** (see the README's warning about the web flasher):
an app-only write at `0x20000` preserves NVS, so your pairing, SDK token,
and Wi-Fi survive.
