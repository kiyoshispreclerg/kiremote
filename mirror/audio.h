/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_AUDIO_H
#define KR_AUDIO_H

/* The phone's sound on this computer's speakers: raw PCM (signed 16 bit
 * little-endian, stereo, 48 kHz: what the scrcpy-server sends with
 * audio_codec=raw) into a PulseAudio playback stream (PipeWire answers the
 * same protocol). libpulse's own mainloop is driven from the caller's
 * poll(): kr_audio_poll() takes poll()'s place, so nothing blocks and no
 * thread is made.
 *
 * Latency matters more than completeness: what waits more than about
 * 250 ms (the loop was busy, the network stalled) is dropped, oldest first. */

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KR_AUDIO_RATE 48000
#define KR_AUDIO_FRAME 4 /* bytes: two channels of 16 bits */
/* The most we keep waiting for the server, in bytes. */
#define KR_AUDIO_MAX_QUEUE (KR_AUDIO_RATE / 4 * KR_AUDIO_FRAME)

typedef struct kr_audio kr_audio;

/* app: how the sound server names us; title: the stream (the phone's name).
 * NULL with *why (translated) if there is no sound server to connect to. */
kr_audio *kr_audio_new(const char *app, const char *title, const char **why);
void kr_audio_free(kr_audio *a);

/* false once the sound server went away or refused the stream: the caller
 * goes on without sound. */
bool kr_audio_ok(const kr_audio *a);

/* Queues PCM (any length; a half frame waits for its other half). */
void kr_audio_play(kr_audio *a, const uint8_t *pcm, size_t len);
/* Bytes queued here, not yet given to the sound server. */
size_t kr_audio_queued(const kr_audio *a);

/* poll() for the caller's descriptors plus the sound server's: the same
 * arguments and return value as poll(2) (errno too). */
int kr_audio_poll(kr_audio *a, struct pollfd *fds, nfds_t nfds, int timeout_ms);

#endif
