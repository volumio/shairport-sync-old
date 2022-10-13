/*
 * Metadata hub and access methods.
 * Basically, if you need to store metadata
 * (e.g. for use with the dbus interfaces),
 * then you need a metadata hub,
 * where everything is stored
 * This file is part of Shairport Sync.
 * Copyright (c) Mike Brady 2017--2020
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

#include <stdlib.h>
#include <string.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <inttypes.h>

#include "config.h"

#include "common.h"
#include "dacp.h"
#include "metadata_hub.h"

#ifdef CONFIG_MBEDTLS
#include <mbedtls/md5.h>
#include <mbedtls/version.h>
#endif

#ifdef CONFIG_POLARSSL
#include <polarssl/md5.h>
#endif

#ifdef CONFIG_OPENSSL
#include <openssl/md5.h>
#endif

struct metadata_bundle metadata_store;

int metadata_hub_initialised = 0;

pthread_rwlock_t metadata_hub_re_lock = PTHREAD_RWLOCK_INITIALIZER;

int string_update(char **str, int *flag, char *s) {
  if (s)
    return string_update_with_size(str, flag, s, strlen(s));
  else
    return string_update_with_size(str, flag, NULL, 0);
}

void metadata_hub_init(void) {
  // debug(1, "Metadata bundle initialisation.");
  memset(&metadata_store, 0, sizeof(metadata_store));
  metadata_hub_initialised = 1;
}

void metadata_hub_stop(void) {}

void add_metadata_watcher(metadata_watcher fn, void *userdata) {
  int i;
  for (i = 0; i < number_of_watchers; i++) {
    if (metadata_store.watchers[i] == NULL) {
      metadata_store.watchers[i] = fn;
      metadata_store.watchers_data[i] = userdata;
      // debug(1, "Added a metadata watcher into slot %d", i);
      break;
    }
  }
}

/*
void metadata_hub_unlock_hub_mutex_cleanup(__attribute__((unused)) void *arg) {
  // debug(1, "metadata_hub_unlock_hub_mutex_cleanup called.");
  pthread_rwlock_unlock(&metadata_hub_re_lock);
}
*/

void run_metadata_watchers(void) {
  int i;
  for (i = 0; i < number_of_watchers; i++) {
    if (metadata_store.watchers[i]) {
      metadata_store.watchers[i](&metadata_store, metadata_store.watchers_data[i]);
    }
  }
  // turn off changed flags
  metadata_store.cover_art_pathname_changed = 0;
  metadata_store.client_ip_changed = 0;
  metadata_store.server_ip_changed = 0;
  metadata_store.progress_string_changed = 0;
  metadata_store.item_id_changed = 0;
  metadata_store.item_composite_id_changed = 0;
  metadata_store.artist_name_changed = 0;
  metadata_store.album_artist_name_changed = 0;
  metadata_store.album_name_changed = 0;
  metadata_store.track_name_changed = 0;
  metadata_store.genre_changed = 0;
  metadata_store.comment_changed = 0;
  metadata_store.composer_changed = 0;
  metadata_store.file_kind_changed = 0;
  metadata_store.song_description_changed = 0;
  metadata_store.song_album_artist_changed = 0;
  metadata_store.sort_artist_changed = 0;
  metadata_store.sort_album_changed = 0;
  metadata_store.sort_composer_changed = 0;
  metadata_store.songtime_in_milliseconds_changed = 0;
}

void metadata_hub_modify_prolog(void) {
  // always run this before changing an entry or a sequence of entries in the metadata_hub
  // debug(1, "locking metadata hub for writing");
  if (pthread_rwlock_trywrlock(&metadata_hub_re_lock) != 0) {
    debug(2, "Metadata_hub write lock is already taken -- must wait.");
    pthread_rwlock_wrlock(&metadata_hub_re_lock);
    debug(2, "Okay -- acquired the metadata_hub write lock.");
  } else {
    debug(3, "Metadata_hub write lock acquired.");
  }
}

void metadata_hub_modify_epilog(int modified) {
  metadata_store.dacp_server_has_been_active =
      metadata_store.dacp_server_active; // set the scanner_has_been_active now.
  if (modified) {
    run_metadata_watchers();
  }
  pthread_rwlock_unlock(&metadata_hub_re_lock);
  debug(3, "Metadata_hub write lock unlocked.");
}

