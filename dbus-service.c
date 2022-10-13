/*
 * This file is part of Shairport Sync.
 * Copyright (c) Mike Brady 2018 -- 2019
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "config.h"

#include "common.h"
#include "player.h"
#include "rtsp.h"

#include "rtp.h"

#include "dacp.h"
#include "metadata_hub.h"

#include "dbus-service.h"

#ifdef CONFIG_CONVOLUTION
#include <FFTConvolver/convolver.h>
#endif

ShairportSync *shairportSyncSkeleton;

int service_is_running = 0;

ShairportSyncDiagnostics *shairportSyncDiagnosticsSkeleton = NULL;
ShairportSyncRemoteControl *shairportSyncRemoteControlSkeleton = NULL;
ShairportSyncAdvancedRemoteControl *shairportSyncAdvancedRemoteControlSkeleton = NULL;

guint ownerID = 0;

void dbus_metadata_watcher(struct metadata_bundle *argc, __attribute__((unused)) void *userdata) {
  char response[100];
  gboolean current_status, new_status;

  const char *th;
  shairport_sync_advanced_remote_control_set_volume(shairportSyncAdvancedRemoteControlSkeleton,
                                                    argc->speaker_volume);

  shairport_sync_remote_control_set_airplay_volume(shairportSyncRemoteControlSkeleton,
                                                   argc->airplay_volume);

  shairport_sync_remote_control_set_client(shairportSyncRemoteControlSkeleton, argc->client_ip);


  // although it's a DACP server, the server is in fact, part of the the AirPlay "client" (their term).
  if (argc->dacp_server_active) {
    shairport_sync_remote_control_set_available(shairportSyncRemoteControlSkeleton, TRUE);
  } else {
    shairport_sync_remote_control_set_available(shairportSyncRemoteControlSkeleton, FALSE);
  }

  if (argc->advanced_dacp_server_active) {
    shairport_sync_advanced_remote_control_set_available(shairportSyncAdvancedRemoteControlSkeleton,
                                                         TRUE);
  } else {
    shairport_sync_advanced_remote_control_set_available(shairportSyncAdvancedRemoteControlSkeleton,
                                                         FALSE);
  }

  if (argc->progress_string) {
    // debug(1, "Check progress string");
    th = shairport_sync_remote_control_get_progress_string(shairportSyncRemoteControlSkeleton);
    if ((th == NULL) || (strcasecmp(th, argc->progress_string) != 0)) {
      // debug(1, "Progress string should be changed");
      shairport_sync_remote_control_set_progress_string(shairportSyncRemoteControlSkeleton,
                                                        argc->progress_string);
    }
  }

  switch (argc->player_state) {
  case PS_NOT_AVAILABLE:
    shairport_sync_remote_control_set_player_state(shairportSyncRemoteControlSkeleton,
                                                   "Not Available");
    break;
  case PS_STOPPED:
    shairport_sync_remote_control_set_player_state(shairportSyncRemoteControlSkeleton, "Stopped");
    break;
  case PS_PAUSED:
    shairport_sync_remote_control_set_player_state(shairportSyncRemoteControlSkeleton, "Paused");
    break;
  case PS_PLAYING:
    shairport_sync_remote_control_set_player_state(shairportSyncRemoteControlSkeleton, "Playing");
    break;
  default:
    debug(1, "This should never happen.");
  }

  switch (argc->play_status) {
  case PS_NOT_AVAILABLE:
    strcpy(response, "Not Available");
    break;
  case PS_STOPPED:
    strcpy(response, "Stopped");
    break;
  case PS_PAUSED:
    strcpy(response, "Paused");
    break;
  case PS_PLAYING:
    strcpy(response, "Playing");
    break;
  default:
    debug(1, "This should never happen.");
  }

  th = shairport_sync_advanced_remote_control_get_playback_status(
      shairportSyncAdvancedRemoteControlSkeleton);

  // only set this if it's different
  if ((th == NULL) || (strcasecmp(th, response) != 0)) {
    debug(3, "Playback Status should be changed");
    shairport_sync_advanced_remote_control_set_playback_status(
        shairportSyncAdvancedRemoteControlSkeleton, response);
  }

  switch (argc->repeat_status) {
  case RS_NOT_AVAILABLE:
    strcpy(response, "Not Available");
    break;
  case RS_OFF:
    strcpy(response, "Off");
    break;
  case RS_ONE:
    strcpy(response, "One");
    break;
  case RS_ALL:
    strcpy(response, "All");
    break;
  default:
    debug(1, "This should never happen.");
  }
  th = shairport_sync_advanced_remote_control_get_loop_status(
      shairportSyncAdvancedRemoteControlSkeleton);

  // only set this if it's different
  if ((th == NULL) || (strcasecmp(th, response) != 0)) {
    debug(3, "Loop Status should be changed");
    shairport_sync_advanced_remote_control_set_loop_status(
        shairportSyncAdvancedRemoteControlSkeleton, response);
  }


  switch (argc->shuffle_status) {
  case SS_NOT_AVAILABLE:
    new_status = FALSE;
    break;
  case SS_OFF:
    new_status = FALSE;
    break;
  case SS_ON:
    new_status = TRUE;
    break;
  default:
    new_status = FALSE;
    debug(1, "Unknown shuffle status -- this should never happen.");
  }

  current_status = shairport_sync_advanced_remote_control_get_shuffle(
      shairportSyncAdvancedRemoteControlSkeleton);

  // only set this if it's different
  if (current_status != new_status) {
    debug(3, "Shuffle State should be changed");
    shairport_sync_advanced_remote_control_set_shuffle(
        shairportSyncAdvancedRemoteControlSkeleton, new_status);
  }

  // Build the metadata array
  debug(2, "Build metadata");
  GVariantBuilder *dict_builder = g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));

  // Add in the artwork URI if it exists.
  if (argc->cover_art_pathname) {
    GVariant *artUrl = g_variant_new("s", argc->cover_art_pathname);
    g_variant_builder_add(dict_builder, "{sv}", "mpris:artUrl", artUrl);
  }

  // Add in the Track ID based on the 'mper' metadata if it is non-zero
  if (argc->item_id != 0) {
    char trackidstring[128];
    snprintf(trackidstring, sizeof(trackidstring), "/org/gnome/ShairportSync/%" PRIX64 "",
             argc->item_id);
    GVariant *trackid = g_variant_new("o", trackidstring);
    g_variant_builder_add(dict_builder, "{sv}", "mpris:trackid", trackid);
  }

  // Add the track name if it exists
  if (argc->track_name) {
    GVariant *track_name = g_variant_new("s", argc->track_name);
    g_variant_builder_add(dict_builder, "{sv}", "xesam:title", track_name);
  }

  // Add the album name if it exists
  if (argc->album_name) {
    GVariant *album_name = g_variant_new("s", argc->album_name);
    g_variant_builder_add(dict_builder, "{sv}", "xesam:album", album_name);
  }

  // Add the artist name if it exists
  if (argc->artist_name) {
    GVariantBuilder *artist_as = g_variant_builder_new(G_VARIANT_TYPE("as"));
    g_variant_builder_add(artist_as, "s", argc->artist_name);
    GVariant *artists = g_variant_builder_end(artist_as);
    g_variant_builder_unref(artist_as);
    g_variant_builder_add(dict_builder, "{sv}", "xesam:artist", artists);
  }

  // Add the genre if it exists
  if (argc->genre) {
    GVariantBuilder *genre_as = g_variant_builder_new(G_VARIANT_TYPE("as"));
    g_variant_builder_add(genre_as, "s", argc->genre);
    GVariant *genre = g_variant_builder_end(genre_as);
    g_variant_builder_unref(genre_as);
    g_variant_builder_add(dict_builder, "{sv}", "xesam:genre", genre);
  }

  if (argc->songtime_in_milliseconds) {
    uint64_t track_length_in_microseconds = argc->songtime_in_milliseconds;
    track_length_in_microseconds *= 1000; // to microseconds in 64-bit precision
                                          // Make up the track name and album name
    // debug(1, "Set tracklength to %lu.", track_length_in_microseconds);
    GVariant *tracklength = g_variant_new("x", track_length_in_microseconds);
    g_variant_builder_add(dict_builder, "{sv}", "mpris:length", tracklength);
  }

  GVariant *dict = g_variant_builder_end(dict_builder);
  g_variant_builder_unref(dict_builder);
  shairport_sync_remote_control_set_metadata(shairportSyncRemoteControlSkeleton, dict);
}

static gboolean on_handle_set_volume(ShairportSyncAdvancedRemoteControl *skeleton,
                                     GDBusMethodInvocation *invocation, const gint volume,
                                     __attribute__((unused)) gpointer user_data) {
  debug(2, "Set volume to %d.", volume);
  dacp_set_volume(volume);
  shairport_sync_advanced_remote_control_complete_set_volume(skeleton, invocation);
  return TRUE;
}

static gboolean on_handle_fast_forward(ShairportSyncRemoteControl *skeleton,
                                       GDBusMethodInvocation *invocation,
                                       __attribute__((unused)) gpointer user_data) {
  send_simple_dacp_command("beginff");
  shairport_sync_remote_control_complete_fast_forward(skeleton, invocation);
  return TRUE;
}

static gboolean on_handle_rewind(ShairportSyncRemoteControl *skeleton,
                                 GDBusMethodInvocation *invocation,
                                 __attribute__((unused)) gpointer user_data) {
  send_simple_dacp_command("beginrew");
  shairport_sync_remote_control_complete_rewind(skeleton, invocation);
  return TRUE;
}

static gboolean on_handle_toggle_mute(ShairportSyncRemoteControl *skeleton,
                                      GDBusMethodInvocation *invocation,
                                      __attribute__((unused)) gpointer user_data) {
  send_simple_dacp_command("mutetoggle");
  shairport_sync_remote_control_complete_toggle_mute(skeleton, invocation);
  return TRUE;
}

static gboolean on_handle_next(ShairportSyncRemoteControl *skeleton,
                               GDBusMethodInvocation *invocation,
                               __attribute__((unused)) gpointer user_data) {
  send_simple_dacp_command("nextitem");
  shairport_sync_remote_control_complete_next(skeleton, invocation);
  return TRUE;
}

static gboolean on_handle_previous(ShairportSyncRemoteControl *skeleton,
                                   GDBusMethodInvocation *invocation,
                                   __attribute__((unused)) gpointer user_data) {
  send_simple_dacp_command("previtem");
  shairport_sync_remote_control_complete_previous(skeleton, invocation);
  return TRUE;
}

static gboolean on_handle_pause(ShairportSyncRemoteControl *skeleton,
                                GDBusMethodInvocation *invocation,
                                __attribute__((unused)) gpointer user_data) {
  send_simple_dacp_command("pause");
  shairport_sync_remote_control_complete_pause(skeleton, invocation);
  return TRUE;
}

static gboolean on_handle_play_pause(ShairportSyncRemoteControl *skeleton,
                                     GDBusMethodInvocation *invocation,
                                     __attribute__((unused)) gpointer user_data) {
  send_simple_dacp_command("playpause");
  shairport_sync_remote_control_complete_play_pause(skeleton, invocation);
  return TRUE;
}

static gboolean on_handle_play(ShairportSyncRemoteControl *skeleton,
                               GDBusMethodInvocation *invocation,
                               __attribute__((unused)) gpointer user_data) {
  send_simple_dacp_command("play");
  shairport_sync_remote_control_complete_play(skeleton, invocation);
  return TRUE;
}

static gboolean on_handle_stop(ShairportSyncRemoteControl *skeleton,
                               GDBusMethodInvocation *invocation,
                               __attribute__((unused)) gpointer user_data) {
  send_simple_dacp_command("stop");
  shairport_sync_remote_control_complete_stop(skeleton, invocation);
  return TRUE;
}

static gboolean on_handle_resume(ShairportSyncRemoteControl *skeleton,
                                 GDBusMethodInvocation *invocation,
                                 __attribute__((unused)) gpointer user_data) {
  send_simple_dacp_command("playresume");
  shairport_sync_remote_control_complete_resume(skeleton, invocation);
  return TRUE;
}

static gboolean on_handle_shuffle_songs(ShairportSyncRemoteControl *skeleton,
                                        GDBusMethodInvocation *invocation,
                                        __attribute__((unused)) gpointer user_data) {
  send_simple_dacp_command("shuffle_songs");
  shairport_sync_remote_control_complete_shuffle_songs(skeleton, invocation);
  return TRUE;
}

static gboolean on_handle_volume_up(ShairportSyncRemoteControl *skeleton,
                                    GDBusMethodInvocation *invocation,
                                    __attribute__((unused)) gpointer user_data) {
  send_simple_dacp_command("volumeup");
  shairport_sync_remote_control_complete_volume_up(skeleton, invocation);
  return TRUE;
}

static gboolean on_handle_volume_down(ShairportSyncRemoteControl *skeleton,
                                      GDBusMethodInvocation *invocation,
                                      __attribute__((unused)) gpointer user_data) {
  send_simple_dacp_command("volumedown");
  shairport_sync_remote_control_complete_volume_down(skeleton, invocation);
  return TRUE;
}

static gboolean on_handle_set_airplay_volume(ShairportSyncRemoteControl *skeleton,
                                     GDBusMethodInvocation *invocation, const gdouble volume,
                                     __attribute__((unused)) gpointer user_data) {
  debug(2, "Set airplay volume to %.6f.", volume);
  char command[256] = "";
  snprintf(command, sizeof(command), "setproperty?dmcp.device-volume=%.6f", volume);
  send_simple_dacp_command(command);
  shairport_sync_remote_control_complete_set_airplay_volume(skeleton, invocation);
  return TRUE;
}



gboolean notify_elapsed_time_callback(ShairportSyncDiagnostics *skeleton,
                                      __attribute__((unused)) gpointer user_data) {
  // debug(1, "\"notify_elapsed_time_callback\" called.");
  if (shairport_sync_diagnostics_get_elapsed_time(skeleton)) {
    config.debugger_show_elapsed_time = 1;
    debug(1, ">> start including elapsed time in logs");
  } else {
    config.debugger_show_elapsed_time = 0;
    debug(1, ">> stop including elapsed time in logs");
  }
  return TRUE;
}

gboolean notify_delta_time_callback(ShairportSyncDiagnostics *skeleton,
                                    __attribute__((unused)) gpointer user_data) {
  // debug(1, "\"notify_delta_time_callback\" called.");
  if (shairport_sync_diagnostics_get_delta_time(skeleton)) {
    config.debugger_show_relative_time = 1;
    debug(1, ">> start including delta time in logs");
  } else {
    config.debugger_show_relative_time = 0;
    debug(1, ">> stop including delta time in logs");
  }
  return TRUE;
}

gboolean notify_file_and_line_callback(ShairportSyncDiagnostics *skeleton,
                                    __attribute__((unused)) gpointer user_data) {
  // debug(1, "\"notify_file_and_line_callback\" called.");
  if (shairport_sync_diagnostics_get_file_and_line(skeleton)) {
    config.debugger_show_file_and_line = 1;
    debug(1, ">> start including file and line in logs");
  } else {
    config.debugger_show_file_and_line = 0;
    debug(1, ">> stop including file and line in logs");
  }
  return TRUE;
}

gboolean notify_statistics_callback(ShairportSyncDiagnostics *skeleton,
                                    __attribute__((unused)) gpointer user_data) {
  // debug(1, "\"notify_statistics_callback\" called.");
  if (shairport_sync_diagnostics_get_statistics(skeleton)) {
    debug(1, ">> start logging statistics");
    config.statistics_requested = 1;
  } else {
    debug(1, ">> stop logging statistics");
    config.statistics_requested = 0;
  }
  return TRUE;
}

gboolean notify_verbosity_callback(ShairportSyncDiagnostics *skeleton,
                                   __attribute__((unused)) gpointer user_data) {
  gint th = shairport_sync_diagnostics_get_verbosity(skeleton);
  if ((th >= 0) && (th <= 3)) {
    if (th == 0)
      debug(1, ">> log verbosity set to %d.", th);
    debuglev = th;
    debug(1, ">> log verbosity set to %d.", th);
  } else {
    debug(1, ">> invalid log verbosity: %d. Ignored.", th);
    shairport_sync_diagnostics_set_verbosity(skeleton, debuglev);
  }
  return TRUE;
}

gboolean notify_disable_standby_callback(ShairportSync *skeleton,
                                         __attribute__((unused)) gpointer user_data) {
  // debug(1, "\"notify_disable_standby_callback\" called.");
  if (shairport_sync_get_disable_standby(skeleton)) {
    debug(1, ">> activating disable standby");
    config.keep_dac_busy = 1;
  } else {
    debug(1, ">> deactivating disable standby");
    config.keep_dac_busy = 0;
  }
  return TRUE;
}

#ifdef CONFIG_CONVOLUTION
gboolean notify_convolution_callback(ShairportSync *skeleton,
                                                __attribute__((unused)) gpointer user_data) {
  // debug(1, "\"notify_convolution_callback\" called.");
  if (shairport_sync_get_convolution(skeleton)) {
    debug(1, ">> activating convolution");
    config.convolution = 1;
    config.convolver_valid = convolver_init(config.convolution_ir_file, config.convolution_max_length);
  } else {
    debug(1, ">> deactivating convolution");
    config.convolution = 0;
  }
  return TRUE;
}
#else
gboolean notify_convolution_callback(__attribute__((unused)) ShairportSync *skeleton,
                                                __attribute__((unused)) gpointer user_data) {
  warn(">> Convolution support is not built in to this build of Shairport Sync.");
  return TRUE;
}
#endif

#ifdef CONFIG_CONVOLUTION
gboolean notify_convolution_gain_callback(ShairportSync *skeleton,
                                            __attribute__((unused)) gpointer user_data) {

  gdouble th = shairport_sync_get_convolution_gain(skeleton);
  if ((th <= 0.0) && (th >= -100.0)) {
    debug(1, ">> setting convolution gain to %f.", th);
    config.convolution_gain = th;
  } else {
    debug(1, ">> invalid convolution gain: %f. Ignored.", th);
    shairport_sync_set_convolution_gain(skeleton, config.convolution_gain);
  }
  return TRUE;
}
#else
gboolean notify_convolution_gain_callback(__attribute__((unused)) ShairportSync *skeleton,
                                                __attribute__((unused)) gpointer user_data) {
  warn(">> Convolution support is not built in to this build of Shairport Sync.");
  return TRUE;
}
#endif
#ifdef CONFIG_CONVOLUTION
gboolean notify_convolution_impulse_response_file_callback(ShairportSync *skeleton,
                                                __attribute__((unused)) gpointer user_data) {
  char *th = (char *)shairport_sync_get_convolution_impulse_response_file(skeleton);
  if (config.convolution_ir_file)
    free(config.convolution_ir_file);
  config.convolution_ir_file = strdup(th);
  debug(1, ">> setting configuration impulse response filter file to \"%s\".", config.convolution_ir_file);
  config.convolver_valid = convolver_init(config.convolution_ir_file, config.convolution_max_length);
  return TRUE;
}
#else
gboolean notify_convolution_impulse_response_file_callback(__attribute__((unused)) ShairportSync *skeleton,
                                                __attribute__((unused)) gpointer user_data) {
  __attribute__((unused)) char *th = (char *)shairport_sync_get_convolution_impulse_response_file(skeleton);
  return TRUE;
}
#endif



gboolean notify_loudness_callback(ShairportSync *skeleton,
                                                __attribute__((unused)) gpointer user_data) {
  // debug(1, "\"notify_loudness_callback\" called.");
  if (shairport_sync_get_loudness(skeleton)) {
    debug(1, ">> activating loudness");
    config.loudness = 1;
  } else {
    debug(1, ">> deactivating loudness");
    config.loudness = 0;
  }
  return TRUE;
}

gboolean notify_loudness_threshold_callback(ShairportSync *skeleton,
                                            __attribute__((unused)) gpointer user_data) {
  gdouble th = shairport_sync_get_loudness_threshold(skeleton);
  if ((th <= 0.0) && (th >= -100.0)) {
    debug(1, ">> setting loudness threshold to %f.", th);
    config.loudness_reference_volume_db = th;
  } else {
    debug(1, ">> invalid loudness threshold: %f. Ignored.", th);
    shairport_sync_set_loudness_threshold(skeleton, config.loudness_reference_volume_db);
  }
  return TRUE;
}

gboolean notify_drift_tolerance_callback(ShairportSync *skeleton,
                                         __attribute__((unused)) gpointer user_data) {
  gdouble dt = shairport_sync_get_drift_tolerance(skeleton);
  if ((dt >= 0.0) && (dt <= 2.0)) {
    debug(1, ">> setting drift tolerance to %f seconds", dt);
    config.tolerance = dt;
  } else {
    debug(1, ">> invalid drift tolerance: %f seconds. Ignored.", dt);
    shairport_sync_set_drift_tolerance(skeleton, config.tolerance);
  }
  return TRUE;
}

gboolean notify_disable_standby_mode_callback(ShairportSync *skeleton,
                                              __attribute__((unused)) gpointer user_data) {
  char *th = (char *)shairport_sync_get_disable_standby_mode(skeleton);
  if ((strcasecmp(th, "no") == 0) || (strcasecmp(th, "off") == 0) ||
      (strcasecmp(th, "never") == 0)) {
    config.disable_standby_mode = disable_standby_off;
    config.keep_dac_busy = 0;
  } else if ((strcasecmp(th, "yes") == 0) || (strcasecmp(th, "on") == 0) ||
             (strcasecmp(th, "always") == 0)) {
    config.disable_standby_mode = disable_standby_always;
    config.keep_dac_busy = 1;
  } else if (strcasecmp(th, "auto") == 0)
    config.disable_standby_mode = disable_standby_auto;
  else {
    warn("An unrecognised disable_standby_mode: \"%s\" was requested via D-Bus interface.", th);
    switch (config.disable_standby_mode) {
    case disable_standby_off:
      shairport_sync_set_disable_standby_mode(skeleton, "off");
      break;
    case disable_standby_always:
      shairport_sync_set_disable_standby_mode(skeleton, "always");
      break;
    case disable_standby_auto:
      shairport_sync_set_disable_standby_mode(skeleton, "auto");
      break;
    default:
      break;
    }
  }
  return TRUE;
}

gboolean notify_alacdecoder_callback(ShairportSync *skeleton,
                                     __attribute__((unused)) gpointer user_data) {
  char *th = (char *)shairport_sync_get_alacdecoder(skeleton);
#ifdef CONFIG_APPLE_ALAC
  if (strcasecmp(th, "hammerton") == 0)
    config.use_apple_decoder = 0;
  else if (strcasecmp(th, "apple") == 0)
    config.use_apple_decoder = 1;
  else {
    warn("An unrecognised ALAC decoder: \"%s\" was requested via D-Bus interface.", th);
    if (config.use_apple_decoder == 0)
      shairport_sync_set_alacdecoder(skeleton, "hammerton");
    else
      shairport_sync_set_alacdecoder(skeleton, "apple");
  }
// debug(1,"Using the %s ALAC decoder.", ((config.use_apple_decoder==0) ? "Hammerton" : "Apple"));
#else
  if (strcasecmp(th, "hammerton") == 0) {
    config.use_apple_decoder = 0;
    // debug(1,"Using the Hammerton ALAC decoder.");
  } else {
    warn("An unrecognised ALAC decoder: \"%s\" was requested via D-Bus interface. (Possibly "
         "support for this decoder was not compiled "
         "into this version of Shairport Sync.)",
         th);
    shairport_sync_set_alacdecoder(skeleton, "hammerton");
  }
#endif
  return TRUE;
}

gboolean notify_interpolation_callback(ShairportSync *skeleton,
                                       __attribute__((unused)) gpointer user_data) {
  char *th = (char *)shairport_sync_get_interpolation(skeleton);
#ifdef CONFIG_SOXR
  if (strcasecmp(th, "basic") == 0)
    config.packet_stuffing = ST_basic;
  else if (strcasecmp(th, "soxr") == 0)
    config.packet_stuffing = ST_soxr;
  else if (strcasecmp(th, "auto") == 0)
    config.packet_stuffing = ST_auto;
  else {
    warn("An unrecognised interpolation method: \"%s\" was requested via the D-Bus interface.", th);
    switch (config.packet_stuffing) {
    case ST_basic:
      shairport_sync_set_interpolation(skeleton, "basic");
      break;
    case ST_soxr:
      shairport_sync_set_interpolation(skeleton, "soxr");
      break;
    case ST_auto:
      shairport_sync_set_interpolation(skeleton, "auto");
      break;
    default:
      debug(1, "This should never happen!");
      shairport_sync_set_interpolation(skeleton, "basic");
      break;
    }
  }
#else
  if (strcasecmp(th, "basic") == 0)
    config.packet_stuffing = ST_basic;
  else {
    warn("An unrecognised interpolation method: \"%s\" was requested via the D-Bus interface. "
         "(Possibly support for this method was not compiled "
         "into this version of Shairport Sync.)",
         th);
    shairport_sync_set_interpolation(skeleton, "basic");
  }
#endif
  return TRUE;
}

gboolean notify_volume_control_profile_callback(ShairportSync *skeleton,
                                                __attribute__((unused)) gpointer user_data) {
  char *th = (char *)shairport_sync_get_volume_control_profile(skeleton);
  //  enum volume_control_profile_type previous_volume_control_profile =
  //  config.volume_control_profile;
  if (strcasecmp(th, "standard") == 0)
    config.volume_control_profile = VCP_standard;
  else if (strcasecmp(th, "flat") == 0)
    config.volume_control_profile = VCP_flat;
  else {
    warn("Unrecognised Volume Control Profile: \"%s\".", th);
    switch (config.volume_control_profile) {
    case VCP_standard:
      shairport_sync_set_volume_control_profile(skeleton, "standard");
      break;
    case VCP_flat:
      shairport_sync_set_volume_control_profile(skeleton, "flat");
      break;
    default:
      debug(1, "This should never happen!");
      shairport_sync_set_volume_control_profile(skeleton, "standard");
      break;
    }
  }
  return TRUE;
}

gboolean notify_shuffle_callback(ShairportSyncAdvancedRemoteControl *skeleton,
                                 __attribute__((unused)) gpointer user_data) {
  // debug(1,"notify_shuffle_callback called");
  if (shairport_sync_advanced_remote_control_get_shuffle(skeleton))
    send_simple_dacp_command("setproperty?dacp.shufflestate=1");
  else
    send_simple_dacp_command("setproperty?dacp.shufflestate=0");
  return TRUE;
}

gboolean notify_loop_status_callback(ShairportSyncAdvancedRemoteControl *skeleton,
                                     __attribute__((unused)) gpointer user_data) {
  // debug(1,"notify_loop_status_callback called");
  char *th = (char *)shairport_sync_advanced_remote_control_get_loop_status(skeleton);
  //  enum volume_control_profile_type previous_volume_control_profile =
  //  config.volume_control_profile;
  // debug(1, "notify_loop_status_callback called with loop status of \"%s\".", th);
  if (strcasecmp(th, "off") == 0)
    send_simple_dacp_command("setproperty?dacp.repeatstate=0");
  else if (strcasecmp(th, "one") == 0)
    send_simple_dacp_command("setproperty?dacp.repeatstate=1");
  else if (strcasecmp(th, "all") == 0)
    send_simple_dacp_command("setproperty?dacp.repeatstate=2");
  else if (strcasecmp(th, "not available") != 0) {
    warn("Illegal Loop Request: \"%s\".", th);
    switch (metadata_store.repeat_status) {
    case RS_NOT_AVAILABLE:
      shairport_sync_advanced_remote_control_set_loop_status(skeleton, "Not Available");
      break;
    case RS_OFF:
      shairport_sync_advanced_remote_control_set_loop_status(skeleton, "Off");
      break;
    case RS_ONE:
      shairport_sync_advanced_remote_control_set_loop_status(skeleton, "One");
      break;
    case RS_ALL:
      shairport_sync_advanced_remote_control_set_loop_status(skeleton, "All");
      break;
    default:
      debug(1, "This should never happen!");
      shairport_sync_advanced_remote_control_set_loop_status(skeleton, "Off");
      break;
    }
  }
  return TRUE;
}

static gboolean on_handle_quit(ShairportSync *skeleton, GDBusMethodInvocation *invocation,
                               __attribute__((unused)) const gchar *command,
                               __attribute__((unused)) gpointer user_data) {
  debug(1, "quit requested (native interface)");
  if (main_thread_id)
    debug(1, "Cancelling main thread results in %d.", pthread_cancel(main_thread_id));
  else
    debug(1, "Main thread ID is NULL.");
  shairport_sync_complete_quit(skeleton, invocation);
  return TRUE;
}

static gboolean on_handle_remote_command(ShairportSync *skeleton, GDBusMethodInvocation *invocation,
                                         const gchar *command,
                                         __attribute__((unused)) gpointer user_data) {
  debug(1, "RemoteCommand with command \"%s\".", command);
  int reply = 0;
  char *client_reply = NULL;
  ssize_t reply_size = 0;
  reply = dacp_send_command((const char *)command, &client_reply, &reply_size);
  char *client_reply_hex = alloca(reply_size * 2 + 1);
  if (client_reply_hex) {
    char *p = client_reply_hex;
    if (client_reply) {
      char *q = client_reply;
      int i;
      for (i = 0; i < reply_size; i++) {
        snprintf(p, 3, "%02X", *q);
        p += 2;
        q++;
      }
    }
    *p = '\0';
  }
  shairport_sync_complete_remote_command(skeleton, invocation, reply, client_reply_hex);
  return TRUE;
}


static void on_dbus_name_acquired(GDBusConnection *connection, const gchar *name,
                                  __attribute__((unused)) gpointer user_data) {

  // debug(1, "Shairport Sync native D-Bus interface \"%s\" acquired on the %s bus.", name,
  // (config.dbus_service_bus_type == DBT_session) ? "session" : "system");

  shairportSyncSkeleton = shairport_sync_skeleton_new();
  g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(shairportSyncSkeleton), connection,
                                   "/org/gnome/ShairportSync", NULL);

  shairportSyncDiagnosticsSkeleton = shairport_sync_diagnostics_skeleton_new();
  g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(shairportSyncDiagnosticsSkeleton),
                                   connection, "/org/gnome/ShairportSync", NULL);

  shairportSyncRemoteControlSkeleton = shairport_sync_remote_control_skeleton_new();
  g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(shairportSyncRemoteControlSkeleton),
                                   connection, "/org/gnome/ShairportSync", NULL);

  shairportSyncAdvancedRemoteControlSkeleton =
      shairport_sync_advanced_remote_control_skeleton_new();

  g_dbus_interface_skeleton_export(
      G_DBUS_INTERFACE_SKELETON(shairportSyncAdvancedRemoteControlSkeleton), connection,
      "/org/gnome/ShairportSync", NULL);

  g_signal_connect(shairportSyncSkeleton, "notify::interpolation",
                   G_CALLBACK(notify_interpolation_callback), NULL);
  g_signal_connect(shairportSyncSkeleton, "notify::alacdecoder",
                   G_CALLBACK(notify_alacdecoder_callback), NULL);
  g_signal_connect(shairportSyncSkeleton, "notify::disable-standby-mode",
                   G_CALLBACK(notify_disable_standby_mode_callback), NULL);
  g_signal_connect(shairportSyncSkeleton, "notify::volume-control-profile",
                   G_CALLBACK(notify_volume_control_profile_callback), NULL);
  g_signal_connect(shairportSyncSkeleton, "notify::disable-standby",
                   G_CALLBACK(notify_disable_standby_callback), NULL);
  g_signal_connect(shairportSyncSkeleton, "notify::convolution",
                   G_CALLBACK(notify_convolution_callback), NULL);
  g_signal_connect(shairportSyncSkeleton, "notify::convolution-gain",
                   G_CALLBACK(notify_convolution_gain_callback), NULL);
  g_signal_connect(shairportSyncSkeleton, "notify::convolution-impulse-response-file",
                   G_CALLBACK(notify_convolution_impulse_response_file_callback), NULL);
  g_signal_connect(shairportSyncSkeleton, "notify::loudness",
                   G_CALLBACK(notify_loudness_callback), NULL);
  g_signal_connect(shairportSyncSkeleton, "notify::loudness-threshold",
                   G_CALLBACK(notify_loudness_threshold_callback), NULL);
  g_signal_connect(shairportSyncSkeleton, "notify::drift-tolerance",
                   G_CALLBACK(notify_drift_tolerance_callback), NULL);

  g_signal_connect(shairportSyncSkeleton, "handle-quit", G_CALLBACK(on_handle_quit), NULL);

  g_signal_connect(shairportSyncSkeleton, "handle-remote-command",
                   G_CALLBACK(on_handle_remote_command), NULL);

  g_signal_connect(shairportSyncDiagnosticsSkeleton, "notify::verbosity",
                   G_CALLBACK(notify_verbosity_callback), NULL);

  g_signal_connect(shairportSyncDiagnosticsSkeleton, "notify::statistics",
                   G_CALLBACK(notify_statistics_callback), NULL);

  g_signal_connect(shairportSyncDiagnosticsSkeleton, "notify::elapsed-time",
                   G_CALLBACK(notify_elapsed_time_callback), NULL);

  g_signal_connect(shairportSyncDiagnosticsSkeleton, "notify::delta-time",
                   G_CALLBACK(notify_delta_time_callback), NULL);

  g_signal_connect(shairportSyncDiagnosticsSkeleton, "notify::file-and-line",
                   G_CALLBACK(notify_file_and_line_callback), NULL);

  g_signal_connect(shairportSyncRemoteControlSkeleton, "handle-fast-forward",
                   G_CALLBACK(on_handle_fast_forward), NULL);
  g_signal_connect(shairportSyncRemoteControlSkeleton, "handle-rewind",
                   G_CALLBACK(on_handle_rewind), NULL);
  g_signal_connect(shairportSyncRemoteControlSkeleton, "handle-toggle-mute",
                   G_CALLBACK(on_handle_toggle_mute), NULL);
  g_signal_connect(shairportSyncRemoteControlSkeleton, "handle-next", G_CALLBACK(on_handle_next),
                   NULL);
  g_signal_connect(shairportSyncRemoteControlSkeleton, "handle-previous",
                   G_CALLBACK(on_handle_previous), NULL);
  g_signal_connect(shairportSyncRemoteControlSkeleton, "handle-pause", G_CALLBACK(on_handle_pause),
                   NULL);
  g_signal_connect(shairportSyncRemoteControlSkeleton, "handle-play-pause",
                   G_CALLBACK(on_handle_play_pause), NULL);
  g_signal_connect(shairportSyncRemoteControlSkeleton, "handle-play", G_CALLBACK(on_handle_play),
                   NULL);
  g_signal_connect(shairportSyncRemoteControlSkeleton, "handle-stop", G_CALLBACK(on_handle_stop),
                   NULL);
  g_signal_connect(shairportSyncRemoteControlSkeleton, "handle-resume",
                   G_CALLBACK(on_handle_resume), NULL);
  g_signal_connect(shairportSyncRemoteControlSkeleton, "handle-shuffle-songs",
                   G_CALLBACK(on_handle_shuffle_songs), NULL);
  g_signal_connect(shairportSyncRemoteControlSkeleton, "handle-volume-up",
                   G_CALLBACK(on_handle_volume_up), NULL);
  g_signal_connect(shairportSyncRemoteControlSkeleton, "handle-volume-down",
                   G_CALLBACK(on_handle_volume_down), NULL);
  g_signal_connect(shairportSyncRemoteControlSkeleton, "handle-set-airplay-volume",
                   G_CALLBACK(on_handle_set_airplay_volume), NULL);


  g_signal_connect(shairportSyncAdvancedRemoteControlSkeleton, "handle-set-volume",
                   G_CALLBACK(on_handle_set_volume), NULL);

  g_signal_connect(shairportSyncAdvancedRemoteControlSkeleton, "notify::shuffle",
                   G_CALLBACK(notify_shuffle_callback), NULL);

  g_signal_connect(shairportSyncAdvancedRemoteControlSkeleton, "notify::loop-status",
                   G_CALLBACK(notify_loop_status_callback), NULL);

  add_metadata_watcher(dbus_metadata_watcher, NULL);

  shairport_sync_set_loudness_threshold(SHAIRPORT_SYNC(shairportSyncSkeleton),
                                        config.loudness_reference_volume_db);
  shairport_sync_set_drift_tolerance(SHAIRPORT_SYNC(shairportSyncSkeleton), config.tolerance);

#ifdef CONFIG_APPLE_ALAC
  if (config.use_apple_decoder == 0) {
    shairport_sync_set_alacdecoder(SHAIRPORT_SYNC(shairportSyncSkeleton), "hammerton");
    debug(1, ">> ALACDecoder set to \"hammerton\"");
  } else {
    shairport_sync_set_alacdecoder(SHAIRPORT_SYNC(shairportSyncSkeleton), "apple");
    debug(1, ">> ALACDecoder set to \"apple\"");
  }
#else
  shairport_sync_set_alacdecoder(SHAIRPORT_SYNC(shairportSyncSkeleton), "hammerton");
  debug(1, ">> ALACDecoder set to \"hammerton\"");

#endif

  shairport_sync_set_active(SHAIRPORT_SYNC(shairportSyncSkeleton), FALSE);
  debug(1, ">> Active set to \"false\"");

  switch (config.disable_standby_mode) {
  case disable_standby_off:
    shairport_sync_set_disable_standby_mode(SHAIRPORT_SYNC(shairportSyncSkeleton), "off");
    debug(1, ">> disable standby mode set to \"off\"");
    break;
  case disable_standby_always:
    shairport_sync_set_disable_standby_mode(SHAIRPORT_SYNC(shairportSyncSkeleton), "always");
    debug(1, ">> disable standby mode set to \"always\"");
    break;
  case disable_standby_auto:
    shairport_sync_set_disable_standby_mode(SHAIRPORT_SYNC(shairportSyncSkeleton), "auto");
    debug(1, ">> disable standby mode set to \"auto\"");
    break;
  default:
    debug(1, "invalid disable_standby mode!");
    break;
  }

#ifdef CONFIG_SOXR
  if (config.packet_stuffing == ST_basic) {
    shairport_sync_set_interpolation(SHAIRPORT_SYNC(shairportSyncSkeleton), "basic");
    debug(1, ">> interpolation set to \"basic\" (soxr support built in)");
  } else if (config.packet_stuffing == ST_auto) {
    shairport_sync_set_interpolation(SHAIRPORT_SYNC(shairportSyncSkeleton), "auto");
    debug(1, ">> interpolation set to \"auto\" (soxr support built in)");
  } else {
    shairport_sync_set_interpolation(SHAIRPORT_SYNC(shairportSyncSkeleton), "soxr");
    debug(1, ">> interpolation set to \"soxr\"");
  }
#else
  if (config.packet_stuffing == ST_basic) {
    shairport_sync_set_interpolation(SHAIRPORT_SYNC(shairportSyncSkeleton), "basic");
    debug(1, ">> interpolation set to \"basic\" (no soxr support)");
  } else if (config.packet_stuffing == ST_auto) {
    shairport_sync_set_interpolation(SHAIRPORT_SYNC(shairportSyncSkeleton), "auto");
    debug(1, ">> interpolation set to \"auto\" (no soxr support)");
  }
#endif

  if (config.volume_control_profile == VCP_standard)
    shairport_sync_set_volume_control_profile(SHAIRPORT_SYNC(shairportSyncSkeleton), "standard");
  else
    shairport_sync_set_volume_control_profile(SHAIRPORT_SYNC(shairportSyncSkeleton), "flat");

  if (config.keep_dac_busy == 0) {
    shairport_sync_set_disable_standby(SHAIRPORT_SYNC(shairportSyncSkeleton), FALSE);
  } else {
    shairport_sync_set_disable_standby(SHAIRPORT_SYNC(shairportSyncSkeleton), TRUE);
  }

  if (config.loudness == 0) {
    shairport_sync_set_loudness(SHAIRPORT_SYNC(shairportSyncSkeleton), FALSE);
  } else {
    shairport_sync_set_loudness(SHAIRPORT_SYNC(shairportSyncSkeleton), TRUE);
  }

#ifdef CONFIG_CONVOLUTION
  if (config.convolution == 0) {
    shairport_sync_set_convolution(SHAIRPORT_SYNC(shairportSyncSkeleton), FALSE);
  } else {
    shairport_sync_set_convolution(SHAIRPORT_SYNC(shairportSyncSkeleton), TRUE);
  }
  if (config.convolution_ir_file)
    shairport_sync_set_convolution_impulse_response_file(SHAIRPORT_SYNC(shairportSyncSkeleton), config.convolution_ir_file);
//  else
//    shairport_sync_set_convolution_impulse_response_file(SHAIRPORT_SYNC(shairportSyncSkeleton), NULL);
#endif

  shairport_sync_set_version(SHAIRPORT_SYNC(shairportSyncSkeleton), PACKAGE_VERSION);
  char *vs = get_version_string();
  shairport_sync_set_version_string(SHAIRPORT_SYNC(shairportSyncSkeleton), vs);
  if (vs)
    free(vs);

  shairport_sync_diagnostics_set_verbosity(
      SHAIRPORT_SYNC_DIAGNOSTICS(shairportSyncDiagnosticsSkeleton), debuglev);

  // debug(2,">> log verbosity is %d.",debuglev);

  if (config.statistics_requested == 0) {
    shairport_sync_diagnostics_set_statistics(
        SHAIRPORT_SYNC_DIAGNOSTICS(shairportSyncDiagnosticsSkeleton), FALSE);
    // debug(1, ">> statistics logging is off");
  } else {
    shairport_sync_diagnostics_set_statistics(
        SHAIRPORT_SYNC_DIAGNOSTICS(shairportSyncDiagnosticsSkeleton), TRUE);
    // debug(1, ">> statistics logging is on");
  }

  if (config.debugger_show_elapsed_time == 0) {
    shairport_sync_diagnostics_set_elapsed_time(
        SHAIRPORT_SYNC_DIAGNOSTICS(shairportSyncDiagnosticsSkeleton), FALSE);
    // debug(1, ">> elapsed time is included in log entries");
  } else {
    shairport_sync_diagnostics_set_elapsed_time(
        SHAIRPORT_SYNC_DIAGNOSTICS(shairportSyncDiagnosticsSkeleton), TRUE);
    // debug(1, ">> elapsed time is not included in log entries");
  }

  if (config.debugger_show_relative_time == 0) {
    shairport_sync_diagnostics_set_delta_time(
        SHAIRPORT_SYNC_DIAGNOSTICS(shairportSyncDiagnosticsSkeleton), FALSE);
    // debug(1, ">> delta time is included in log entries");
  } else {
    shairport_sync_diagnostics_set_delta_time(
        SHAIRPORT_SYNC_DIAGNOSTICS(shairportSyncDiagnosticsSkeleton), TRUE);
    // debug(1, ">> delta time is not included in log entries");
  }

  if (config.debugger_show_file_and_line == 0) {
    shairport_sync_diagnostics_set_file_and_line(
        SHAIRPORT_SYNC_DIAGNOSTICS(shairportSyncDiagnosticsSkeleton), FALSE);
    // debug(1, ">> file and line is included in log entries");
  } else {
    shairport_sync_diagnostics_set_file_and_line(
        SHAIRPORT_SYNC_DIAGNOSTICS(shairportSyncDiagnosticsSkeleton), TRUE);
    // debug(1, ">> file and line is not included in log entries");
  }

  shairport_sync_remote_control_set_player_state(shairportSyncRemoteControlSkeleton,
                                                 "Not Available");
  shairport_sync_advanced_remote_control_set_playback_status(
      shairportSyncAdvancedRemoteControlSkeleton, "Not Available");

  shairport_sync_advanced_remote_control_set_loop_status(shairportSyncAdvancedRemoteControlSkeleton,
                                                         "Not Available");

  debug(1, "Shairport Sync native D-Bus service started at \"%s\" on the %s bus.", name,
        (config.dbus_service_bus_type == DBT_session) ? "session" : "system");
  service_is_running = 1;
}

static void on_dbus_name_lost_again(__attribute__((unused)) GDBusConnection *connection,
                                    __attribute__((unused)) const gchar *name,
                                    __attribute__((unused)) gpointer user_data) {
  warn("Could not acquire a Shairport Sync native D-Bus interface \"%s\" on the %s bus.", name,
       (config.dbus_service_bus_type == DBT_session) ? "session" : "system");
}

static void on_dbus_name_lost(__attribute__((unused)) GDBusConnection *connection,
                              __attribute__((unused)) const gchar *name,
                              __attribute__((unused)) gpointer user_data) {
  // debug(1, "Could not acquire a Shairport Sync native D-Bus interface \"%s\" on the %s bus --
  // will try adding the process "
  //         "number to the end of it.",
  //      name, (config.dbus_service_bus_type == DBT_session) ? "session" : "system");
  pid_t pid = getpid();
  char interface_name[256] = "";
  snprintf(interface_name, sizeof(interface_name), "org.gnome.ShairportSync.i%d", pid);
  GBusType dbus_bus_type = G_BUS_TYPE_SYSTEM;
  if (config.dbus_service_bus_type == DBT_session)
    dbus_bus_type = G_BUS_TYPE_SESSION;
  // debug(1, "Looking for a Shairport Sync native D-Bus interface \"%s\" on the %s bus.",
  // interface_name,(config.dbus_service_bus_type == DBT_session) ? "session" : "system");
  g_bus_own_name(dbus_bus_type, interface_name, G_BUS_NAME_OWNER_FLAGS_NONE, NULL,
                 on_dbus_name_acquired, on_dbus_name_lost_again, NULL, NULL);
}

int start_dbus_service() {
  //  shairportSyncSkeleton = NULL;
  GBusType dbus_bus_type = G_BUS_TYPE_SYSTEM;
  if (config.dbus_service_bus_type == DBT_session)
    dbus_bus_type = G_BUS_TYPE_SESSION;
  // debug(1, "Looking for a Shairport Sync native D-Bus interface \"org.gnome.ShairportSync\" on
  // the %s bus.",(config.dbus_service_bus_type == DBT_session) ? "session" : "system");
  ownerID = g_bus_own_name(dbus_bus_type, "org.gnome.ShairportSync", G_BUS_NAME_OWNER_FLAGS_NONE,
                           NULL, on_dbus_name_acquired, on_dbus_name_lost, NULL, NULL);
  return 0; // this is just to quieten a compiler warning
}

void stop_dbus_service() {
  debug(2, "stopping dbus service");
  if (ownerID)
    g_bus_unown_name(ownerID);
  else
    debug(1, "Zero OwnerID for \"org.gnome.ShairportSync\".");
  service_is_running = 0;
}

int dbus_service_is_running() { return service_is_running; }
