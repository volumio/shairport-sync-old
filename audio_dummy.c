/*
 * dummy output driver. This file is part of Shairport.
 * Copyright (c) James Laird 2013
 * All rights reserved.
 *
 * Modifications for audio synchronisation
 * and related work, copyright (c) Mike Brady 2014
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "audio.h"
#include "common.h"
#include <stdio.h>
#include <sys/time.h>
#include <unistd.h>

static int init(__attribute__((unused)) int argc, __attribute__((unused)) char **argv) { return 0; }

static void deinit(void) {}

static void start(int sample_rate, __attribute__((unused)) int sample_format) {
  debug(1, "dummy audio output started at Fs=%d Hz\n", sample_rate);
}

static int play(__attribute__((unused)) void *buf, __attribute__((unused)) int samples) {
  return 0;
}

static void stop(void) { debug(1, "dummy audio stopped\n"); }

audio_output audio_dummy = {.name = "dummy",
                            .help = NULL,
                            .init = &init,
                            .deinit = &deinit,
                            .prepare = NULL,
                            .start = &start,
                            .stop = &stop,
                            .is_running = NULL,
                            .flush = NULL,
                            .delay = NULL,
                            .play = &play,
                            .volume = NULL,
                            .parameters = NULL,
                            .mute = NULL};
