#!/usr/bin/env python3
"""Minimal OpenAI-compatible Kokoro TTS server for the Muse gadget.

POST /v1/audio/speech {model, input, voice, response_format:"mp3"} -> MP3
GET  /health -> {"status": "ok"}

Kokoro (ONNX) synthesizes 24 kHz mono WAV; ffmpeg encodes it to MP3, which
is what the gadget's firmware accepts (Content-Type: audio/mpeg,
streamed with chunked transfer encoding — a short opening segment
first, then one sentence at a time — so the gadget starts playing
after only a few words while the rest are still being synthesized).
"""
import io
import json
import os
import re
import subprocess
import sys
import threading
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import numpy as np
from kokoro_onnx import Kokoro

HERE = os.path.dirname(os.path.abspath(__file__))
MODEL = os.path.join(HERE, "kokoro-v1.0.onnx")
VOICES = os.path.join(HERE, "voices-v1.0.bin")
# ffmpeg location: $FFMPEG, else the Apple Silicon Homebrew path when
# present, else plain "ffmpeg" from PATH (Linux, Intel Macs).
FFMPEG = os.environ.get("FFMPEG") or (
    "/opt/homebrew/bin/ffmpeg"
    if os.path.exists("/opt/homebrew/bin/ffmpeg") else "ffmpeg")
PORT = 8880

print("loading Kokoro model...", flush=True)
kokoro = Kokoro(MODEL, VOICES)
_lock = threading.Lock()
print("Kokoro ready on port %d" % PORT, flush=True)


def synth_segment_mp3(text, voice):
    with _lock:
        samples, sr = kokoro.create(text, voice=voice or "af_heart", speed=1.0, lang="en-us")
    # Maximize loudness for the gadget's small speaker: Kokoro's raw output
    # is quiet and peaks were already at the ceiling, so the remaining
    # loudness is in the average level. Soft-knee compress with tanh
    # (squashes peaks relative to the body of the speech), then raise RMS
    # toward -10 dBFS, capped so the peak never exceeds -0.3 dBFS (0.97).
    # The ffmpeg alimiter in the encode step is a final safety net.
    s = np.asarray(samples, dtype=np.float64)
    if s.size:
        peak = float(np.max(np.abs(s)))
        rms = float(np.sqrt(np.mean(s * s)))
        if peak > 0.0 and rms > 0.0:
            drive = 3.0
            s = np.tanh(drive * s) / np.tanh(drive * peak) * peak
            peak = float(np.max(np.abs(s)))
            rms = float(np.sqrt(np.mean(s * s)))
            gain = min(0.97 / peak, 0.30 / rms)
            s = s * gain
            sys.stderr.write("synth louder gain x%.2f (peak %.3f rms %.4f)\n" % (gain, peak, rms))
    pcm = (np.clip(s, -1.0, 1.0) * 32767).astype("<i2")
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(pcm.tobytes())
    p = subprocess.run(
        [FFMPEG, "-v", "error", "-f", "wav", "-i", "pipe:0",
         "-af", "alimiter=limit=0.95",
         "-codec:a", "libmp3lame", "-b:a", "64k", "-f", "mp3", "pipe:1"],
        input=buf.getvalue(), capture_output=True)
    if p.returncode != 0:
        raise RuntimeError("ffmpeg failed: " + p.stderr.decode()[:200])
    return p.stdout


def _split_lead(sentence):
    """Split the opening sentence into a short lead + the remainder.

    The lead ends at the earliest natural boundary — a comma or other
    clause punctuation — or, for a long opening without one, at a word
    boundary near 50 characters, so the first audio chunk is
    synthesized after only a few words. Openings with no usable
    boundary are returned whole.
    """
    cut = None
    for m in re.finditer(r"[,;:] | — | – ", sentence[:72]):
        if m.start() >= 8:
            cut = m.end() - 1  # keep the punctuation, drop the space
            break
    if cut is None and len(sentence) > 60:
        space = sentence.rfind(" ", 20, 58)
        if space > 0:
            cut = space
    if cut is None:
        return sentence, ""
    lead = sentence[:cut].strip()
    rest = sentence[cut:].strip()
    if not lead or not rest:
        return sentence, ""
    return lead, rest


def split_sentences(text):
    """Split text into segments for streaming synthesis.

    The first segment is deliberately short (see _split_lead) so the
    first audio leaves after only a few words; the remainder is split
    on ., !, ? boundaries, with fragments shorter than 25 characters
    merged into a neighbour so tiny pieces don't each pay a full
    synthesis + encode round trip on their own. The first segment is
    still synthesized before the response headers, so a total
    synthesis failure reports as a clean 500, exactly as before.
    """
    parts = [p.strip() for p in re.split(r"(?<=[.!?])\s+", text)]
    parts = [p for p in parts if p]
    if not parts:
        return [text]
    lead, rest = _split_lead(parts[0])
    tail = ([rest] if rest else []) + parts[1:]
    segments = []
    for p in tail:
        if segments and len(segments[-1]) < 25:
            segments[-1] += " " + p
        else:
            segments.append(p)
    if len(segments) > 1 and len(segments[-1]) < 25:
        segments[-2] += " " + segments.pop()
    return [lead] + segments


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def do_GET(self):
        if self.path == "/health":
            body = b'{"status": "ok"}'
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_error(404)

    def do_POST(self):
        if self.path != "/v1/audio/speech":
            self.send_error(404)
            return
        try:
            n = int(self.headers.get("Content-Length", "0"))
            req = json.loads(self.rfile.read(n) or b"{}")
            text = (req.get("input") or "").strip()
            voice = req.get("voice") or "af_heart"
            if not text:
                self.send_error(400, "empty input")
                return
            segments = split_sentences(text)
            # Synthesize the first sentence before sending headers, so
            # a total synthesis failure still reports as a clean 500.
            first = synth_segment_mp3(segments[0], voice)
        except Exception as e:  # noqa: BLE001 - report any failure as a 500
            sys.stderr.write("synth error: %r\n" % (e,))
            self.send_error(500, "synthesis failed")
            return
        # Stream the MP3 sentence by sentence with chunked transfer
        # encoding (no Content-Length): the first sentence's audio
        # leaves immediately, while the rest are still synthesizing.
        self.send_response(200)
        self.send_header("Content-Type", "audio/mpeg")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        try:
            self._write_chunk(first)
            for seg in segments[1:]:
                self._write_chunk(synth_segment_mp3(seg, voice))
        except Exception as e:  # noqa: BLE001 - headers are already sent
            sys.stderr.write("synth error mid-stream: %r\n" % (e,))
        finally:
            try:
                self.wfile.write(b"0\r\n\r\n")
                self.wfile.flush()
            except Exception:  # noqa: BLE001 - connection may be gone
                pass

    def _write_chunk(self, data):
        if not data:
            return
        self.wfile.write(b"%x\r\n" % len(data))
        self.wfile.write(data)
        self.wfile.write(b"\r\n")
        self.wfile.flush()


if __name__ == "__main__":
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
