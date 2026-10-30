/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *  Copyright (C) 2014-2017 - Ali Bouhlel
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

/* PS4 audio driver, following the design of upstream RetroArch's
 * ps4_audio.c (2026): the ring is sized from the latency setting, a
 * period is played as soon as one is held, and the window handed to
 * sceAudioOutOutput stays the device's until the call after it returns.
 * The worker also reports its own timing to the log, so underruns and
 * late periods can be told apart on the console. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include <rthreads/rthreads.h>

#include <orbis/AudioOut.h>
#include <orbis/libkernel.h>

#include "../../defines/ps4_defines.h"
#include "../../retroarch.h"
#include "../../verbosity.h"

/* Frames per sceAudioOutOutput call */
#define PS4_AUDIO_PERIOD      512u
/* The only rate sceAudioOut opens at */
#define PS4_AUDIO_RATE        48000u
#define PS4_AUDIO_FRAME_BYTES 4u
#define PS4_AUDIO_PERIOD_US   (PS4_AUDIO_PERIOD * 1000000u / PS4_AUDIO_RATE)

/* Six periods hold a 30 Hz frame of audio (1600 frames) beside the
 * window in flight; sixty-four bound the latency setting. */
#define PS4_AUDIO_RING_MIN    (PS4_AUDIO_PERIOD * 6u)
#define PS4_AUDIO_RING_MAX    (PS4_AUDIO_PERIOD * 64u)

/* Above the main thread (SCE_KERNEL_PRIO_FIFO_DEFAULT, 700): a period
 * the worker hands over late is a gap the listener hears. */
#define PS4_AUDIO_WORKER_PRIO 300

#define PS4_AUDIO_WRITE_WAIT_US 100000
#define PS4_AUDIO_REPORT_US     5000000u

/* Frames held, and frames the writer may place: one short of the ring,
 * so a full ring and an empty one are not both write == read. */
#define RING_HELD(w, r, n) ((unsigned)((w) >= (r) ? (w) - (r) : (n) - (r) + (w)))
#define RING_FREE(w, r, n) ((n) - 1u - RING_HELD((w), (r), (n)))

#define LOAD_ACQ(p)     __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define STORE_REL(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)

typedef struct ps4_audio_stats
{
   unsigned periods;
   unsigned underruns;
   unsigned late;
   unsigned max_gap_us;
   unsigned min_held;
} ps4_audio_stats_t;

typedef struct ps4_audio
{
   /* The ring, followed by one period of silence */
   uint32_t *buffer;
   unsigned ring;

   sthread_t *worker_thread;
   slock_t *lock;
   scond_t *cond;

   /* Published with release by their owner, read with acquire by the
    * other side. read_pos is the start of the window in flight. */
   unsigned read_pos;
   unsigned write_pos;

   int port;
   int running;
   bool nonblock;

   /* Written by the writer only */
   unsigned writer_waits;
} ps4_audio_t;

static unsigned ps4_ring_frames(unsigned latency)
{
   unsigned frames = PS4_AUDIO_RATE * latency / 1000u;
   frames = ((frames + PS4_AUDIO_PERIOD - 1u) / PS4_AUDIO_PERIOD)
      * PS4_AUDIO_PERIOD;
   if (frames < PS4_AUDIO_RING_MIN)
      return PS4_AUDIO_RING_MIN;
   if (frames > PS4_AUDIO_RING_MAX)
      return PS4_AUDIO_RING_MAX;
   return frames;
}

static void ps4_audio_stats_reset(ps4_audio_stats_t *stats, unsigned ring)
{
   memset(stats, 0, sizeof(*stats));
   stats->min_held = ring;
}

