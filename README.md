# muse-gadget-voice

**Give a Muse ESP32 gadget a voice: a local TTS client for the firmware plus a
streaming Kokoro server. First sound in about a second.**

A Muse gadget running the stock Gadget SDK firmware *shows* its replies as
text — spoken replies were left by the SDK as a bring-your-own-TTS hook
(`start_tts()`). This repo fills that hook in:

- **Firmware:** a small original TTS client (`muse_tts.c/.h`) that POSTs each
  reply's text to a speech endpoint on your own network and streams the MP3
  back into the decoder path the firmware already has — plus the wiring to
  hook it up, and an optional patch that unlocks the speaker chip's unused
  volume headroom.
- **Server:** a ~200-line Kokoro TTS server (Python, ONNX) that answers with
  an OpenAI-style `/v1/audio/speech` API and **streams the reply sentence by
  sentence**, so the gadget starts talking after the first few words instead
  of waiting for the whole reply to be synthesized.

Built and measured on a Waveshare ESP32-S3-Touch-AMOLED-1.75C running
[facebookincubator/muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk)
(ESP-IDF v6.0.3), with the server on a Mac on the same LAN.

## How we got here (the short story)

- **The gadget was silent because it ran the wrong firmware variant.** A web
  flasher had installed the stock *non-C* 1.75 image on 1.75C hardware; that
  board variant clocks the audio codec on a pin the C hardware doesn't use,
  so the codec never woke — while every software check passed. Reflashing
  the correct local build (app-only, pairing preserved) brought the speaker
  and mic back.
- **Replies are text-only by design.** Upstream, `start_tts()` is an empty
  hook. We wrote our own client rather than fetching speech from a cloud
  endpoint, so voice works entirely on the local network.
- **Quiet was a server problem, then a chip problem.** The gadget was
  already at 100% in settings; the server was sending un-gained audio
  (fixed with a compressor/limiter chain, verified with `ffmpeg
  volumedetect`), and the stock volume curve still left the ES8311 DAC far
  below its ceiling (fixed with a custom curve — patch 02).
- **Slow was a server problem too.** The first server synthesized the
  *entire* reply before sending a single byte: 4.7 s to first sound.
  Sentence-by-sentence streaming cut that to ~1.2 s. A firmware "stream"
  flag we tried turned out to be a placebo with this server — the
  bottleneck was never in the gadget.
- **Everything was verified from the primary source** — boot logs, server
  logs, hash-checked flashes — not from "it should work."

## Setup

### 0. What you need

- A Muse gadget (ours: Waveshare ESP32-S3-Touch-AMOLED-1.75C) already
  paired and working with text replies.
