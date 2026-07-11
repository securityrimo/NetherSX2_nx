/* aaudio.c -- NDK AAudio ABI shim backed by one libnx audren FLOAT voice.
 *
 * libemucore.so drives audio through AetherSX2's "AAudioMod" pull-callback
 * backend: it builds a stream (48000 / 2ch / PCM_FLOAT / LOW_LATENCY), then a
 * device thread repeatedly calls the registered data callback to FILL
 * interleaved stereo frames. We satisfy the ~19 imported symbols with a single
 * audren stereo float voice @ 48000 and a dedicated playback thread that pulls
 * from the core and feeds wavebufs. No conversion in the common path (audren
 * speaks float natively); an s16 fallback covers the case audren rejects FLOAT.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

#include "aaudio.h"
#include "pthr.h"
#include "util.h"

// One data-callback block size when the builder didn't pick one. 512 frames @
// 48k float stereo is ~10.7 ms -- within LOW_LATENCY and a comfortable multiple
// of the audren mix tick (~5 ms), so one pulled block spans ~2 audren frames.
#define DEFAULT_FRAMES_PER_CB 512

// Triple-buffer the wavebufs so the playback thread always has a free slot to
// fill while audren consumes the others; double would stall on the boundary.
#define NUM_WAVEBUFS 3

// ---------------------------------------------------------------------------
// audren config: stereo, 48 kHz, the default revision/mix layout. One mix
// (the final output mix) with stereo channels feeding the default device sink.
// ---------------------------------------------------------------------------
static const AudioRendererConfig k_audren_cfg = {
  .output_rate     = AudioRendererOutputRate_48kHz,
  .num_voices      = 4,
  .num_effects     = 0,
  .num_sinks       = 1,
  .num_mix_objs    = 1,
  .num_mix_buffers = 2,  // stereo
};

// ---------------------------------------------------------------------------
// Concrete definitions of the opaque types declared in aaudio.h. The core only
// ever holds these by pointer, so their layout is private to this file.
// ---------------------------------------------------------------------------
struct AAudioStreamBuilderStruct {
  int32_t sample_rate, channel_count, device_id;
  aaudio_format_t           format;
  aaudio_direction_t        direction;
  aaudio_sharing_mode_t     sharing;
  aaudio_performance_mode_t perf;
  int32_t buffer_capacity_frames, frames_per_cb;
  AAudioStream_dataCallback  data_cb;  void *data_user;
  AAudioStream_errorCallback error_cb; void *err_user;
};

struct AAudioStreamStruct {
  // resolved config (echo of builder, with negotiated values)
  int32_t sample_rate, channel_count, frames_per_cb;
  aaudio_format_t format;            // negotiated: FLOAT, or I16 on fallback
  AAudioStream_dataCallback  data_cb;  void *data_user;
  AAudioStream_errorCallback error_cb; void *err_user;

  // libnx audren side
  int               audren_inited;
  AudioDriver       drv;
  int               mempool_id;
  int               voice_id;
  AudioDriverWaveBuf wavebufs[NUM_WAVEBUFS];
  void             *ring;            // mempool-backed, page aligned
  size_t            ring_bytes;
  size_t            block_bytes;     // one cb block, in the device's sample fmt
  int               next_buf;        // round-robin index into wavebufs

  // playback thread + state
  pthread_t                  thread;
  int                        thread_started;
  volatile int               running;
  volatile int               thread_exited;   // set by playback_thread before it returns
  volatile int               in_data_cb;      // 1 while inside the core's data_cb
  volatile int               thread_detached;  // stop had to abandon a wedged thread
  volatile aaudio_stream_state_t state;
  volatile int64_t           frames_read;   // monotonic; AAudioStream_getFramesRead
  Mutex                      lock;           // guards state + cv
  CondVar                    state_cv;       // signalled on every state change
};

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

// libnx Result -> AAudio result. The core only branches on ==AAUDIO_OK vs not
// and logs the number, so a single generic error code is sufficient.
static aaudio_result_t map_result(Result rc) {
  return R_SUCCEEDED(rc) ? AAUDIO_OK : AAUDIO_ERROR_UNAVAILABLE;
}

// set state under lock and wake any waitForStateChange. Every transition must
// go through here so the condvar stays in sync with `state`.
static void set_state(AAudioStream *s, aaudio_stream_state_t st) {
  mutexLock(&s->lock);
  s->state = st;
  condvarWakeAll(&s->state_cv);
  mutexUnlock(&s->lock);
}

// bytes per interleaved stereo frame in the negotiated device format.
static size_t frame_bytes(AAudioStream *s) {
  size_t sample = (s->format == AAUDIO_FORMAT_PCM_I16) ? sizeof(int16_t)
                                                       : sizeof(float);
  return sample * (size_t)s->channel_count;
}

// ---------------------------------------------------------------------------
// builder
// ---------------------------------------------------------------------------

aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder **builder) {
  if (!builder) return AAUDIO_ERROR_NULL;
  AAudioStreamBuilder *b = calloc(1, sizeof(*b));
  if (!b) return AAUDIO_ERROR_NO_MEMORY;
  // sane defaults matching the AAudioMod request; the core overrides via setters
  b->sample_rate   = 48000;
  b->channel_count = 2;
  b->format        = AAUDIO_FORMAT_PCM_FLOAT;
  b->direction     = AAUDIO_DIRECTION_OUTPUT;
  b->sharing       = AAUDIO_SHARING_MODE_SHARED;
  b->perf          = AAUDIO_PERFORMANCE_MODE_LOW_LATENCY;
  b->device_id     = 0;
  b->buffer_capacity_frames = 0;
  b->frames_per_cb = 0;  // 0 => we choose DEFAULT_FRAMES_PER_CB at openStream
  *builder = b;
  debugPrintf("aaudio: createStreamBuilder %p\n", (void *)b);
  return AAUDIO_OK;
}

void AAudioStreamBuilder_setDeviceId(AAudioStreamBuilder *b, int32_t v) {
  if (b) b->device_id = v;
}
void AAudioStreamBuilder_setDirection(AAudioStreamBuilder *b, aaudio_direction_t v) {
  if (b) b->direction = v;
}
void AAudioStreamBuilder_setSharingMode(AAudioStreamBuilder *b, aaudio_sharing_mode_t v) {
  if (b) b->sharing = v;
}
void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder *b, int32_t v) {
  if (b) b->sample_rate = v;
}
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder *b, int32_t v) {
  if (b) b->channel_count = v;
}
void AAudioStreamBuilder_setFormat(AAudioStreamBuilder *b, aaudio_format_t v) {
  if (b) b->format = v;
}
void AAudioStreamBuilder_setBufferCapacityInFrames(AAudioStreamBuilder *b, int32_t v) {
  if (b) b->buffer_capacity_frames = v;
}
void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder *b, aaudio_performance_mode_t v) {
  if (b) b->perf = v;
}
void AAudioStreamBuilder_setFramesPerDataCallback(AAudioStreamBuilder *b, int32_t v) {
  if (b) b->frames_per_cb = v;
}
void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder *b,
                                         AAudioStream_dataCallback cb, void *user) {
  if (b) { b->data_cb = cb; b->data_user = user; }
}
void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder *b,
                                          AAudioStream_errorCallback cb, void *user) {
  if (b) { b->error_cb = cb; b->err_user = user; }
}

// Bring up audren and one stereo voice. We try FLOAT first (zero-copy from the
// SPU2 mixer); if audren rejects the float voice we drop to PCM_I16 and convert
// in the playback thread.
static aaudio_result_t open_audren(AAudioStream *s) {
  Result rc;

  rc = audrenInitialize(&k_audren_cfg);
  if (R_FAILED(rc)) {
    debugPrintf("aaudio: audrenInitialize failed: 0x%x\n", rc);
    return map_result(rc);
  }
  s->audren_inited = 1;

  rc = audrvCreate(&s->drv, &k_audren_cfg, s->channel_count);
  if (R_FAILED(rc)) {
    debugPrintf("aaudio: audrvCreate failed: 0x%x\n", rc);
    audrenExit();
    s->audren_inited = 0;
    return map_result(rc);
  }

  // Ring buffer holding NUM_WAVEBUFS data-callback blocks back to back. audren
  // mempools must be page-aligned in both base and size; alignTo handles size.
  s->block_bytes = (size_t)s->frames_per_cb * frame_bytes(s);
  s->ring_bytes  = (s->block_bytes * NUM_WAVEBUFS + 0xFFF) & ~(size_t)0xFFF;
  s->ring = aligned_alloc(0x1000, s->ring_bytes);
  if (!s->ring) {
    debugPrintf("aaudio: ring alloc (%zu) failed\n", s->ring_bytes);
    audrvClose(&s->drv);
    audrenExit();
    s->audren_inited = 0;
    return AAUDIO_ERROR_NO_MEMORY;
  }
  memset(s->ring, 0, s->ring_bytes);
  armDCacheFlush(s->ring, s->ring_bytes);

  s->mempool_id = audrvMemPoolAdd(&s->drv, s->ring, s->ring_bytes);
  if (s->mempool_id < 0) {
    debugPrintf("aaudio: audrvMemPoolAdd failed\n");
    goto fail_pool;
  }
  audrvMemPoolAttach(&s->drv, s->mempool_id);

  // final output mix on the default device, full gain on both channels.
  // NOTE(on-device): the exact audrvVoiceInit signature
  // (drv, id, channel_count, PcmFormat, sample_rate) and the
  // SetDestinationMix(drv, id, AUDREN_FINAL_MIX_ID) constant should be
  // re-checked against the installed libnx headers; values below are the
  // documented audrv API.
  static const u8 sink_channels[] = { 0, 1 };
  int sink_id = audrvDeviceSinkAdd(&s->drv, AUDREN_DEFAULT_DEVICE_NAME,
                                   s->channel_count, sink_channels);
  if (sink_id < 0)
    debugPrintf("aaudio: audrvDeviceSinkAdd failed (continuing)\n");

  rc = audrvUpdate(&s->drv);
  if (R_FAILED(rc))
    debugPrintf("aaudio: initial audrvUpdate failed: 0x%x\n", rc);

  s->voice_id = 0;
  PcmFormat pcm = (s->format == AAUDIO_FORMAT_PCM_I16) ? PcmFormat_Int16
                                                       : PcmFormat_Float;
  bool ok = audrvVoiceInit(&s->drv, s->voice_id, s->channel_count, pcm,
                           (float)s->sample_rate);
  if (!ok && s->format != AAUDIO_FORMAT_PCM_I16) {
    // audren refused FLOAT -- fall back to s16 and convert in the thread.
    debugPrintf("aaudio: FLOAT voice refused, falling back to PCM_I16\n");
    s->format = AAUDIO_FORMAT_PCM_I16;
    // resize ring for the now-smaller frame size
    free(s->ring);
    s->block_bytes = (size_t)s->frames_per_cb * frame_bytes(s);
    s->ring_bytes  = (s->block_bytes * NUM_WAVEBUFS + 0xFFF) & ~(size_t)0xFFF;
    s->ring = aligned_alloc(0x1000, s->ring_bytes);
    if (!s->ring) { goto fail_voice; }
    memset(s->ring, 0, s->ring_bytes);
    armDCacheFlush(s->ring, s->ring_bytes);
    audrvMemPoolDetach(&s->drv, s->mempool_id);
    audrvMemPoolRemove(&s->drv, s->mempool_id);
    s->mempool_id = audrvMemPoolAdd(&s->drv, s->ring, s->ring_bytes);
    if (s->mempool_id < 0) goto fail_voice;
    audrvMemPoolAttach(&s->drv, s->mempool_id);
    ok = audrvVoiceInit(&s->drv, s->voice_id, s->channel_count,
                        PcmFormat_Int16, (float)s->sample_rate);
  }
  if (!ok) {
    debugPrintf("aaudio: audrvVoiceInit failed\n");
    goto fail_voice;
  }

  audrvVoiceSetDestinationMix(&s->drv, s->voice_id, AUDREN_FINAL_MIX_ID);
  // straight stereo: L->mixbuf0, R->mixbuf1 at unity, no cross-feed.
  audrvVoiceSetMixFactor(&s->drv, s->voice_id, 1.0f, 0, 0);
  audrvVoiceSetMixFactor(&s->drv, s->voice_id, 1.0f, 1, 1);
  if (s->channel_count >= 2) {
    audrvVoiceSetMixFactor(&s->drv, s->voice_id, 0.0f, 1, 0);
    audrvVoiceSetMixFactor(&s->drv, s->voice_id, 0.0f, 0, 1);
  }
  audrvVoiceStart(&s->drv, s->voice_id);

  audrvUpdate(&s->drv);

  // ★ THE renderer must be explicitly started. Without audrenStartAudioRenderer()
  // everything above succeeds -- mempool attached, sink added, voice started, the
  // playback thread happily pulls samples and audrvUpdate() returns OK -- but
  // audren never actually renders and the console stays SILENT. This one missing
  // call was why the port had no sound despite a complete audio path.
  rc = audrenStartAudioRenderer();
  if (R_FAILED(rc)) {
    debugPrintf("aaudio: audrenStartAudioRenderer failed: 0x%x\n", rc);
    goto fail_voice;
  }

  debugPrintf("aaudio: audren ready: %d Hz %d ch %s, block %zu B x %d wavebufs, "
              "renderer STARTED\n", s->sample_rate, s->channel_count,
              (s->format == AAUDIO_FORMAT_PCM_I16) ? "s16" : "float",
              s->block_bytes, NUM_WAVEBUFS);
  return AAUDIO_OK;

fail_voice:
  if (s->mempool_id >= 0) {
    audrvMemPoolDetach(&s->drv, s->mempool_id);
    audrvMemPoolRemove(&s->drv, s->mempool_id);
  }
fail_pool:
  free(s->ring);
  s->ring = NULL;
  audrvClose(&s->drv);
  audrenExit();
  s->audren_inited = 0;
  return AAUDIO_ERROR_UNAVAILABLE;
}

aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder *b,
                                               AAudioStream **stream) {
  if (!b || !stream) return AAUDIO_ERROR_NULL;
  if (!b->data_cb) {
    // AAudioMod always sets a data callback; without it we have no source.
    debugPrintf("aaudio: openStream with no data callback\n");
    return AAUDIO_ERROR_NULL;
  }

  AAudioStream *s = calloc(1, sizeof(*s));
  if (!s) return AAUDIO_ERROR_NO_MEMORY;

  s->sample_rate   = b->sample_rate   > 0 ? b->sample_rate   : 48000;
  s->channel_count = b->channel_count > 0 ? b->channel_count : 2;
  s->frames_per_cb = b->frames_per_cb > 0 ? b->frames_per_cb : DEFAULT_FRAMES_PER_CB;
  s->format        = (b->format == AAUDIO_FORMAT_PCM_I16) ? AAUDIO_FORMAT_PCM_I16
                                                          : AAUDIO_FORMAT_PCM_FLOAT;
  s->data_cb   = b->data_cb;   s->data_user = b->data_user;
  s->error_cb  = b->error_cb;  s->err_user  = b->err_user;
  s->mempool_id = -1;
  s->voice_id   = -1;
  mutexInit(&s->lock);
  condvarInit(&s->state_cv);

  debugPrintf("aaudio: (AAudioMod) Creating stream %dHz %dch fmt=%d cb=%d\n",
              s->sample_rate, s->channel_count, s->format, s->frames_per_cb);

  aaudio_result_t r = open_audren(s);
  if (r != AAUDIO_OK) {
    free(s);
    return r;
  }

  set_state(s, AAUDIO_STREAM_STATE_OPEN);
  *stream = s;
  return AAUDIO_OK;
}

aaudio_result_t AAudioStreamBuilder_delete(AAudioStreamBuilder *b) {
  free(b);
  return AAUDIO_OK;
}

// ---------------------------------------------------------------------------
// playback thread: pull from the core, feed audren, advance frames_read
// ---------------------------------------------------------------------------

// is this wavebuf free to refill? Done/Free means audren is no longer reading
// it; a never-queued slot has state 0 (== AudioDriverWaveBufState_Free).
static int wavebuf_is_free(AudioDriverWaveBuf *wb) {
  return wb->state == AudioDriverWaveBufState_Free ||
         wb->state == AudioDriverWaveBufState_Done;
}

static void *playback_thread(void *arg) {
  AAudioStream *s = (AAudioStream *)arg;

  // keep audio off the GS performance cores (which the core pins itself); a
  // background core avoids starving the renderer -- the lswtcs stutter lesson.
  pthr_pin_bg_core();
  pthr_ensure_fake_tls();  // data_cb runs core code; needs a valid TLS cookie

  set_state(s, AAUDIO_STREAM_STATE_STARTED);
  debugPrintf("aaudio: (AAudioMod) Starting stream...\n");

  while (s->running) {
    AudioDriverWaveBuf *wb = &s->wavebufs[s->next_buf];
    if (!wavebuf_is_free(wb)) {
      // all slots in flight -- wait one audren mix tick and re-poll.
      audrenWaitFrame();
      audrvUpdate(&s->drv);
      continue;
    }

    // PULL: the core fills `numFrames` interleaved frames into our block, IN THE
    // STREAM'S NEGOTIATED FORMAT. AAudioMod asks for fmt=1 == AAUDIO_FORMAT_PCM_I16,
    // so it writes int16 directly. The old code assumed the core always emitted
    // float: it handed the callback a float scratch buffer, then reinterpreted the
    // int16 bytes it wrote as floats and "converted" them -- pure garbage, which is
    // why audio was a loud noise. The voice is already PcmFormat_Int16 to match, so
    // just let the core fill the wavebuf block directly. No conversion, either way.
    void *block = (uint8_t *)s->ring + (size_t)s->next_buf * s->block_bytes;
    void *cb_dst = block;

    // data_cb runs CORE code and can block on a core lock the VM-reset thread
    // holds while it is stopping us -> flag it so stop_playback_thread can tell a
    // wedged-in-core thread from a clean one and detach instead of deadlocking.
    __atomic_store_n(&s->in_data_cb, 1, __ATOMIC_RELEASE);
    aaudio_data_callback_result_t cr =
        s->data_cb((AAudioStream *)s, s->data_user, cb_dst, s->frames_per_cb);
    __atomic_store_n(&s->in_data_cb, 0, __ATOMIC_RELEASE);


    // hand the freshly filled block to audren.
    armDCacheFlush(block, s->block_bytes);
    memset(wb, 0, sizeof(*wb));
    wb->data_raw      = s->ring;
    wb->size          = s->ring_bytes;
    wb->start_sample_offset = (s32)((size_t)s->next_buf * s->frames_per_cb);
    wb->end_sample_offset   = wb->start_sample_offset + s->frames_per_cb;
    audrvVoiceAddWaveBuf(&s->drv, s->voice_id, wb);
    audrvUpdate(&s->drv);

    // Prove samples are actually flowing (and that they're not all silence).
    {
      static unsigned fed = 0;
      if (fed < 3 || (fed & 0x1ff) == 0) {
        float peak = 0.f;
        const int n = s->frames_per_cb * s->channel_count;
        if (s->format == AAUDIO_FORMAT_PCM_I16) {
          const int16_t *p = (const int16_t *)block;
          for (int i = 0; i < n; i++) {
            float a = (p[i] < 0 ? -(float)p[i] : (float)p[i]) / 32768.0f;
            if (a > peak) peak = a;
          }
        } else {
          const float *f = (const float *)block;
          for (int i = 0; i < n; i++) {
            float a = f[i] < 0 ? -f[i] : f[i];
            if (a > peak) peak = a;
          }
        }
        debugPrintf("aaudio: fed block #%u (%d frames, %s) peak=%.3f\n", fed,
                    s->frames_per_cb,
                    (s->format == AAUDIO_FORMAT_PCM_I16) ? "s16" : "f32", peak);
      }
      fed++;
    }

    s->frames_read += s->frames_per_cb;   // monotonic; AAudioStream_getFramesRead
    s->next_buf = (s->next_buf + 1) % NUM_WAVEBUFS;

    if (cr == AAUDIO_CALLBACK_RESULT_STOP) {
      s->running = 0;
      break;
    }

    // block until audren has consumed a frame's worth before refilling.
    audrenWaitFrame();
  }

  debugPrintf("aaudio: playback thread exit (frames_read=%lld)\n",
              (long long)s->frames_read);
  __atomic_store_n(&s->thread_exited, 1, __ATOMIC_RELEASE);
  return NULL;
}

// Stop the playback thread WITHOUT deadlocking. The core calls requestStop/close
// from a VM-reset context while (sometimes) holding a lock that data_cb -- which
// runs on the playback thread -- is blocked on; a bare pthread_join then hangs
// the whole VM forever (observed: "Stopping stream..." was the last log line, no
// thread-exit, no progress for 25s). So: signal stop, then wait up to ~1.5s for a
// clean exit and join; if the thread is wedged inside the core callback, DETACH
// and return so the reset can proceed -- which releases the lock and lets the
// detached thread drain and exit on its own. Returns 1 if joined, 0 if detached.
static int stop_playback_thread(AAudioStream *s) {
  if (!s->thread_started) return 1;
  __atomic_store_n(&s->running, 0, __ATOMIC_RELEASE);
  for (int i = 0; i < 300; i++) { // 300 * 5ms = 1.5s
    if (__atomic_load_n(&s->thread_exited, __ATOMIC_ACQUIRE)) {
      pthread_join(s->thread, NULL);
      s->thread_started = 0;
      return 1;
    }
    svcSleepThread(5000000ULL);
  }
  debugPrintf("aaudio: playback thread wedged (in_data_cb=%d); detaching to "
              "avoid VM-reset deadlock\n", s->in_data_cb);
  pthread_detach(s->thread);
  s->thread_started = 0;
  s->thread_detached = 1;
  return 0;
}

// ---------------------------------------------------------------------------
// stream control
// ---------------------------------------------------------------------------

aaudio_result_t AAudioStream_requestStart(AAudioStream *s) {
  if (!s) return AAUDIO_ERROR_NULL;
  debugPrintf("aaudio: (AAudioMod) requestStart\n");
  set_state(s, AAUDIO_STREAM_STATE_STARTING);

  if (s->thread_started) {
    // resume from pause: unpause the voice and let the loop pull again.
    s->running = 1;
    audrvVoiceSetPaused(&s->drv, s->voice_id, false);
    audrvUpdate(&s->drv);
    set_state(s, AAUDIO_STREAM_STATE_STARTED);
    return AAUDIO_OK;
  }

  // If a previous stop had to detach a wedged thread, make sure it has actually
  // exited before spawning a new one -- else two threads would drive `s` at once.
  if (s->thread_detached && !__atomic_load_n(&s->thread_exited, __ATOMIC_ACQUIRE)) {
    for (int i = 0; i < 400 && !__atomic_load_n(&s->thread_exited, __ATOMIC_ACQUIRE); i++)
      svcSleepThread(5000000ULL); // wait up to 2s for the old thread to drain
    if (!__atomic_load_n(&s->thread_exited, __ATOMIC_ACQUIRE)) {
      debugPrintf("aaudio: requestStart: previous playback thread still wedged; refusing\n");
      set_state(s, AAUDIO_STREAM_STATE_OPEN);
      return AAUDIO_ERROR_UNAVAILABLE;
    }
  }
  s->thread_detached = 0;
  s->thread_exited = 0;   // fresh thread; must be cleared BEFORE pthread_create
  s->in_data_cb = 0;

  s->running = 1;
  // spawn via newlib pthreads directly (this is OUR thread, not core-spawned);
  // it pins itself to a bg core in playback_thread.
  int rc = pthread_create(&s->thread, NULL, playback_thread, s);
  if (rc != 0) {
    s->running = 0;
    debugPrintf("aaudio: pthread_create failed: %d\n", rc);
    set_state(s, AAUDIO_STREAM_STATE_OPEN);
    return AAUDIO_ERROR_UNAVAILABLE;
  }
  s->thread_started = 1;
  // playback_thread sets STATE_STARTED once it's running.
  return AAUDIO_OK;
}

aaudio_result_t AAudioStream_requestPause(AAudioStream *s) {
  if (!s) return AAUDIO_ERROR_NULL;
  debugPrintf("aaudio: (AAudioMod) Requesting pause...\n");
  set_state(s, AAUDIO_STREAM_STATE_PAUSING);
  // stop pulling but keep the thread alive so a later start just unpauses.
  s->running = 0;
  if (s->voice_id >= 0) {
    audrvVoiceSetPaused(&s->drv, s->voice_id, true);
    audrvUpdate(&s->drv);
  }
  set_state(s, AAUDIO_STREAM_STATE_PAUSED);
  return AAUDIO_OK;
}

aaudio_result_t AAudioStream_requestStop(AAudioStream *s) {
  if (!s) return AAUDIO_ERROR_NULL;
  debugPrintf("aaudio: (AAudioMod) Stopping stream...\n");
  set_state(s, AAUDIO_STREAM_STATE_STOPPING);
  stop_playback_thread(s);
  if (s->voice_id >= 0) {
    audrvVoiceStop(&s->drv, s->voice_id);
    audrvUpdate(&s->drv);
  }
  set_state(s, AAUDIO_STREAM_STATE_STOPPED);
  return AAUDIO_OK;
}

aaudio_result_t AAudioStream_close(AAudioStream *s) {
  if (!s) return AAUDIO_ERROR_NULL;
  debugPrintf("aaudio: (AAudioMod) Closing stream...\n");
  set_state(s, AAUDIO_STREAM_STATE_CLOSING);

  int joined = stop_playback_thread(s);

  if (s->audren_inited) {
    if (s->voice_id >= 0)
      audrvVoiceStop(&s->drv, s->voice_id);
    if (s->mempool_id >= 0) {
      audrvMemPoolDetach(&s->drv, s->mempool_id);
      audrvMemPoolRemove(&s->drv, s->mempool_id);
    }
    audrvUpdate(&s->drv);
    audrvClose(&s->drv);
    audrenStopAudioRenderer();  // pairs with audrenStartAudioRenderer()
    audrenExit();
    s->audren_inited = 0;
  }

  set_state(s, AAUDIO_STREAM_STATE_CLOSED);
  // If we had to detach a wedged playback thread and it still hasn't exited, it
  // may still touch `s`/`s->ring` -> leak them rather than free-then-UAF. This is
  // a rare teardown-race path; a one-shot leak at VM reset/exit is acceptable.
  if (!joined && !__atomic_load_n(&s->thread_exited, __ATOMIC_ACQUIRE)) {
    debugPrintf("aaudio: leaking stream (detached playback thread still live)\n");
    return AAUDIO_OK;
  }
  free(s->ring);
  free(s);
  return AAUDIO_OK;
}

int64_t AAudioStream_getFramesRead(AAudioStream *s) {
  // monotonic counter the core uses for A/V sync and its disconnect/
  // "counter mismatch" reopen check; MUST advance while playing.
  return s ? s->frames_read : 0;
}

aaudio_result_t AAudioStream_waitForStateChange(AAudioStream *s,
                                                aaudio_stream_state_t inputState,
                                                aaudio_stream_state_t *nextState,
                                                int64_t timeoutNanoseconds) {
  if (!s) return AAUDIO_ERROR_NULL;

  mutexLock(&s->lock);
  if (s->state != inputState) {
    // already moved on -- report current state immediately.
    if (nextState) *nextState = s->state;
    mutexUnlock(&s->lock);
    return AAUDIO_OK;
  }

  // wait for the next set_state() signal, bounded by the caller's timeout.
  // condvarWaitTimeout returns 0xEA01 (timeout) when the deadline expires.
  Result rc = condvarWaitTimeout(&s->state_cv, &s->lock,
                                 (u64)(timeoutNanoseconds > 0 ? timeoutNanoseconds : 0));
  aaudio_stream_state_t cur = s->state;
  mutexUnlock(&s->lock);

  if (R_FAILED(rc) && cur == inputState) {
    // timed out without a transition; the core tolerates and retries.
    return AAUDIO_ERROR_TIMEOUT;
  }
  if (nextState) *nextState = cur;
  return AAUDIO_OK;
}
