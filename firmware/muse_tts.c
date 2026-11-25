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

/*
 * Local HTTP speech for chat replies: the worker half of muse_tts.h.
 *
 * One fetch at a time. muse_tts_start() copies the reply's text and
 * wakes the worker; the worker POSTs the text as JSON to the speech
 * endpoint (CONFIG_MUSE_TTS_URL, an OpenAI-style /v1/audio/speech
 * service) and pushes the MP3 response into a stream buffer in PSRAM
 * as it lands, which is the backpressure: a full buffer stalls the
 * download until the session drains some. muse_tts_read() is the
 * session's non-blocking drain of that buffer, and muse_tts_cancel()
 * flags the worker to abandon the fetch at its next checkpoint.
 */

#include "muse_tts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "muse_mem.h"
#include "sdkconfig.h"

#ifndef CONFIG_MUSE_TTS_URL
#define CONFIG_MUSE_TTS_URL ""
#endif
#ifndef CONFIG_MUSE_TTS_MODEL
#define CONFIG_MUSE_TTS_MODEL "kokoro"
#endif
#ifndef CONFIG_MUSE_TTS_VOICE
#define CONFIG_MUSE_TTS_VOICE "af_heart"
#endif
#ifndef CONFIG_MUSE_TTS_LANG_CODE
#define CONFIG_MUSE_TTS_LANG_CODE ""
#endif

static const char *TAG = "muse_tts";

/* MP3 in flight between the worker and the session. Speech arrives at
 * a few KB/s and the session drains it into its own far larger buffer,
 * so this only has to smooth out network bursts. */
#define TTS_BUF_BYTES (48 * 1024)
#define TTS_CHUNK 2048
/* The endpoint synthesizes before it answers, and a long reply can
 * take a while: per-read patience, not a whole-fetch deadline. */
#define TTS_TIMEOUT_MS 15000
/* How long start waits for a cancelling worker to wind down before
 * giving the reply to the session's fallback instead. */
#define TTS_START_WAIT_MS 2500
/* A full buffer with no drain for this long means the turn is gone. */
#define TTS_SEND_STALL_MS 30000

static StreamBufferHandle_t s_buf;
static TaskHandle_t s_worker;
static bool s_ready;

/* Written by start/cancel and the worker; read by all three. The
 * session is the only reader of audio, the worker the only writer,
 * and request ids keep a stale reader from touching a new fetch. */
static volatile bool s_busy;        /* the worker owns a request */
static volatile bool s_done;        /* the fetch has finished */
static volatile bool s_abort;       /* cancel asked the worker to stop */
static volatile muse_tts_status_t s_final;   /* how the fetch ended */
static volatile uint32_t s_cur_id;  /* the live request's id */
static uint32_t s_next_id;
static char *s_text;                /* the live request's text (PSRAM) */

/* Bytes `s` needs as a JSON string body, quotes not counted. */
static size_t json_escaped_len(const char *s)
{
    size_t n = 0;
    for (const uint8_t *p = (const uint8_t *)s; *p; p++) {
        n += (*p == '"' || *p == '\\' || *p == '\n' || *p == '\r' || *p == '\t') ? 2
             : (*p < 0x20) ? 6 : 1;
    }
    return n;
}

/* Writes `s` JSON-escaped at `dst`, returning the new write position. */
static char *json_escape(char *dst, const char *s)
{
    static const char hex[] = "0123456789abcdef";
    for (const uint8_t *p = (const uint8_t *)s; *p; p++) {
        uint8_t c = *p;
        if (c == '"' || c == '\\') {
            *dst++ = '\\';
            *dst++ = (char)c;
        } else if (c == '\n') {
            *dst++ = '\\'; *dst++ = 'n';
        } else if (c == '\r') {
            *dst++ = '\\'; *dst++ = 'r';
        } else if (c == '\t') {
            *dst++ = '\\'; *dst++ = 't';
        } else if (c < 0x20) {
            *dst++ = '\\'; *dst++ = 'u'; *dst++ = '0'; *dst++ = '0';
            *dst++ = hex[c >> 4];
            *dst++ = hex[c & 0xf];
        } else {
            *dst++ = (char)c;   /* UTF-8 passes through untouched */
        }
    }
    return dst;
}

