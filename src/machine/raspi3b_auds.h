/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The VideoCore's audio service, VCHIQ's 'AUDS', as the Raspberry Pi
 * firmware plays it behind the headphone jack and HDMI.
 *
 * Nothing documents it but its user, Linux's snd-bcm2835
 * (drivers/staging/vc04_services/bcm2835-audio of Raspberry Pi's Linux,
 * at cff533a: vc_vchi_audioserv_defs.h and bcm2835-vchiq.c). Linux opens
 * 'AUDS', version 2 from 1, for each stream that a program opens; its
 * messages are struct vc_audio_msg, a type and a union, 20 bytes:
 *   OPEN, then CONTROL (the volume, in 1/256 dB of attenuation, and the
 *   destination: 0 auto, 1 headphones, 2 and 3 HDMI) and CONFIG (the
 *   channels, the rate and the bits a sample) before each stream;
 *   START; WRITE, the number of bytes of samples that follow, in DATA
 *   of at most max_packet bytes (or a bulk transfer when it is 0); STOP,
 *   at once or, with draining, at the end of what was written; CLOSE.
 * It waits for the RESULT of CONTROL, CONFIG and CLOSE, success 0, and
 * takes COMPLETE, the bytes played since the last COMPLETE with the
 * WRITE's two cookies, as the position of its stream; a COMPLETE with
 * bit 30 set says that the VideoCore has played all it was given, and
 * Linux stops the stream (an underrun). The VideoCore's version 2 tells
 * Linux to send the samples in DATA (max_packet 4000), not in bulk.
 *
 * smirror's VideoCore keeps what is written, at most 512 KB a stream
 * (the most Linux keeps, buffer_bytes_max), and from START plays it in
 * virtual time, at the stream's rate: every 10 ms, the frames whose
 * time has come, a COMPLETE of their bytes. When a block finds nothing
 * left, it sends a COMPLETE of bit 30 and the stream ends; STOP without
 * draining ends it at once, and drops what was not played, as does a
 * new CONFIG. The volume is applied: each sample times 10^(-dB/20),
 * rounded, clamped. A stream to the headphones, or auto (with no
 * display, the headphones), goes to the audio output; one to HDMI,
 * which has no display, goes nowhere, at the same pace.
 *
 * Bit 30 is from a Raspberry Pi engineer (raspberrypi/firmware issue
 * 696); the 10 ms, from Linux's comment "position update is in 10ms
 * order". The rest is smirror's choice: COMPLETE counts bytes, as Linux
 * reads it; the last samples are reported before the end, a block
 * later; RESULT and COMPLETE are 20 bytes; OPEN, START, STOP and WRITE,
 * which Linux does not wait for, have no answer; WRITE's silence is
 * not used.
 *
 * Not modelled, and the machine stops: a message before OPEN, a second
 * OPEN, a type that is not the Arm's, a message shorter than 20 bytes,
 * a CONFIG other than 1 to 8 channels, 8000 to 192000 Hz and 8 or 16
 * bits (channels 0 is IEC958 pass-through), a CONFIG or a START while
 * the stream plays, a START or a WRITE before CONFIG, a volume outside
 * Linux's (-1024 to 26214) or a destination above 3, a WRITE by bulk
 * transfer, DATA larger than its WRITE's max_packet or than the rest of
 * the WRITE, more than 512 KB of samples waiting, and two streams
 * playing to the output at once (mixing).
 */

#ifndef MACHINE_RASPI3B_AUDS_H
#define MACHINE_RASPI3B_AUDS_H

#include "core/audio.h"
#include "core/machine.h"
#include "machine/raspi3b_vchiq.h"

typedef struct Raspi3bAuds Raspi3bAuds;

/* The service, added to v, with no audio output; NULL when out of memory. */
Raspi3bAuds *raspi3b_auds_create(Machine *m, Raspi3bVchiq *v);
void raspi3b_auds_destroy(Raspi3bAuds *a);

/*
 * Where the headphones' sound goes: out, which the caller keeps, or
 * NULL for nothing plugged in. Set before the machine runs.
 */
void raspi3b_auds_set_output(Raspi3bAuds *a, AudioOut *out);

#endif /* MACHINE_RASPI3B_AUDS_H */