- A checkout of `facebookincubator/muse-gadget-sdk` that builds for your
  board (ESP-IDF set up per the SDK's own docs).
- A Mac or Linux box on the **same LAN** as the gadget to run the server,
  with Python 3 and `ffmpeg` installed.

### 1. Flash the CORRECT board variant for your hardware

This is the trap that cost us an evening, so it goes first:

- Board variants that differ by one letter are **different hardware**.
  The 1.75 and 1.75C Waveshare boards clock the audio codec differently;
  the wrong image boots fine, passes its software checks, and stays
  silent (mic reads zeros, DAC never clocks).
- **Do not use the Muse Gadgets web flasher's "Install firmware" step** on
  a board you have already built firmware for — it installs the stock
  image and can silently replace your build with the wrong variant. The
  flasher's "send token" step is fine; the install step is the hazard.
- Prefer an **app-only flash**: writing just the app image at `0x20000`
  with esptool leaves NVS untouched, so pairing, your SDK token, and Wi-Fi
  credentials all survive — no re-pairing.

```sh
python3 -m esptool --chip esp32s3 -p /dev/cu.usbmodemXXXX -b 460800 \
  write_flash 0x20000 build-<your-build>/muse-gadget.bin
```

(Port and build-directory names differ per board/OS; see the SDK docs.)

### 2. Run the TTS server

```sh
cd server
python3 -m venv venv && ./venv/bin/pip install -r requirements.txt
# Kokoro model files (from the kokoro-onnx releases) go next to server.py:
#   kokoro-v1.0.onnx  (~325 MB)
#   voices-v1.0.bin   (~28 MB)
./venv/bin/python server.py          # listens on port 8880
curl http://localhost:8880/health    # {"status": "ok"}
```

The server is a plain stdlib HTTP service: `POST /v1/audio/speech` with
`{"model": "kokoro", "input": "text", "voice": "af_heart",
"response_format": "mp3"}` returns chunked MP3, streamed segment by
segment. A compressor + limiter chain in the encode step keeps speech loud
and full for a small speaker. On macOS, the included
`com.yourname.kokoro-tts.plist.example` shows how to run it under launchd
so it starts at login and restarts itself; on Linux, any service manager
(or a plain `nohup`) does the same job.

Note the server's LAN address (e.g. `192.168.1.50`) — the gadget needs it
in the next step, and both must be on the same network.

### 3. Apply the firmware changes

Full details in [`firmware/PATCHES.md`](firmware/PATCHES.md):

```sh
# from the root of your SDK checkout
cp /path/to/muse-gadget-voice/firmware/muse_tts.c  esp32/components/muse/
cp /path/to/muse-gadget-voice/firmware/muse_tts.h  esp32/components/muse/
git apply /path/to/muse-gadget-voice/patches/01-local-tts-wiring.patch
git apply /path/to/muse-gadget-voice/patches/02-dac-volume-curve.patch   # optional, loudness
```

Then append [`firmware/sdkconfig.tts.example`](firmware/sdkconfig.tts.example)
to your build's sdkconfig, with **your** server's address:

```ini
CONFIG_MUSE_TTS_HTTP=y
CONFIG_MUSE_TTS_URL="http://192.168.1.50:8880/v1/audio/speech"
CONFIG_MUSE_TTS_VOICE="af_heart"
CONFIG_MUSE_TTS_MODEL="kokoro"
```

### 4. Build and flash

Build the SDK the way you normally do for your board, then flash
**app-only** as in step 1. On boot, the serial log should show:

```
muse_tts: local TTS ready: POST http://192.168.1.50:8880/v1/audio/speech (model kokoro, voice af_heart)
```

### 5. Talk

Press the gadget's talk button and ask something. The reply is spoken,
starting with the first sentence while the rest is still synthesizing.
The server log shows one POST per reply from the gadget's address.

## Measured results

Same hardware, same network, typical short replies (measured with `curl`
timing against the server and confirmed in the gadget's server logs):

| | Before | After |
| --- | --- | --- |
| Time to first audio byte, 4-sentence reply | 4.74 s (first byte only after full synthesis) | **1.23 s** |
| Typical short reply, first byte | ~1.1–1.3 s | (server streams the lead segment first) |
| Typical short reply, total | — | ~1.7–1.9 s |
| Speaker top end (DAC register) | ≈ +3.5 dB of a +32 dB ceiling | ≈ +8.5 dB (patch 02) |
| Server speech level (mean) | −23.0 dB | −13.5 dB (compressor chain) |

## Troubleshooting

- **Gadget totally silent, mic self-test reads zero / −100 dBFS** — wrong
  board-variant firmware (step 1). Check the boot log's board line against
  your actual hardware, character for character.
- **Replies still text-only** — the local fetch is failing and the firmware
  fell back. Look for `muse_tts: local TTS ready` in the boot log; check
  the URL in sdkconfig; confirm the gadget and server are on the same LAN
  and `curl` the server's `/health` from another machine on that LAN.
- **Voice works but is quiet** — settings volume is only the software top.
  Make sure the server's compressor chain is in place (it is, in the
  included `server.py`), then apply patch 02 for the DAC headroom. Judge
  the top value by ear; past a point it's distortion, not loudness.
- **Voice starts slowly** — your server is buffering. Speech latency is
  the server's time-to-first-byte: it must synthesize a *short first
  segment* and stream, not synthesize the whole reply first. A firmware
  config flag can't fix a buffering server.
- **Bluetooth headphones/AirPods** — not possible on the ESP32-S3: it has
  no Bluetooth Classic radio (BLE only), so A2DP/HFP can't be added by any
  firmware. The speaker is the audio output.
- **Lost pairing after a flash** — you flashed more than the app. Re-flash
  app-only at `0x20000`; NVS (pairing, token, Wi-Fi) lives elsewhere and
  survives app-only writes.

## Layout

```
firmware/   muse_tts.c/.h (new files), PATCHES.md, sdkconfig.tts.example
patches/    01-local-tts-wiring.patch, 02-dac-volume-curve.patch
server/     server.py, requirements.txt, launchd plist example
```

## Credits

Built by Rafath Mohammed with Pintu (a Muse agent), in one long evening,
on top of Meta's [Muse Gadget SDK](https://github.com/facebookincubator/muse-gadget-sdk).
Speech by [Kokoro-82M](https://huggingface.co/hexgrad/Kokoro-82M) via
[kokoro-onnx](https://github.com/thewh1teagle/kokoro-onnx). Field notes in
[dan1t0's write-up](https://dan1t0.com/2026/11/09/Playing-with-Meta-Muse-ESP32-S3-Part-1/)
on this same setup pointed us at the local-TTS pattern; the client and
server here are original code written for this repo.

## License

Apache-2.0 (see `LICENSE`) — the same license as the base Muse Gadget SDK.
The SDK itself is **not** included here; get it from
facebookincubator/muse-gadget-sdk. The patch files in `patches/` modify
SDK files and are shared under the same terms.