/* The speech request: {"model":..,"input":..,"voice":..,...} in PSRAM. */
static char *build_body(const char *text, size_t *out_len)
{
    const char *model = CONFIG_MUSE_TTS_MODEL;
    const char *voice = CONFIG_MUSE_TTS_VOICE;
    const char *lang = CONFIG_MUSE_TTS_LANG_CODE;
    size_t cap = json_escaped_len(text) + json_escaped_len(model) + json_escaped_len(voice)
               + json_escaped_len(lang) + 128;
    char *body = heap_caps_malloc(cap, MUSE_BIG_CAPS);
    if (!body) {
        return NULL;
    }
    char *w = body;
    w += sprintf(w, "{\"model\":\"");
    w = json_escape(w, model);
    w += sprintf(w, "\",\"input\":\"");
    w = json_escape(w, text);
    w += sprintf(w, "\",\"voice\":\"");
    w = json_escape(w, voice);
    w += sprintf(w, "\",\"response_format\":\"mp3\"");
    if (lang[0]) {
        w += sprintf(w, ",\"lang_code\":\"");
        w = json_escape(w, lang);
        *w++ = '"';
    }
#if CONFIG_MUSE_TTS_STREAM
    w += sprintf(w, ",\"stream\":true");
#endif
    *w++ = '}';
    *w = '\0';
    *out_len = (size_t)(w - body);
    return body;
}

/* Pushes one downloaded chunk into the stream buffer, honouring abort
 * and giving up if the session stops draining for good. */
static bool push_audio(const uint8_t *data, size_t len)
{
    size_t sent = 0;
    int stalled_ms = 0;
    while (sent < len) {
        if (s_abort) {
            return false;
        }
        size_t n = xStreamBufferSend(s_buf, data + sent, len - sent, pdMS_TO_TICKS(200));
        if (n) {
            sent += n;
            stalled_ms = 0;
        } else if ((stalled_ms += 200) >= TTS_SEND_STALL_MS) {
            ESP_LOGW(TAG, "speech buffer undrained for %d ms; dropping the fetch", TTS_SEND_STALL_MS);
            return false;
        }
    }
    return true;
}