static void ps4_audio_worker(void *data)
{
   ps4_audio_t *ps4      = (ps4_audio_t*)data;
   unsigned play_pos     = LOAD_ACQ(&ps4->read_pos);
   uint64_t last_out     = 0;
   uint64_t last_report  = sceKernelGetProcessTime();
   ps4_audio_stats_t stats;
   int prio_before       = -1;

   scePthreadGetprio(pthread_self(), &prio_before);
   if (scePthreadSetprio(pthread_self(), PS4_AUDIO_WORKER_PRIO) < 0)
      RARCH_WARN("[PS4 audio]: could not raise worker priority (%d)\n",
            prio_before);
   else
      RARCH_LOG("[PS4 audio]: worker priority %d -> %d\n",
            prio_before, PS4_AUDIO_WORKER_PRIO);

   ps4_audio_stats_reset(&stats, ps4->ring);

   while (LOAD_ACQ(&ps4->running))
   {
      uint64_t now;
      unsigned write_pos = LOAD_ACQ(&ps4->write_pos);
      unsigned held      = RING_HELD(write_pos, play_pos, ps4->ring);
      bool silence       = held < PS4_AUDIO_PERIOD;
      unsigned next_pos  = play_pos;

      if (silence)
         stats.underruns++;
      else
      {
         if (held < stats.min_held)
            stats.min_held = held;
         next_pos += PS4_AUDIO_PERIOD;
         if (next_pos >= ps4->ring)
            next_pos = 0;
      }

      /* Returns once the previous call's window has been output, with
       * this call's window queued for the hardware. */
      sceAudioOutOutput(ps4->port, silence
            ? ps4->buffer + ps4->ring
            : ps4->buffer + play_pos);

      now = sceKernelGetProcessTime();
      if (last_out)
      {
         unsigned gap = (unsigned)(now - last_out);
         if (gap > stats.max_gap_us)
            stats.max_gap_us = gap;
         if (gap > PS4_AUDIO_PERIOD_US * 3u / 2u)
            stats.late++;
      }
      last_out = now;
      stats.periods++;

      /* The previous window has been output; this call's is in flight,
       * so everything before play_pos is the writer's. */
      STORE_REL(&ps4->read_pos, play_pos);
      play_pos = next_pos;

      slock_lock(ps4->lock);
      scond_signal(ps4->cond);
      slock_unlock(ps4->lock);

      if (now - last_report >= PS4_AUDIO_REPORT_US)
      {
         /* Straight to the kernel log: the worker stays off stdio, whose
          * locking does not know about libkernel threads. */
         char line[256];
         snprintf(line, sizeof(line), "[PS4 audio]: periods %u, underruns %u, late %u, "
               "max gap %u us, min held %u/%u frames, writer waits %u\n",
               stats.periods, stats.underruns, stats.late,
               stats.max_gap_us, stats.min_held, ps4->ring,
               __atomic_load_n(&ps4->writer_waits, __ATOMIC_RELAXED));
         sceKernelDebugOutText(0, line);
         ps4_audio_stats_reset(&stats, ps4->ring);
         __atomic_store_n(&ps4->writer_waits, 0, __ATOMIC_RELAXED);
         last_report = now;
      }
   }
}

static bool ps4_audio_start_worker(ps4_audio_t *ps4)
{
   STORE_REL(&ps4->running, 1);
   ps4->worker_thread = sthread_create(ps4_audio_worker, ps4);
   if (!ps4->worker_thread)
   {
      STORE_REL(&ps4->running, 0);
      return false;
   }
   return true;
}

static void ps4_audio_stop_worker(ps4_audio_t *ps4)
{
   STORE_REL(&ps4->running, 0);
   slock_lock(ps4->lock);
   scond_signal(ps4->cond);
   slock_unlock(ps4->lock);
   if (ps4->worker_thread)
   {
      sthread_join(ps4->worker_thread);
      ps4->worker_thread = NULL;
   }
}

static void ps4_audio_free(void *data)
{
   ps4_audio_t *ps4 = (ps4_audio_t*)data;
   if (!ps4)
      return;

   ps4_audio_stop_worker(ps4);
   if (ps4->port >= 0)
      sceAudioOutClose(ps4->port);
   if (ps4->cond)
      scond_free(ps4->cond);
   if (ps4->lock)
      slock_free(ps4->lock);
   free(ps4->buffer);
   free(ps4);
}

static void *ps4_audio_init(const char *device,
      unsigned rate, unsigned latency,
      unsigned block_frames,
      unsigned *new_rate)
{
   ps4_audio_t *ps4 = (ps4_audio_t*)calloc(1, sizeof(*ps4));

   (void)device;
   (void)block_frames;

   if (!ps4)
      return NULL;

   ps4->port = -1;
   ps4->ring = ps4_ring_frames(latency);

   /* Every other rate is refused; resample to the one it takes */
   if (rate != PS4_AUDIO_RATE)
      *new_rate = PS4_AUDIO_RATE;

   ps4->buffer = (uint32_t*)calloc(ps4->ring + PS4_AUDIO_PERIOD,
         PS4_AUDIO_FRAME_BYTES);
   ps4->lock   = slock_new();
   ps4->cond   = scond_new();
   if (!ps4->buffer || !ps4->lock || !ps4->cond)
      goto error;

   /* Already initialised by the system is not an error here */
   sceAudioOutInit();
   ps4->port = sceAudioOutOpen(0xff, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0,
         PS4_AUDIO_PERIOD, PS4_AUDIO_RATE, SCE_AUDIO_OUT_MODE_STEREO);
   if (ps4->port < 0)
   {
      RARCH_ERR("[PS4 audio]: sceAudioOutOpen failed 0x%08X\n", ps4->port);
      goto error;
   }

   if (!ps4_audio_start_worker(ps4))
      goto error;

   RARCH_LOG("[PS4 audio]: ring %u frames (%u ms at %u Hz, latency setting %u ms), period %u\n",
         ps4->ring, ps4->ring * 1000u / PS4_AUDIO_RATE, PS4_AUDIO_RATE,
         latency, PS4_AUDIO_PERIOD);
   return ps4;

error:
   ps4_audio_free(ps4);
   return NULL;
}