void metadata_hub_read_prolog(void) {
  // always run this before reading an entry or a sequence of entries in the metadata_hub
  // debug(1, "locking metadata hub for reading");
  if (pthread_rwlock_tryrdlock(&metadata_hub_re_lock) != 0) {
    debug(2, "Metadata_hub read lock is already taken -- must wait.");
    pthread_rwlock_rdlock(&metadata_hub_re_lock);
    debug(2, "Okay -- acquired the metadata_hub read lock.");
  }
}

void metadata_hub_read_epilog(void) {
  // always run this after reading an entry or a sequence of entries in the metadata_hub
  // debug(1, "unlocking metadata hub for reading");
  pthread_rwlock_unlock(&metadata_hub_re_lock);
}

char *metadata_write_image_file(const char *buf, int len) {

  // warning -- this removes all files from the directory apart from this one, if it exists
  // it will return a path to the image file allocated with malloc.
  // free it if you don't need it.

  char *path = NULL; // this will be what is returned  
  if (strcmp(config.cover_art_cache_dir,"") != 0) { // an empty string means do not write the file

    uint8_t img_md5[16];
    // uint8_t ap_md5[16];

  #ifdef CONFIG_OPENSSL
    MD5_CTX ctx;
    MD5_Init(&ctx);
    MD5_Update(&ctx, buf, len);
    MD5_Final(img_md5, &ctx);
  #endif

  #ifdef CONFIG_MBEDTLS
  #if MBEDTLS_VERSION_MINOR >= 7
    mbedtls_md5_context tctx;
    mbedtls_md5_starts_ret(&tctx);
    mbedtls_md5_update_ret(&tctx, (const unsigned char *)buf, len);
    mbedtls_md5_finish_ret(&tctx, img_md5);
  #else
    mbedtls_md5_context tctx;
    mbedtls_md5_starts(&tctx);
    mbedtls_md5_update(&tctx, (const unsigned char *)buf, len);
    mbedtls_md5_finish(&tctx, img_md5);
  #endif
  #endif

  #ifdef CONFIG_POLARSSL
    md5_context tctx;
    md5_starts(&tctx);
    md5_update(&tctx, (const unsigned char *)buf, len);
    md5_finish(&tctx, img_md5);
  #endif

    char img_md5_str[33];
    memset(img_md5_str, 0, sizeof(img_md5_str));
    char *ext;
    char png[] = "png";
    char jpg[] = "jpg";
    int i;
    for (i = 0; i < 16; i++)
      snprintf(&img_md5_str[i * 2], 3, "%02x", (uint8_t)img_md5[i]);
    // see if the file is a jpeg or a png
    if (strncmp(buf, "\xFF\xD8\xFF", 3) == 0)
      ext = jpg;
    else if (strncmp(buf, "\x89\x50\x4E\x47\x0D\x0A\x1A\x0A", 8) == 0)
      ext = png;
    else {
      debug(1, "Unidentified image type of cover art -- jpg extension used.");
      ext = jpg;
    }
    mode_t oldumask = umask(000);
    int result = mkpath(config.cover_art_cache_dir, 0777);
    umask(oldumask);
    if ((result == 0) || (result == -EEXIST)) {
      // see if the file exists by opening it.
      // if it exists, we're done
      char *prefix = "cover-";

      size_t pl = strlen(config.cover_art_cache_dir) + 1 + strlen(prefix) + strlen(img_md5_str) + 1 +
                  strlen(ext);

      path = malloc(pl + 1);
      snprintf(path, pl + 1, "%s/%s%s.%s", config.cover_art_cache_dir, prefix, img_md5_str, ext);
      int cover_fd = open(path, O_WRONLY | O_CREAT | O_EXCL, S_IRWXU | S_IRGRP | S_IROTH);
      if (cover_fd > 0) {
        // write the contents
        if (write(cover_fd, buf, len) < len) {
          warn("Writing cover art file \"%s\" failed!", path);
          free(path);
          path = NULL;
        }
        close(cover_fd);

        // now delete all other files, if requested
        if (config.retain_coverart == 0) {
          DIR *d;
          struct dirent *dir;
          d = opendir(config.cover_art_cache_dir);
          if (d) {
            int fnl = strlen(prefix) + strlen(img_md5_str) + 1 + strlen(ext) + 1;

            char *full_filename = malloc(fnl);
            if (full_filename == NULL)
              die("Can't allocate memory at metadata_write_image_file.");
            memset(full_filename, 0, fnl);
            snprintf(full_filename, fnl, "%s%s.%s", prefix, img_md5_str, ext);
            int dir_fd = open(config.cover_art_cache_dir, O_DIRECTORY);
            if (dir_fd > 0) {
              while ((dir = readdir(d)) != NULL) {
                if (dir->d_type == DT_REG) {
                  if (strcmp(full_filename, dir->d_name) != 0) {
                    if (unlinkat(dir_fd, dir->d_name, 0) != 0) {
                      debug(1, "Error %d deleting cover art file \"%s\".", errno, dir->d_name);
                    }
                  }
                }
              }
            } else {
              debug(1, "Can't open the directory for deletion.");
            }
            free(full_filename);
            closedir(d);
          }
        }
      } else {
        //      if (errno == EEXIST)
        //        debug(1, "Cover art file \"%s\" already exists!", path);
        //      else {
        if (errno != EEXIST) {
          warn("Could not open file \"%s\" for writing cover art", path);
          free(path);
          path = NULL;
        }
      }
    } else {
      debug(1, "Couldn't access or create the cover art cache directory \"%s\".",
            config.cover_art_cache_dir);
    }
  }
  return path;
}