/* Runs one fetch to its end: sets s_final/s_done, frees the request. */
static void run_fetch(char *text)
{
    size_t body_len = 0;
    char *body = build_body(text, &body_len);
    free(text);
    if (!body) {
        ESP_LOGW(TAG, "no memory for the speech request");
        s_final = MUSE_TTS_ERROR;
        return;
    }

    esp_http_client_config_t cfg = {
        .url = CONFIG_MUSE_TTS_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = TTS_TIMEOUT_MS,
        .buffer_size = TTS_CHUNK,
        .buffer_size_tx = TTS_CHUNK,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(body);
        s_final = MUSE_TTS_ERROR;
        return;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");

    bool ok = false;
    if (!s_abort && esp_http_client_open(client, (int)body_len) == ESP_OK) {
        size_t off = 0;
        bool wrote = true;
        while (off < body_len) {
            int n = esp_http_client_write(client, body + off, (int)(body_len - off));
            if (n < 0) {
                wrote = false;
                break;
            }
            off += (size_t)n;
        }
        if (wrote && esp_http_client_fetch_headers(client) >= 0) {
            int status = esp_http_client_get_status_code(client);
            if (status == 200) {
                uint8_t chunk[TTS_CHUNK];
                ok = true;
                while (!esp_http_client_is_complete_data_received(client)) {
                    if (s_abort) {
                        ok = false;
                        break;
                    }
                    int n = esp_http_client_read(client, (char *)chunk, sizeof(chunk));
                    if (n < 0) {
                        ok = false;
                        break;
                    }
                    if (n > 0 && !push_audio(chunk, (size_t)n)) {
                        ok = false;
                        break;
                    }
                }
            } else {
                ESP_LOGW(TAG, "speech endpoint answered HTTP %d", status);
            }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(body);
    if (s_abort) {
        ok = false;
    }
    s_final = ok ? MUSE_TTS_END : MUSE_TTS_ERROR;
    ESP_LOGI(TAG, "speech fetch %s", ok ? "complete" : "failed");
}

static void tts_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        char *text = s_text;
        s_text = NULL;
        if (text) {
            if (s_abort) {
                free(text);
                s_final = MUSE_TTS_ERROR;
            } else {
                run_fetch(text);
            }
        } else {
            s_final = MUSE_TTS_ERROR;
        }
        s_done = true;
        s_busy = false;
    }
}

void muse_tts_init(void)
{
    if (s_ready) {
        return;
    }
    uint8_t *storage = heap_caps_malloc(TTS_BUF_BYTES + 1, MUSE_BIG_CAPS);
    StaticStreamBuffer_t *ctrl =
        heap_caps_malloc(sizeof(StaticStreamBuffer_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!storage || !ctrl) {
        ESP_LOGW(TAG, "no memory for the speech buffer; local TTS unavailable");
        free(storage);
        free(ctrl);
        return;
    }
    s_buf = xStreamBufferCreateStatic(TTS_BUF_BYTES, 1, storage, ctrl);
    if (!s_buf ||
        xTaskCreatePinnedToCore(tts_task, "muse_tts", 12 * 1024, NULL, 5, &s_worker, 0) != pdPASS) {
        ESP_LOGW(TAG, "could not start the speech worker; local TTS unavailable");
        return;
    }
    s_ready = true;
    if (CONFIG_MUSE_TTS_URL[0]) {
        ESP_LOGI(TAG, "local TTS ready: POST %s (model %s, voice %s)",
                 CONFIG_MUSE_TTS_URL, CONFIG_MUSE_TTS_MODEL, CONFIG_MUSE_TTS_VOICE);
    } else {
        ESP_LOGI(TAG, "local TTS not configured (no speech endpoint URL)");
    }
}

uint32_t muse_tts_start(const char *text)
{
    if (!s_ready || !text || !text[0] || !CONFIG_MUSE_TTS_URL[0]) {
        return 0;
    }
    /* One fetch at a time. A worker still winding down from a cancel
     * gets a moment; past that, this reply falls back instead of
     * stalling the session. */
    for (int waited = 0; s_busy && waited < TTS_START_WAIT_MS; waited += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_busy) {
        return 0;
    }
    char *copy = heap_caps_malloc(strlen(text) + 1, MUSE_BIG_CAPS);
    if (!copy) {
        return 0;
    }
    strcpy(copy, text);
    xStreamBufferReset(s_buf);
    s_final = MUSE_TTS_ERROR;
    s_done = false;
    s_abort = false;
    s_text = copy;
    if (++s_next_id == 0) {
        ++s_next_id;   /* 0 is the session's "no fetch" marker */
    }
    s_cur_id = s_next_id;
    s_busy = true;
    xTaskNotifyGive(s_worker);
    return s_cur_id;
}

size_t muse_tts_read(uint32_t id, uint8_t *dst, size_t len, muse_tts_status_t *status)
{
    if (!status) {
        return 0;
    }
    if (!s_ready || !dst || id == 0 || id != s_cur_id || (!s_busy && !s_done)) {
        *status = MUSE_TTS_ERROR;
        return 0;
    }
    size_t n = xStreamBufferReceive(s_buf, dst, len, 0);
    if (!s_done) {
        *status = MUSE_TTS_PENDING;
        return n;
    }
    if (n && xStreamBufferBytesAvailable(s_buf)) {
        *status = MUSE_TTS_PENDING;   /* the tail is still draining */
        return n;
    }
    *status = s_final;   /* drained: END, or ERROR (possibly after audio) */
    return n;
}

void muse_tts_cancel(void)
{
    if (s_busy) {
        s_abort = true;
    }
}