static ssize_t ps4_audio_write(void *data, const void *buf, size_t size)
{
   ps4_audio_t *ps4     = (ps4_audio_t*)data;
   const uint32_t *src  = (const uint32_t*)buf;
   unsigned frames      = (unsigned)(size / PS4_AUDIO_FRAME_BYTES);
   unsigned written     = 0;

   /* Stopped (menu): drop rather than wait on a worker that is gone */
   if (!LOAD_ACQ(&ps4->running))
      return 0;

   while (written < frames)
   {
      unsigned write_pos = ps4->write_pos;
      unsigned room      = RING_FREE(write_pos,
            LOAD_ACQ(&ps4->read_pos), ps4->ring);
      unsigned chunk;

      if (!room)
      {
         if (ps4->nonblock)
            break;

         __atomic_fetch_add(&ps4->writer_waits, 1, __ATOMIC_RELAXED);
         slock_lock(ps4->lock);
         if (LOAD_ACQ(&ps4->running) && !RING_FREE(write_pos,
                  LOAD_ACQ(&ps4->read_pos), ps4->ring))
            scond_wait_timeout(ps4->cond, ps4->lock,
                  PS4_AUDIO_WRITE_WAIT_US);
         slock_unlock(ps4->lock);

         if (!LOAD_ACQ(&ps4->running))
            break;
         continue;
      }

      chunk = frames - written;
      if (chunk > room)
         chunk = room;
      if (chunk > ps4->ring - write_pos)
         chunk = ps4->ring - write_pos;

      memcpy(ps4->buffer + write_pos, src + written,
            chunk * PS4_AUDIO_FRAME_BYTES);
      write_pos += chunk;
      if (write_pos >= ps4->ring)
         write_pos = 0;
      STORE_REL(&ps4->write_pos, write_pos);
      written   += chunk;
   }

   return (ssize_t)(written * PS4_AUDIO_FRAME_BYTES);
}

static bool ps4_audio_alive(void *data)
{
   ps4_audio_t *ps4 = (ps4_audio_t*)data;
   return ps4 && LOAD_ACQ(&ps4->running);
}

static bool ps4_audio_stop(void *data)
{
   ps4_audio_t *ps4 = (ps4_audio_t*)data;
   if (ps4)
      ps4_audio_stop_worker(ps4);
   return true;
}

static bool ps4_audio_start(void *data, bool is_shutdown)
{
   ps4_audio_t *ps4 = (ps4_audio_t*)data;

   (void)is_shutdown;

   if (!ps4)
      return false;
   if (LOAD_ACQ(&ps4->running))
      return true;
   return ps4_audio_start_worker(ps4);
}

static void ps4_audio_set_nonblock_state(void *data, bool toggle)
{
   ps4_audio_t *ps4 = (ps4_audio_t*)data;
   if (ps4)
      ps4->nonblock = toggle;
}

static bool ps4_audio_use_float(void *data)
{
   (void)data;
   return false;
}

static size_t ps4_audio_write_avail(void *data)
{
   ps4_audio_t *ps4 = (ps4_audio_t*)data;
   if (!ps4)
      return 0;
   return RING_FREE(ps4->write_pos, LOAD_ACQ(&ps4->read_pos), ps4->ring)
      * PS4_AUDIO_FRAME_BYTES;
}

static size_t ps4_audio_buffer_size(void *data)
{
   ps4_audio_t *ps4 = (ps4_audio_t*)data;
   if (!ps4)
      return 0;
   return ps4->ring * PS4_AUDIO_FRAME_BYTES;
}

audio_driver_t audio_ps4 = {
   ps4_audio_init,
   ps4_audio_write,
   ps4_audio_stop,
   ps4_audio_start,
   ps4_audio_alive,
   ps4_audio_set_nonblock_state,
   ps4_audio_free,
   ps4_audio_use_float,
   "ps4",
   NULL,
   NULL,
   ps4_audio_write_avail,
   ps4_audio_buffer_size
};