void metadata_hub_process_metadata(uint32_t type, uint32_t code, char *data, uint32_t length) {
  // metadata coming in from the audio source or from Shairport Sync itself passes through here
  // this has more information about tags, which might be relevant:
  // https://code.google.com/p/ytrack/wiki/DMAP

  // all the following items of metadata are contained in one metadata packet
  // they are preceded by an 'ssnc' 'mdst' item and followed by an 'ssnc 'mden' item.

  char *cs;
  int changed = 0;
  if (type == 'core') {
    switch (code) {
    case 'mper': {
        // get the 64-bit number as a uint64_t by reading two uint32_t s and combining them
        uint64_t vl = ntohl(*(uint32_t*)data); // get the high order 32 bits
        vl = vl << 32; // shift them into the correct location
        uint64_t ul = ntohl(*(uint32_t*)(data+sizeof(uint32_t))); // and the low order 32 bits
        vl = vl + ul;
        debug(2, "MH Item ID seen: \"%" PRIx64 "\" of length %u.", vl, length);
        if (vl != metadata_store.item_id) {
          metadata_store.item_id = vl;
          metadata_store.item_id_changed = 1;
          metadata_store.item_id_received = 1;
          debug(2, "MH Item ID set to: \"%" PRIx64 "\"", metadata_store.item_id);
        }
      }      
      break;
    case 'astm': {
        uint32_t ui = ntohl(*(uint32_t *)data);
        debug(2, "MH Song Time seen: \"%u\" of length %u.", ui, length);
        if (ui != metadata_store.songtime_in_milliseconds) {
          metadata_store.songtime_in_milliseconds = ui;
          metadata_store.songtime_in_milliseconds_changed = 1;
          debug(2, "MH Song Time set to: \"%u\"", metadata_store.songtime_in_milliseconds);
        }
      }
      break;
    case 'asal':
      cs = strndup(data, length);
      if (string_update(&metadata_store.album_name, &metadata_store.album_name_changed, cs)) {
        debug(2, "MH Album name set to: \"%s\"", metadata_store.album_name);
      }
      free(cs);
      break;
    case 'asar':
      cs = strndup(data, length);
      if (string_update(&metadata_store.artist_name, &metadata_store.artist_name_changed, cs)) {
        debug(2, "MH Artist name set to: \"%s\"", metadata_store.artist_name);
      }
      free(cs);
      break;
    case 'assl':
      cs = strndup(data, length);
      if (string_update(&metadata_store.album_artist_name,
                        &metadata_store.album_artist_name_changed, cs)) {
        debug(2, "MH Album Artist name set to: \"%s\"", metadata_store.album_artist_name);
      }
      free(cs);
      break;
    case 'ascm':
      cs = strndup(data, length);
      if (string_update(&metadata_store.comment, &metadata_store.comment_changed, cs)) {
        debug(2, "MH Comment set to: \"%s\"", metadata_store.comment);
      }
      free(cs);
      break;
    case 'asgn':
      cs = strndup(data, length);
      if (string_update(&metadata_store.genre, &metadata_store.genre_changed, cs)) {
        debug(2, "MH Genre set to: \"%s\"", metadata_store.genre);
      }
      free(cs);
      break;
    case 'minm':
      cs = strndup(data, length);
      if (string_update(&metadata_store.track_name, &metadata_store.track_name_changed, cs)) {
        debug(2, "MH Track Name set to: \"%s\"", metadata_store.track_name);
      }
      free(cs);
      break;
    case 'ascp':
      cs = strndup(data, length);
      if (string_update(&metadata_store.composer, &metadata_store.composer_changed, cs)) {
        debug(2, "MH Composer set to: \"%s\"", metadata_store.composer);
      }
      free(cs);
      break;
    case 'asdt':
      cs = strndup(data, length);
      if (string_update(&metadata_store.song_description, &metadata_store.song_description_changed,
                        cs)) {
        debug(2, "MH Song Description set to: \"%s\"", metadata_store.song_description);
      }
      free(cs);
      break;
    case 'asaa':
      cs = strndup(data, length);
      if (string_update(&metadata_store.song_album_artist,
                        &metadata_store.song_album_artist_changed, cs)) {
        debug(2, "MH Song Album Artist set to: \"%s\"", metadata_store.song_album_artist);
      }
      free(cs);
      break;
    case 'assn':
      cs = strndup(data, length);
      if (string_update(&metadata_store.sort_name, &metadata_store.sort_name_changed, cs)) {
        debug(2, "MH Sort Name set to: \"%s\"", metadata_store.sort_name);
      }
      free(cs);
      break;
    case 'assa':
      cs = strndup(data, length);
      if (string_update(&metadata_store.sort_artist, &metadata_store.sort_artist_changed, cs)) {
        debug(2, "MH Sort Artist set to: \"%s\"", metadata_store.sort_artist);
      }
      free(cs);
      break;
    case 'assu':
      cs = strndup(data, length);
      if (string_update(&metadata_store.sort_album, &metadata_store.sort_album_changed, cs)) {
        debug(2, "MH Sort Album set to: \"%s\"", metadata_store.sort_album);
      }
      free(cs);
      break;
    case 'assc':
      cs = strndup(data, length);
      if (string_update(&metadata_store.sort_composer, &metadata_store.sort_composer_changed, cs)) {
        debug(2, "MH Sort Composer set to: \"%s\"", metadata_store.sort_composer);
      }
      free(cs);
    default:
      /*
          {
            char typestring[5];
            *(uint32_t *)typestring = htonl(type);
            typestring[4] = 0;
            char codestring[5];
            *(uint32_t *)codestring = htonl(code);
            codestring[4] = 0;
            char *payload;
            if (length < 2048)
              payload = strndup(data, length);
            else
              payload = NULL;
            debug(1, "MH \"%s\" \"%s\" (%d bytes): \"%s\".", typestring, codestring, length,
         payload);
            if (payload)
              free(payload);
          }
      */
      break;
    }
  } else if (type == 'ssnc') {
    switch (code) {
    // ignore the following
    case 'pcst':
    case 'pcen':
      break;
    case 'mdst':
      debug(2, "MH Metadata stream processing start.");
      metadata_hub_modify_prolog();
      break;
    case 'mden':
      debug(2, "MH Metadata stream processing end.");
      metadata_hub_modify_epilog(1);
      debug(2, "MH Metadata stream processing epilog complete.");
      break;
    case 'PICT':
      metadata_hub_modify_prolog();
      debug(2, "MH Picture received, length %u bytes.", length);
      char uri[2048];
      if ((length > 16) && (strcmp(config.cover_art_cache_dir,"")!=0)) { // if it's okay to write the file
        char *pathname = metadata_write_image_file(data, length);
        snprintf(uri, sizeof(uri), "file://%s", pathname);
        free(pathname);
      } else {
        uri[0] = '\0';
      }
      if (string_update(&metadata_store.cover_art_pathname,
                        &metadata_store.cover_art_pathname_changed,
                        uri)) // if the picture's file path is different from the stored one...
        metadata_hub_modify_epilog(1);
      else
        metadata_hub_modify_epilog(0);
      break;
    case 'clip':
      metadata_hub_modify_prolog();
      cs = strndup(data, length);
      if (string_update(&metadata_store.client_ip, &metadata_store.client_ip_changed, cs)) {
        changed = 1;
        debug(2, "MH Client IP set to: \"%s\"", metadata_store.client_ip);
      }
      free(cs);
      metadata_hub_modify_epilog(changed);
      break;
    case 'prgr':
      metadata_hub_modify_prolog();
      cs = strndup(data, length);
      if (string_update(&metadata_store.progress_string, &metadata_store.progress_string_changed,
                        cs)) {
        changed = 1;
        debug(2, "MH Progress String set to: \"%s\"", metadata_store.progress_string);
      }
      free(cs);
      metadata_hub_modify_epilog(changed);
      break;
    case 'svip':
      metadata_hub_modify_prolog();
      cs = strndup(data, length);
      if (string_update(&metadata_store.server_ip, &metadata_store.server_ip_changed, cs)) {
        changed = 1;
        debug(2, "MH Server IP set to: \"%s\"", metadata_store.server_ip);
      }
      free(cs);
      metadata_hub_modify_epilog(changed);
      break;
    case 'abeg':
      metadata_hub_modify_prolog();
      changed = (metadata_store.active_state != AM_ACTIVE);
      metadata_store.active_state = AM_ACTIVE;
      metadata_hub_modify_epilog(changed);
      break;
    case 'aend':
      metadata_hub_modify_prolog();
      changed = (metadata_store.active_state != AM_INACTIVE);
      metadata_store.active_state = AM_INACTIVE;
      metadata_hub_modify_epilog(changed);
      break;
    case 'pbeg':
      metadata_hub_modify_prolog();
      changed = ((metadata_store.player_state != PS_PLAYING) || (metadata_store.player_thread_active == 0));
      metadata_store.player_state = PS_PLAYING;
      metadata_store.player_thread_active = 1;
      metadata_hub_modify_epilog(changed);
      break;
    case 'pend':
      metadata_hub_modify_prolog();
      changed = ((metadata_store.player_state != PS_STOPPED) || (metadata_store.player_thread_active == 1));
      metadata_store.player_state = PS_STOPPED;
      metadata_store.player_thread_active = 0;
      metadata_hub_modify_epilog(changed);
      break;
    case 'pfls':
      metadata_hub_modify_prolog();
      changed = (metadata_store.player_state != PS_PAUSED);
      metadata_store.player_state = PS_PAUSED;
      metadata_hub_modify_epilog(changed);
      break;
    case 'pffr': // this is sent when the first frame has been received
    case 'prsm':
      metadata_hub_modify_prolog();
      changed = (metadata_store.player_state != PS_PLAYING);
      metadata_store.player_state = PS_PLAYING;
      metadata_hub_modify_epilog(changed);
      break;
    case 'pvol': {
      metadata_hub_modify_prolog();
      // Note: it's assumed that the config.airplay volume has already been correctly set.
      //int32_t actual_volume;
      //int gv = dacp_get_volume(&actual_volume);
      //metadata_hub_modify_prolog();
      //if ((gv == 200) && (metadata_store.speaker_volume != actual_volume)) {
      //  metadata_store.speaker_volume = actual_volume;
      //  changed = 1;
      //}
      if (metadata_store.airplay_volume != config.airplay_volume) {
        metadata_store.airplay_volume = config.airplay_volume;
        changed = 1;
      }
      metadata_hub_modify_epilog(changed); // change
    } break;

    default: {
      char typestring[5];
      uint32_t tm = htonl(type);
      memcpy(typestring, &tm, sizeof(uint32_t));
      typestring[4] = 0;
      char codestring[5];
      uint32_t cm = htonl(code);
      memcpy(codestring, &cm, sizeof(uint32_t));
      codestring[4] = 0;
      char *payload;
      if (length < 2048)
        payload = strndup(data, length);
      else
        payload = NULL;
      // debug(1, "MH \"%s\" \"%s\" (%d bytes): \"%s\".", typestring, codestring, length, payload);
      if (payload)
        free(payload);
    }
    }
  }
}
