/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Local HTTP speech for chat replies (CONFIG_MUSE_TTS_HTTP).
 *
 * The chat session hands muse_tts_start() a reply's text; a worker task
 * POSTs it as JSON to the configured speech endpoint (an OpenAI-style
 * /v1/audio/speech service, for example a Kokoro server on the LAN) and
 * buffers the MP3 that comes back. The session drains the buffer with
 * muse_tts_read() and feeds the bytes to the same decoder path the
 * server speech stream uses. Nothing here blocks the session task:
 * start and read return immediately, and a failed or unconfigured
 * service simply makes start fail, which the session falls back from.
 */

typedef enum {
    MUSE_TTS_PENDING = 0,   /* fetch in flight: more audio may still come */
    MUSE_TTS_END,           /* fetch finished; the last bytes are draining */
    MUSE_TTS_ERROR,         /* fetch failed or was cancelled */
} muse_tts_status_t;

/* Prepares the worker and its buffer. Safe to call once at session start;
 * if it cannot set up, start simply fails afterwards. */
void muse_tts_init(void);

/*
 * Starts fetching speech for `text` (the reply's full text). Returns a
 * nonzero request id, or 0 when no local service is configured, one is
 * already running, or the text is empty. Only one fetch runs at a time.
 */
uint32_t muse_tts_start(const char *text);

/*
 * Copies up to `len` bytes of the request's MP3 into `dst` and reports
 * how the fetch stands in `status`. Returns the byte count, which may
 * be 0 with MUSE_TTS_PENDING while the worker waits on the network.
 * Once the status is MUSE_TTS_END or MUSE_TTS_ERROR the request is
 * over; the session stops reading it.
 */
size_t muse_tts_read(uint32_t id, uint8_t *dst, size_t len, muse_tts_status_t *status);

/* Abandons the fetch in flight, if any. The worker stops at its next
 * checkpoint; a later start waits for it to wind down before failing. */
void muse_tts_cancel(void);
