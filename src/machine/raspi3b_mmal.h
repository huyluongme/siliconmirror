/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Huy Luong Duc */

/*
 * The VideoCore's multimedia service, 'mmal', over VCHIQ, as the
 * firmware plays it for Linux's MMAL users as they probe: bcm2835-codec,
 * bcm2835-isp and bcm2835-camera (drivers/staging/vc04_services of
 * Raspberry Pi's Linux, at cff533a; vchiq-mmal's mmal-msg*.h give the
 * messages, mmal-parameters.h and mmal-encodings.h the values). The
 * service is version 15, from 10, those of Linux of the same release.
 * Each driver opens it once per device: five times for the codec, twice
 * for the ISP, once for the camera.
 *
 * A message starts with a header of 24 bytes: MMAL's magic ('mmal', its
 * first character in bits 7:0, as are all MMAL's fourccs), the type,
 * control_service, the Arm's context, a status and padding. The
 * VideoCore answers each of the Arm's with the same header, its status
 * 0, and the reply's body, whose first word is MMAL's status:
 *   COMPONENT_CREATE (4): a component by name; the reply gives its
 *     handle and its numbers of input, output and clock ports.
 *   COMPONENT_DESTROY (5): a component of the client's goes.
 *   PORT_INFO_GET (8): a port by type and index; the reply gives its
 *     handle, its mmal_port and its format.
 *   PORT_PARAMETER_GET (15): a parameter of a port: SUPPORTED_ENCODINGS
 *     (1) of an input or output port, CAMERA_INFO (0x1001c) of
 *     camera_info's control port. A parameter larger than the Arm's room
 *     answers ENOSPC (2), with its size and as much of it as fits, as
 *     mmal-vchiq.c's port_parameter_get expects.
 * When the Arm closes the service, the client's components go.
 *
 * The components, their ports (input, output) and encodings:
 *   ril.video_decode (1, 1): MPEG-4, H.264, H.263, MJPEG, VP8 in; I420,
 *     YV12, NV12, NV21, RGB565 out.
 *   ril.video_encode (1, 1): I420, YV12, NV12, NV21, RGB565, RGB24,
 *     BGR24, BGRA, YUYV, YVYU, UYVY, VYUY in; H.264, MJPEG out.
 *   ril.isp (1, 3): 53 Bayer, grey, YUV and RGB encodings in; 13 YUV and
 *     RGB, 8 YUV, and the statistics out.
 *   ril.image_fx (1, 1): I420 and SAND (YUVUV128) in and out.
 *   ril.image_encode (1, 1): the video encoder's 12 in; JPEG out.
 *   camera_info (0, 0): no camera and no flash, as on a board with no
 *     camera; bcm2835-camera then makes no device.
 * Where they come from: V4L2 lists each port's encodings that it knows,
 * in the firmware's order, and lists of real boards were found: the
 * ISP's input of a Zero 2 W (the Pi 3's VideoCore, Linux 6.6.74, 2025,
 * forum topic p=2342774), its outputs (topic t=306258, 2021); the
 * decoder's output of a Pi 3B+ and the encoder's (topic t=234778,
 * 2020), whose input a Pi 4 shows the same (topic t=294161). The
 * decoder's input there is a Pi 4's, H.264, MJPEG and VP8; the Pi 3 has
 * MPEG-4 and H.263 too (the Pi 4 has not, topic t=262558), and a Pi 3
 * of 2019 (topic t=234778) took MPEG-4 as the default, which is the
 * first: the rest of the order follows the Pi 4's. MPEG-2 and VC-1 need
 * a licence this board has not. Not found, and smirror's choice: the
 * image effects' (deinterlace) encodings, the image encoder's input
 * (the video encoder's), its output JPEG alone (Linux knows no other),
 * and the order of H.263.
 *
 * smirror's choices, where nothing documents the firmware: the reply's
 * header is the Arm's, padding and control_service too; component
 * handles count from 1, the lowest free, 16 components at a time; a
 * port's handle is its index among all the component's ports (control
 * 0, then inputs, then outputs), and found is 1; no component has clock
 * ports; a port is disabled, its buffers 1 (minimum, recommended and
 * number), sizes and alignment 0, its format video (the control port's
 * unknown) of its first encoding, all else 0; CAMERA_INFO is as large
 * as Linux's struct, 144 bytes.
 * Not modelled, and the machine stops: the other messages (enabling
 * components and ports, formats, buffers, the other parameters; streaming
 * as a whole), other components (ril.camera, the renderers), a message
 * without MMAL's magic or too short for its type, a handle or port that
 * is not the client's, a PORT_PARAMETER_GET room larger than a reply
 * holds, and more than 16 components.
 */

#ifndef MACHINE_RASPI3B_MMAL_H
#define MACHINE_RASPI3B_MMAL_H

#include "core/machine.h"
#include "machine/raspi3b_vchiq.h"

typedef struct Raspi3bMmal Raspi3bMmal;

/*
 * The service, added to v's. NULL when memory or v's room runs out.
 * When the firmware starts again (raspi3b_vchiq_reset closes every
 * service), it has no component.
 */
Raspi3bMmal *raspi3b_mmal_create(Machine *m, Raspi3bVchiq *v);
void raspi3b_mmal_destroy(Raspi3bMmal *s);

#endif /* MACHINE_RASPI3B_MMAL_H */
