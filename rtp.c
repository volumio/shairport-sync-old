/*
 * Apple RTP protocol handler. This file is part of Shairport.
 * Copyright (c) James Laird 2013
 * Copyright (c) Mike Brady 2014 -- 2019
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

#include "rtp.h"
#include "common.h"
#include "player.h"
#include "rtsp.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <memory.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

uint64_t local_to_remote_time_jitter;
uint64_t local_to_remote_time_jitter_count;

void rtp_initialise(rtsp_conn_info *conn) {
  conn->rtp_time_of_last_resend_request_error_fp = 0;
  conn->rtp_running = 0;
  // initialise the timer mutex
  int rc = pthread_mutex_init(&conn->reference_time_mutex, NULL);
  if (rc)
    debug(1, "Error initialising reference_time_mutex.");
}

void rtp_terminate(rtsp_conn_info *conn) {
  conn->reference_timestamp = 0;
  // destroy the timer mutex
  int rc = pthread_mutex_destroy(&conn->reference_time_mutex);
  if (rc)
    debug(1, "Error destroying reference_time_mutex variable.");
}

uint64_t local_to_remote_time_difference_now(rtsp_conn_info *conn) {
  // this is an attempt to compensate for clock drift since the last time ping that was used
  // so, if we have a non-zero clock drift, we will calculate the drift there would
  // be from the time of the last time ping
  uint64_t local_time_now_fp = get_absolute_time_in_fp();
  uint64_t time_since_last_local_to_remote_time_difference_measurement =
      local_time_now_fp - conn->local_to_remote_time_difference_measurement_time;

  uint64_t remote_time_since_last_local_to_remote_time_difference_measurement =
      (uint64_t)(conn->local_to_remote_time_gradient *
                 time_since_last_local_to_remote_time_difference_measurement);

  double drift;
  if (remote_time_since_last_local_to_remote_time_difference_measurement >=
      time_since_last_local_to_remote_time_difference_measurement)
    drift = (1.0 * (remote_time_since_last_local_to_remote_time_difference_measurement -
                    time_since_last_local_to_remote_time_difference_measurement)) /
            (uint64_t)0x100000000;
  else
    drift = -((1.0 * (time_since_last_local_to_remote_time_difference_measurement -
                      remote_time_since_last_local_to_remote_time_difference_measurement)) /
              (uint64_t)0x100000000);

  //  double interval_ms =
  //  1.0*(((time_since_last_local_to_remote_time_difference_measurement)*1000)>>32);
  //  debug(1,"Measurement drift is %.2f microseconds (0x%" PRIx64 " in 64-bit fp) over %.2f
  //  milliseconds with drift of %.2f
  //  ppm.",drift*1000000,(uint64_t)(drift*(uint64_t)0x100000000),interval_ms,(1.0-conn->local_to_remote_time_gradient)*1000000);
  //  return conn->local_to_remote_time_difference + (uint64_t)(drift*(uint64_t 0x100000000));
  return conn->local_to_remote_time_difference + (uint64_t)(drift * (uint64_t)0x100000000);
}

void rtp_audio_receiver_cleanup_handler(__attribute__((unused)) void *arg) {
  debug(3, "Audio Receiver Cleanup Done.");
}

void *rtp_audio_receiver(void *arg) {
  pthread_cleanup_push(rtp_audio_receiver_cleanup_handler, arg);
  rtsp_conn_info *conn = (rtsp_conn_info *)arg;

  int32_t last_seqno = -1;
  uint8_t packet[2048], *pktp;

  uint64_t time_of_previous_packet_fp = 0;
  float longest_packet_time_interval_us = 0.0;

  // mean and variance calculations from "online_variance" algorithm at
  // https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Online_algorithm

  int32_t stat_n = 0;
  float stat_mean = 0.0;
  float stat_M2 = 0.0;

  int frame_count = 0;
  ssize_t nread;
  while (1) {
    nread = recv(conn->audio_socket, packet, sizeof(packet), 0);

    frame_count++;

    uint64_t local_time_now_fp = get_absolute_time_in_fp();
    if (time_of_previous_packet_fp) {
      float time_interval_us =
          (((local_time_now_fp - time_of_previous_packet_fp) * 1000000) >> 32) * 1.0;
      time_of_previous_packet_fp = local_time_now_fp;
      if (time_interval_us > longest_packet_time_interval_us)
        longest_packet_time_interval_us = time_interval_us;
      stat_n += 1;
      float stat_delta = time_interval_us - stat_mean;
      stat_mean += stat_delta / stat_n;
      stat_M2 += stat_delta * (time_interval_us - stat_mean);
      if (stat_n % 2500 == 0) {
        debug(2, "Packet reception interval stats: mean, standard deviation and max for the last "
                 "2,500 packets in microseconds: %10.1f, %10.1f, %10.1f.",
              stat_mean, sqrtf(stat_M2 / (stat_n - 1)), longest_packet_time_interval_us);
        stat_n = 0;
        stat_mean = 0.0;
        stat_M2 = 0.0;
        time_of_previous_packet_fp = 0;
        longest_packet_time_interval_us = 0.0;
      }
    } else {
      time_of_previous_packet_fp = local_time_now_fp;
    }

    if (nread >= 0) {
      ssize_t plen = nread;
      uint8_t type = packet[1] & ~0x80;
      if (type == 0x60 || type == 0x56) { // audio data / resend
        pktp = packet;
        if (type == 0x56) {
          pktp += 4;
          plen -= 4;
        }
        seq_t seqno = ntohs(*(uint16_t *)(pktp + 2));
        // increment last_seqno and see if it's the same as the incoming seqno

        if (type == 0x60) { // regular audio data

          /*
          char obf[4096];
          char *obfp = obf;
          int obfc;
          for (obfc=0;obfc<plen;obfc++) {
            snprintf(obfp, 3, "%02X", pktp[obfc]);
            obfp+=2;
          };
          *obfp=0;
          debug(1,"Audio Packet Received: \"%s\"",obf);
          */

          if (last_seqno == -1)
            last_seqno = seqno;
          else {
            last_seqno = (last_seqno + 1) & 0xffff;
            // if (seqno != last_seqno)
            //  debug(3, "RTP: Packets out of sequence: expected: %d, got %d.", last_seqno, seqno);
            last_seqno = seqno; // reset warning...
          }
        } else {
          debug(3, "Audio Receiver -- Retransmitted Audio Data Packet %u received.", seqno);
        }

        uint32_t actual_timestamp = ntohl(*(uint32_t *)(pktp + 4));

        // uint32_t ssid = ntohl(*(uint32_t *)(pktp + 8));
        // debug(1, "Audio packet SSID: %08X,%u", ssid,ssid);

        // if (packet[1]&0x10)
        //	debug(1,"Audio packet Extension bit set.");

        pktp += 12;
        plen -= 12;

        // check if packet contains enough content to be reasonable
        if (plen >= 16) {
          if ((config.diagnostic_drop_packet_fraction == 0.0) ||
              (drand48() > config.diagnostic_drop_packet_fraction))
            player_put_packet(seqno, actual_timestamp, pktp, plen, conn);
          else
            debug(3, "Dropping audio packet %u to simulate a bad connection.", seqno);
          continue;
        }
        if (type == 0x56 && seqno == 0) {
          debug(2, "resend-related request packet received, ignoring.");
          continue;
        }
        debug(1, "Audio receiver -- Unknown RTP packet of type 0x%02X length %d seqno %d", type,
              nread, seqno);
      }
      warn("Audio receiver -- Unknown RTP packet of type 0x%02X length %d.", type, nread);
    } else {
      debug(1, "Error receiving an audio packet.");
    }
  }

  /*
  debug(3, "Audio receiver -- Server RTP thread interrupted. terminating.");
  close(conn->audio_socket);
  */

  debug(1, "Audio receiver thread \"normal\" exit -- this can't happen. Hah!");
  pthread_cleanup_pop(0); // don't execute anything here.
  debug(2, "Audio receiver thread exit.");
  pthread_exit(NULL);
}

void rtp_control_handler_cleanup_handler(__attribute__((unused)) void *arg) {
  debug(3, "Control Receiver Cleanup Done.");
}

void *rtp_control_receiver(void *arg) {
  pthread_cleanup_push(rtp_control_handler_cleanup_handler, arg);
  rtsp_conn_info *conn = (rtsp_conn_info *)arg;

  conn->reference_timestamp = 0; // nothing valid received yet
  uint8_t packet[2048], *pktp;
  // struct timespec tn;
  uint64_t remote_time_of_sync;
  uint32_t sync_rtp_timestamp;
  ssize_t nread;
  while (1) {
    nread = recv(conn->control_socket, packet, sizeof(packet), 0);
    // local_time_now = get_absolute_time_in_fp();
    //        clock_gettime(CLOCK_MONOTONIC,&tn);
    //        local_time_now=((uint64_t)tn.tv_sec<<32)+((uint64_t)tn.tv_nsec<<32)/1000000000;

    if (nread >= 0) {

      if ((config.diagnostic_drop_packet_fraction == 0.0) ||
          (drand48() > config.diagnostic_drop_packet_fraction)) {

        ssize_t plen = nread;
        if (packet[1] == 0xd4) {                       // sync data
                                                       /*
                                                            // the following stanza is for debugging only -- normally commented out.
                                                            {
                                                              char obf[4096];
                                                              char *obfp = obf;
                                                              int obfc;
                                                              for (obfc = 0; obfc < plen; obfc++) {
                                                                snprintf(obfp, 3, "%02X", packet[obfc]);
                                                                obfp += 2;
                                                              };
                                                              *obfp = 0;
                                             
                                             
                                                              // get raw timestamp information
                                                              // I think that a good way to understand these timestamps is that
                                                              // (1) the rtlt below is the timestamp of the frame that should be playing at the
                                                              // client-time specified in the packet if there was no delay
                                                              // and (2) that the rt below is the timestamp of the frame that should be playing
                                                              // at the client-time specified in the packet on this device taking account of
                                                              // the delay
                                                              // Thus, (3) the latency can be calculated by subtracting the second from the
                                                              // first.
                                                              // There must be more to it -- there something missing.
                                             
                                                              // In addition, it seems that if the value of the short represented by the second
                                                              // pair of bytes in the packet is 7
                                                              // then an extra time lag is expected to be added, presumably by
                                                              // the AirPort Express.
                                             
                                                              // Best guess is that this delay is 11,025 frames.
                                             
                                                              uint32_t rtlt = nctohl(&packet[4]); // raw timestamp less latency
                                                              uint32_t rt = nctohl(&packet[16]);  // raw timestamp
                                             
                                                              uint32_t fl = nctohs(&packet[2]); //
                                             
                                                              debug(1,"Sync Packet of %d bytes received: \"%s\", flags: %d, timestamps %u and %u,
                                                          giving a latency of %d frames.",plen,obf,fl,rt,rtlt,rt-rtlt);
                                                              //debug(1,"Monotonic timestamps are: %" PRId64 " and %" PRId64 "
                                                          respectively.",monotonic_timestamp(rt, conn),monotonic_timestamp(rtlt, conn));
                                                            }
                                                       */
          if (conn->local_to_remote_time_difference) { // need a time packet to be interchanged
                                                       // first...

            remote_time_of_sync = (uint64_t)nctohl(&packet[8]) << 32;
            remote_time_of_sync += nctohl(&packet[12]);

            // debug(1,"Remote Sync Time: %0llx.",remote_time_of_sync);

            sync_rtp_timestamp = nctohl(&packet[16]);
            uint32_t rtp_timestamp_less_latency = nctohl(&packet[4]);

            // debug(1,"Sync timestamp is %u.",ntohl(*((uint32_t *)&packet[16])));

            if (config.userSuppliedLatency) {
              if (config.userSuppliedLatency != conn->latency) {
                debug(1, "Using the user-supplied latency: %" PRIu32 ".",
                      config.userSuppliedLatency);
              }
              conn->latency = config.userSuppliedLatency;
            } else {

              // It seems that the second pair of bytes in the packet indicate whether a fixed
              // delay of 11,025 frames should be added -- iTunes set this field to 7 and
              // AirPlay sets it to 4.

              // However, on older versions of AirPlay, the 11,025 frames seem to be necessary too

              // The value of 11,025 (0.25 seconds) is a guess based on the "Audio-Latency"
              // parameter
              // returned by an AE.

              // Sigh, it would be nice to have a published protocol...

              uint16_t flags = nctohs(&packet[2]);
              uint32_t la = sync_rtp_timestamp - rtp_timestamp_less_latency; // note, this might
                                                                             // loop around in
                                                                             // modulo. Not sure if
                                                                             // you'll get an error!
              // debug(3, "Latency derived just from the sync packet is %" PRIu32 " frames.", la);

              if ((flags == 7) || ((conn->AirPlayVersion > 0) && (conn->AirPlayVersion <= 353)) ||
                  ((conn->AirPlayVersion > 0) && (conn->AirPlayVersion >= 371))) {
                la += config.fixedLatencyOffset;
                // debug(3, "A fixed latency offset of %d frames has been added, giving a latency of
                // "
                //         "%" PRId64
                //         " frames with flags: %d and AirPlay version %d (triggers if 353 or
                //         less).",
                //      config.fixedLatencyOffset, la, flags, conn->AirPlayVersion);
              }
              if ((conn->maximum_latency) && (conn->maximum_latency < la))
                la = conn->maximum_latency;
              if ((conn->minimum_latency) && (conn->minimum_latency > la))
                la = conn->minimum_latency;

              const uint32_t max_frames = ((3 * BUFFER_FRAMES * 352) / 4) - 11025;

              if (la > max_frames) {
                warn("An out-of-range latency request of %" PRIu32
                     " frames was ignored. Must be %" PRIu32
                     " frames or less (44,100 frames per second). "
                     "Latency remains at %" PRIu32 " frames.",
                     la, max_frames, conn->latency);
              } else {

                if (la != conn->latency) {
                  conn->latency = la;
                  debug(3, "New latency detected: %" PRIu32 ", sync latency: %" PRIu32
                           ", minimum latency: %" PRIu32 ", maximum "
                           "latency: %" PRIu32 ", fixed offset: %" PRIu32 ".",
                        la, sync_rtp_timestamp - rtp_timestamp_less_latency, conn->minimum_latency,
                        conn->maximum_latency, config.fixedLatencyOffset);
                }
              }
            }

            debug_mutex_lock(&conn->reference_time_mutex, 1000, 0);

            if (conn->initial_reference_time == 0) {
              if (conn->packet_count_since_flush > 0) {
                conn->initial_reference_time = remote_time_of_sync;
                conn->initial_reference_timestamp = sync_rtp_timestamp;
              }
            } else {
              uint64_t remote_frame_time_interval =
                  conn->remote_reference_timestamp_time -
                  conn->initial_reference_time; // here, this should never be zero
              if (remote_frame_time_interval) {
                conn->remote_frame_rate =
                    (1.0 * (conn->reference_timestamp - conn->initial_reference_timestamp)) /
                    remote_frame_time_interval; // an IEEE double calculation with a 32-bit
                                                // numerator and 64-bit denominator
                                                // integers
                conn->remote_frame_rate =
                    conn->remote_frame_rate * (uint64_t)0x100000000; // this should just change the
                // [binary] exponent in the IEEE
                // FP representation; the
                // mantissa should be unaffected.
              } else {
                conn->remote_frame_rate = 0.0; // use as a flag.
              }
            }

            // this is for debugging
            uint64_t old_remote_reference_time = conn->remote_reference_timestamp_time;
            uint32_t old_reference_timestamp = conn->reference_timestamp;
            // int64_t old_latency_delayed_timestamp = conn->latency_delayed_timestamp;

            conn->remote_reference_timestamp_time = remote_time_of_sync;
            // conn->reference_timestamp_time =
            //    remote_time_of_sync - local_to_remote_time_difference_now(conn);
            conn->reference_timestamp = sync_rtp_timestamp;
            conn->latency_delayed_timestamp = rtp_timestamp_less_latency;
            debug_mutex_unlock(&conn->reference_time_mutex, 0);

            conn->reference_to_previous_time_difference =
                remote_time_of_sync - old_remote_reference_time;
            if (old_reference_timestamp == 0)
              conn->reference_to_previous_frame_difference = 0;
            else
              conn->reference_to_previous_frame_difference =
                  sync_rtp_timestamp - old_reference_timestamp;

            // int64_t delayed_frame_difference = rtp_timestamp_less_latency -
            // old_latency_delayed_timestamp;

            /*
            if (old_remote_reference_time)
              debug(1,"Time difference: %" PRIu64 " reference and delayed frame differences: %"
            PRId64 " and %" PRId64 ", giving rates _at source!!_ of %f and %f respectively.",
                (conn->reference_to_previous_time_difference*1000000)>>32,conn->reference_to_previous_frame_difference,delayed_frame_difference,
                  (1.0*(conn->reference_to_previous_frame_difference*10000000))/((conn->reference_to_previous_time_difference*10000000)>>32),(1.0*(delayed_frame_difference*10000000))/((conn->reference_to_previous_time_difference*10000000)>>32));
            else
              debug(1,"First sync received");
            */

            // debug(1,"New Reference timestamp and timestamp time...");
            // get estimated remote time now
            // remote_time_now = local_time_now + local_to_remote_time_difference;

            // debug(1,"Sync Time is %lld us late (remote
            // times).",((remote_time_now-remote_time_of_sync)*1000000)>>32);
            // debug(1,"Sync Time is %lld us late (local
            // times).",((local_time_now-reference_timestamp_time)*1000000)>>32);
          } else {
            debug(2, "Sync packet received before we got a timing packet back.");
          }
        } else if (packet[1] == 0xd6) { // resent audio data in the control path -- whaale only?
          pktp = packet + 4;
          plen -= 4;
          seq_t seqno = ntohs(*(uint16_t *)(pktp + 2));
          debug(3, "Control Receiver -- Retransmitted Audio Data Packet %u received.", seqno);

          uint32_t actual_timestamp = ntohl(*(uint32_t *)(pktp + 4));

          pktp += 12;
          plen -= 12;

          // check if packet contains enough content to be reasonable
          if (plen >= 16) {
            player_put_packet(seqno, actual_timestamp, pktp, plen, conn);
            continue;
          } else {
            debug(3, "Too-short retransmitted audio packet received in control port, ignored.");
          }
        } else
          debug(1, "Control Receiver -- Unknown RTP packet of type 0x%02X length %d, ignored.",
                packet[1], nread);
      } else {
        debug(3, "Control Receiver -- dropping a packet to simulate a bad network.");
      }
    } else {
      debug(1, "Control Receiver -- error receiving a packet.");
    }
  }
  debug(1, "Control RTP thread \"normal\" exit -- this can't happen. Hah!");
  pthread_cleanup_pop(0); // don't execute anything here.
  debug(2, "Control RTP thread exit.");
  pthread_exit(NULL);
}

void rtp_timing_sender_cleanup_handler(void *arg) {
  rtsp_conn_info *conn = (rtsp_conn_info *)arg;
  debug(3, "Connection %d: Timing Sender Cleanup.", conn->connection_number);
}

void *rtp_timing_sender(void *arg) {
  pthread_cleanup_push(rtp_timing_sender_cleanup_handler, arg);
  rtsp_conn_info *conn = (rtsp_conn_info *)arg;
  struct timing_request {
    char leader;
    char type;
    uint16_t seqno;
    uint32_t filler;
    uint64_t origin, receive, transmit;
  };

  uint64_t request_number = 0;

  struct timing_request req; // *not* a standard RTCP NACK

  req.leader = 0x80;
  req.type = 0xd2; // Timing request
  req.filler = 0;
  req.seqno = htons(7);

  conn->time_ping_count = 0;
  while (1) {
    // debug(1,"Send a timing request");

    if (!conn->rtp_running)
      debug(1, "rtp_timing_sender called without active stream in RTSP conversation thread %d!",
            conn->connection_number);

    // debug(1, "Requesting ntp timestamp exchange.");

    req.filler = 0;
    req.origin = req.receive = req.transmit = 0;

    //    clock_gettime(CLOCK_MONOTONIC,&dtt);
    conn->departure_time = get_absolute_time_in_fp();
    socklen_t msgsize = sizeof(struct sockaddr_in);
#ifdef AF_INET6
    if (conn->rtp_client_timing_socket.SAFAMILY == AF_INET6) {
      msgsize = sizeof(struct sockaddr_in6);
    }
#endif
    if ((config.diagnostic_drop_packet_fraction == 0.0) ||
        (drand48() > config.diagnostic_drop_packet_fraction)) {
      if (sendto(conn->timing_socket, &req, sizeof(req), 0,
                 (struct sockaddr *)&conn->rtp_client_timing_socket, msgsize) == -1) {
        char em[1024];
        strerror_r(errno, em, sizeof(em));
        debug(1, "Error %d using send-to to the timing socket: \"%s\".", errno, em);
      }
    } else {
      debug(3, "Timing Sender Thread -- dropping outgoing packet to simulate bad network.");
    }

    request_number++;

    if (request_number <= 4)
      usleep(500000); // these are thread cancellation points
    else
      usleep(3000000);
  }
  debug(3, "rtp_timing_sender thread interrupted. This should never happen.");
  pthread_cleanup_pop(0); // don't execute anything here.
  pthread_exit(NULL);
}

void rtp_timing_receiver_cleanup_handler(void *arg) {
  debug(3, "Timing Receiver Cleanup.");
  rtsp_conn_info *conn = (rtsp_conn_info *)arg;
  debug(3, "Cancel Timing Requester.");
  pthread_cancel(conn->timer_requester);
  int oldState;
  pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldState);
  debug(3, "Join Timing Requester.");
  pthread_join(conn->timer_requester, NULL);
  debug(3, "Timing Receiver Cleanup Successful.");
  pthread_setcancelstate(oldState, NULL);
}

void *rtp_timing_receiver(void *arg) {
  pthread_cleanup_push(rtp_timing_receiver_cleanup_handler, arg);
  rtsp_conn_info *conn = (rtsp_conn_info *)arg;

  uint8_t packet[2048];
  ssize_t nread;
  pthread_create(&conn->timer_requester, NULL, &rtp_timing_sender, arg);
  //    struct timespec att;
  uint64_t distant_receive_time, distant_transmit_time, arrival_time, return_time;
  local_to_remote_time_jitter = 0;
  local_to_remote_time_jitter_count = 0;
  // uint64_t first_remote_time = 0;
  // uint64_t first_local_time = 0;

  uint64_t first_local_to_remote_time_difference = 0;
  // uint64_t first_local_to_remote_time_difference_time;
  // uint64_t l2rtd = 0;
  int sequence_number = 0;

  // for getting mean and sd of return times
  int32_t stat_n = 0;
  double stat_mean = 0.0;
  double stat_M2 = 0.0;

  while (1) {
    nread = recv(conn->timing_socket, packet, sizeof(packet), 0);

    if (nread >= 0) {

      if ((config.diagnostic_drop_packet_fraction == 0.0) ||
          (drand48() > config.diagnostic_drop_packet_fraction)) {
        arrival_time = get_absolute_time_in_fp();

        // ssize_t plen = nread;
        // debug(1,"Packet Received on Timing Port.");
        if (packet[1] == 0xd3) { // timing reply
          /*
          char obf[4096];
          char *obfp = obf;
          int obfc;
          for (obfc=0;obfc<plen;obfc++) {
            snprintf(obfp, 3, "%02X", packet[obfc]);
            obfp+=2;
          };
          *obfp=0;
          debug(1,"Timing Packet Received: \"%s\"",obf);
          */

          // arrival_time = ((uint64_t)att.tv_sec<<32)+((uint64_t)att.tv_nsec<<32)/1000000000;
          // departure_time = ((uint64_t)dtt.tv_sec<<32)+((uint64_t)dtt.tv_nsec<<32)/1000000000;

          return_time = arrival_time - conn->departure_time;

          // uint64_t rtus = (return_time * 1000000) >> 32;

          if (((return_time * 1000000) >> 32) < 300000) {

            // debug(2,"Synchronisation ping return time is %f milliseconds.",(rtus*1.0)/1000);

            // distant_receive_time =
            // ((uint64_t)ntohl(*((uint32_t*)&packet[16])))<<32+ntohl(*((uint32_t*)&packet[20]));

            distant_receive_time = (uint64_t)nctohl(&packet[16]) << 32;
            distant_receive_time += nctohl(&packet[20]);

            // distant_transmit_time =
            // ((uint64_t)ntohl(*((uint32_t*)&packet[24])))<<32+ntohl(*((uint32_t*)&packet[28]));

            distant_transmit_time = (uint64_t)nctohl(&packet[24]) << 32;
            distant_transmit_time += nctohl(&packet[28]);

            uint64_t remote_processing_time = 0;

            if (distant_transmit_time >= distant_receive_time)
              remote_processing_time = distant_transmit_time - distant_receive_time;
            else {
              debug(1, "Yikes: distant_transmit_time is before distant_receive_time; remote "
                       "processing time set to zero.");
            }
            // debug(1,"Return trip time: %" PRIu64 " uS, remote processing time: %" PRIu64 "
            // uS.",(return_time*1000000)>>32,(remote_processing_time*1000000)>>32);

            uint64_t local_time_by_remote_clock = distant_transmit_time + return_time / 2;

            // remove the remote processing time from the record of the return time, as long at the
            // processing time looks sensible.

            if (remote_processing_time < return_time)
              return_time -= remote_processing_time;
            else
              debug(1, "Remote processing time greater than return time -- ignored.");

            int cc;
            for (cc = time_ping_history - 1; cc > 0; cc--) {
              conn->time_pings[cc] = conn->time_pings[cc - 1];
              // if ((conn->time_ping_count) && (conn->time_ping_count < 10))
              //                conn->time_pings[cc].dispersion =
              //                  conn->time_pings[cc].dispersion * pow(2.14,
              //                  1.0/conn->time_ping_count);
              conn->time_pings[cc].dispersion =
                  (conn->time_pings[cc].dispersion * 110) /
                  100; // make the dispersions 'age' by this rational factor
            }
            // these are used for doing a least squares calculation to get the drift
            conn->time_pings[0].local_time = arrival_time;
            conn->time_pings[0].remote_time = distant_transmit_time;
            conn->time_pings[0].sequence_number = sequence_number++;
            conn->time_pings[0].chosen = 0;

            conn->time_pings[0].local_to_remote_difference =
                local_time_by_remote_clock - arrival_time;
            conn->time_pings[0].dispersion = return_time;
            if (conn->time_ping_count < time_ping_history)
              conn->time_ping_count++;

            // here, calculate the mean and standard deviation of the return times

            // mean and variance calculations from "online_variance" algorithm at
            // https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Online_algorithm

            double rtfus = 1.0 * ((return_time * 1000000) >> 32);
            stat_n += 1;
            double stat_delta = rtfus - stat_mean;
            stat_mean += stat_delta / stat_n;
            stat_M2 += stat_delta * (rtfus - stat_mean);
            // debug(1, "Timing packet return time stats: current, mean and standard deviation over
            // %d packets: %.1f, %.1f, %.1f (microseconds).",
            //        stat_n,rtfus,stat_mean, sqrtf(stat_M2 / (stat_n - 1)));

            // here, pick the record with the least dispersion, and record that it's been chosen

            // uint64_t local_time_chosen = arrival_time;
            // uint64_t remote_time_chosen = distant_transmit_time;
            // now pick the timestamp with the lowest dispersion
            uint64_t l2rtd = conn->time_pings[0].local_to_remote_difference;
            uint64_t lt = conn->time_pings[0].local_time;
            uint64_t tld = conn->time_pings[0].dispersion;
            int chosen = 0;
            for (cc = 1; cc < conn->time_ping_count; cc++)
              if (conn->time_pings[cc].dispersion < tld) {
                chosen = cc;
                l2rtd = conn->time_pings[cc].local_to_remote_difference;
                lt = conn->time_pings[cc].local_time;
                tld = conn->time_pings[cc].dispersion;
                // local_time_chosen = conn->time_pings[cc].local_time;
                // remote_time_chosen = conn->time_pings[cc].remote_time;
              }
            // debug(1,"Record %d has the lowest dispersion with %0.2f us
            // dispersion.",chosen,1.0*((tld * 1000000) >> 32));
            conn->time_pings[chosen].chosen = 1; // record the fact that it has been used for timing

            /*
            // calculate the jitter -- the absolute time between the current
            local_to_remote_time_difference and the new one and add it to the total jitter count
            int64_t ji;
            int64_t ltd =0; // local time difference for the jitter

            if (conn->time_ping_count > 1) {
              if (l2rtd > conn->local_to_remote_time_difference) {
                local_to_remote_time_jitter =
                    local_to_remote_time_jitter + l2rtd - conn->local_to_remote_time_difference;
                ji = l2rtd - conn->local_to_remote_time_difference; // this is the difference
            between the present local-to-remote-time-difference and the new one, i.e. the jitter
            step
              } else {
                local_to_remote_time_jitter =
                    local_to_remote_time_jitter + conn->local_to_remote_time_difference - l2rtd;
                ji = -(conn->local_to_remote_time_difference - l2rtd);
              }
              local_to_remote_time_jitter_count += 1;
            }
            if (conn->local_to_remote_time_difference_measurement_time < lt)
              ltd = lt-conn->local_to_remote_time_difference_measurement_time;
            else
              ltd = -(conn->local_to_remote_time_difference_measurement_time-lt);

            if (ltd) {
              debug(1,"Jitter: %" PRId64 " microseconds in %" PRId64 " microseconds.", (ji *
            (int64_t)1000000)>>32, (ltd * (int64_t)1000000)>>32);
              debug(1,"Source clock to local clock drift: %.2f ppm.",((1.0*ji)/ltd)*1000000.0);
            }
            // uncomment below to print jitter between client's clock and our clock

            if (ji) {
              int64_t rtus = (tld*1000000)>>32;
              debug(1,"Choosing time difference[%d] with dispersion of %" PRId64 " us with an
            adjustment of %" PRId64 " us",chosen, rtus, (ji*1000000)>>32);
            }
            */
            conn->local_to_remote_time_difference =
                l2rtd; // make this the new local-to-remote-time-difference
            conn->local_to_remote_time_difference_measurement_time = lt; // done at this time.

            if (first_local_to_remote_time_difference == 0) {
              first_local_to_remote_time_difference = conn->local_to_remote_time_difference;
              // first_local_to_remote_time_difference_time = get_absolute_time_in_fp();
            }

            // here, let's try to use the timing pings that were selected because of their short
            // return times to
            // estimate a figure for drift between the local clock (x) and the remote clock (y)

            // if we plug in a local interval, we will get back what that is in remote time

            // calculate the line of best fit for relating the local time and the remote time
            // we will calculate the slope, which is the drift
            // see https://www.varsitytutors.com/hotmath/hotmath_help/topics/line-of-best-fit

            uint64_t y_bar = 0; // remote timestamp average
            uint64_t x_bar = 0; // local timestamp average
            int sample_count = 0;

            // approximate time in seconds to let the system settle down
            const int settling_time = 60;
            // number of points to have for calculating a valid drift
            const int sample_point_minimum = 8;
            for (cc = 0; cc < conn->time_ping_count; cc++)
              if ((conn->time_pings[cc].chosen) &&
                  (conn->time_pings[cc].sequence_number >
                   (settling_time / 3))) { // wait for a approximate settling time
                y_bar += (conn->time_pings[cc].remote_time >>
                          12); // precision is down to 1/4th of a microsecond
                x_bar += (conn->time_pings[cc].local_time >> 12);
                sample_count++;
              }
            if (sample_count > sample_point_minimum) {
              y_bar = y_bar / sample_count;
              x_bar = x_bar / sample_count;

              int64_t xid, yid;
              int64_t mtl, mbl;
              mtl = 0;
              mbl = 0;
              for (cc = 0; cc < conn->time_ping_count; cc++)
                if ((conn->time_pings[cc].chosen) &&
                    (conn->time_pings[cc].sequence_number > (settling_time / 3))) {

                  uint64_t slt = conn->time_pings[cc].local_time >> 12;
                  if (slt > x_bar)
                    xid = slt - x_bar;
                  else
                    xid = -(x_bar - slt);

                  uint64_t srt = conn->time_pings[cc].remote_time >> 12;
                  if (srt > y_bar)
                    yid = srt - y_bar;
                  else
                    yid = -(y_bar - srt);

                  mtl = mtl + xid * yid;
                  mbl = mbl + xid * xid;
                }
              conn->local_to_remote_time_gradient_sample_count = sample_count;
              if (mbl)
                conn->local_to_remote_time_gradient = (1.0 * mtl) / mbl;
              else {
                conn->local_to_remote_time_gradient = 1.0;
                debug(1, "rtp_timing_receiver: mbl is 0");
              }
            } else {
              conn->local_to_remote_time_gradient = 1.0;
            }
            // debug(1,"local to remote time gradient is %12.2f ppm, based on %d
            // samples.",conn->local_to_remote_time_gradient*1000000,sample_count);
          } else {
            debug(2, "Time ping turnaround time: %lld us -- it looks like a timing ping was lost.",
                  (return_time * 1000000) >> 32);
          }
        } else {
          debug(1, "Timing port -- Unknown RTP packet of type 0x%02X length %d.", packet[1], nread);
        }
      } else {
        debug(3, "Timing Receiver Thread -- dropping incoming packet to simulate a bad network.");
      }
    } else {
      debug(1, "Timing receiver -- error receiving a packet.");
    }
  }

  debug(1, "Timing Receiver RTP thread \"normal\" exit -- this can't happen. Hah!");
  pthread_cleanup_pop(0); // don't execute anything here.
  debug(2, "Timing Receiver RTP thread exit.");
  pthread_exit(NULL);
}

static uint16_t bind_port(int ip_family, const char *self_ip_address, uint32_t scope_id,
                          int *sock) {
  // look for a port in the range, if any was specified.
  int ret = 0;

  int local_socket = socket(ip_family, SOCK_DGRAM, IPPROTO_UDP);
  if (local_socket == -1)
    die("Could not allocate a socket.");

  /*
    int val = 1;
    ret = setsockopt(local_socket, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
    if (ret < 0) {
      char errorstring[1024];
      strerror_r(errno, (char *)errorstring, sizeof(errorstring));
      debug(1, "Error %d: \"%s\". Couldn't set SO_REUSEADDR");
    }
  */

  SOCKADDR myaddr;
  int tryCount = 0;
  uint16_t desired_port;
  do {
    tryCount++;
    desired_port = nextFreeUDPPort();
    memset(&myaddr, 0, sizeof(myaddr));
    if (ip_family == AF_INET) {
      struct sockaddr_in *sa = (struct sockaddr_in *)&myaddr;
      sa->sin_family = AF_INET;
      sa->sin_port = ntohs(desired_port);
      inet_pton(AF_INET, self_ip_address, &(sa->sin_addr));
      ret = bind(local_socket, (struct sockaddr *)sa, sizeof(struct sockaddr_in));
    }
#ifdef AF_INET6
    if (ip_family == AF_INET6) {
      struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)&myaddr;
      sa6->sin6_family = AF_INET6;
      sa6->sin6_port = ntohs(desired_port);
      inet_pton(AF_INET6, self_ip_address, &(sa6->sin6_addr));
      sa6->sin6_scope_id = scope_id;
      ret = bind(local_socket, (struct sockaddr *)sa6, sizeof(struct sockaddr_in6));
    }
#endif

  } while ((ret < 0) && (errno == EADDRINUSE) && (desired_port != 0) &&
           (tryCount < config.udp_port_range));

  // debug(1,"UDP port chosen: %d.",desired_port);

  if (ret < 0) {
    close(local_socket);
    char errorstring[1024];
    strerror_r(errno, (char *)errorstring, sizeof(errorstring));
    die("error %d: \"%s\". Could not bind a UDP port! Check the udp_port_range is large enough -- "
        "it must be "
        "at least 3, and 10 or more is suggested -- or "
        "check for restrictive firewall settings or a bad router! UDP base is %u, range is %u and "
        "current suggestion is %u.",
        errno, errorstring, config.udp_port_base, config.udp_port_range, desired_port);
  }

  uint16_t sport;
  SOCKADDR local;
  socklen_t local_len = sizeof(local);
  getsockname(local_socket, (struct sockaddr *)&local, &local_len);
#ifdef AF_INET6
  if (local.SAFAMILY == AF_INET6) {
    struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)&local;
    sport = ntohs(sa6->sin6_port);
  } else
#endif
  {
    struct sockaddr_in *sa = (struct sockaddr_in *)&local;
    sport = ntohs(sa->sin_port);
  }
  *sock = local_socket;
  return sport;
}

void rtp_setup(SOCKADDR *local, SOCKADDR *remote, uint16_t cport, uint16_t tport,
               rtsp_conn_info *conn) {

  // this gets the local and remote ip numbers (and ports used for the TCD stuff)
  // we use the local stuff to specify the address we are coming from and
  // we use the remote stuff to specify where we're goint to

  if (conn->rtp_running)
    warn("rtp_setup has been called with al already-active stream -- ignored. Possible duplicate "
         "SETUP call?");
  else {

    debug(3, "rtp_setup: cport=%d tport=%d.", cport, tport);

    // print out what we know about the client
    void *client_addr = NULL, *self_addr = NULL;
    // int client_port, self_port;
    // char client_port_str[64];
    // char self_addr_str[64];

    conn->connection_ip_family =
        remote->SAFAMILY; // keep information about the kind of ip of the client

#ifdef AF_INET6
    if (conn->connection_ip_family == AF_INET6) {
      struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)remote;
      client_addr = &(sa6->sin6_addr);
      // client_port = ntohs(sa6->sin6_port);
      sa6 = (struct sockaddr_in6 *)local;
      self_addr = &(sa6->sin6_addr);
      // self_port = ntohs(sa6->sin6_port);
      conn->self_scope_id = sa6->sin6_scope_id;
    }
#endif
    if (conn->connection_ip_family == AF_INET) {
      struct sockaddr_in *sa4 = (struct sockaddr_in *)remote;
      client_addr = &(sa4->sin_addr);
      // client_port = ntohs(sa4->sin_port);
      sa4 = (struct sockaddr_in *)local;
      self_addr = &(sa4->sin_addr);
      // self_port = ntohs(sa4->sin_port);
    }

    inet_ntop(conn->connection_ip_family, client_addr, conn->client_ip_string,
              sizeof(conn->client_ip_string));
    inet_ntop(conn->connection_ip_family, self_addr, conn->self_ip_string,
              sizeof(conn->self_ip_string));

    debug(2, "Connection %d: SETUP -- Connection from %s to self at %s.", conn->connection_number,
          conn->client_ip_string, conn->self_ip_string);

    // set up a the record of the remote's control socket
    struct addrinfo hints;
    struct addrinfo *servinfo;

    memset(&conn->rtp_client_control_socket, 0, sizeof(conn->rtp_client_control_socket));
    memset(&hints, 0, sizeof hints);
    hints.ai_family = conn->connection_ip_family;
    hints.ai_socktype = SOCK_DGRAM;
    char portstr[20];
    snprintf(portstr, 20, "%d", cport);
    if (getaddrinfo(conn->client_ip_string, portstr, &hints, &servinfo) != 0)
      die("Can't get address of client's control port");

#ifdef AF_INET6
    if (servinfo->ai_family == AF_INET6) {
      memcpy(&conn->rtp_client_control_socket, servinfo->ai_addr, sizeof(struct sockaddr_in6));
      // ensure the scope id matches that of remote. this is needed for link-local addresses.
      struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)&conn->rtp_client_control_socket;
      sa6->sin6_scope_id = conn->self_scope_id;
    } else
#endif
      memcpy(&conn->rtp_client_control_socket, servinfo->ai_addr, sizeof(struct sockaddr_in));
    freeaddrinfo(servinfo);

    // set up a the record of the remote's timing socket
    memset(&conn->rtp_client_timing_socket, 0, sizeof(conn->rtp_client_timing_socket));
    memset(&hints, 0, sizeof hints);
    hints.ai_family = conn->connection_ip_family;
    hints.ai_socktype = SOCK_DGRAM;
    snprintf(portstr, 20, "%d", tport);
    if (getaddrinfo(conn->client_ip_string, portstr, &hints, &servinfo) != 0)
      die("Can't get address of client's timing port");
#ifdef AF_INET6
    if (servinfo->ai_family == AF_INET6) {
      memcpy(&conn->rtp_client_timing_socket, servinfo->ai_addr, sizeof(struct sockaddr_in6));
      // ensure the scope id matches that of remote. this is needed for link-local addresses.
      struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)&conn->rtp_client_timing_socket;
      sa6->sin6_scope_id = conn->self_scope_id;
    } else
#endif
      memcpy(&conn->rtp_client_timing_socket, servinfo->ai_addr, sizeof(struct sockaddr_in));
    freeaddrinfo(servinfo);

    // now, we open three sockets -- one for the audio stream, one for the timing and one for the
    // control
    conn->remote_control_port = cport;
    conn->remote_timing_port = tport;

    conn->local_control_port = bind_port(conn->connection_ip_family, conn->self_ip_string,
                                         conn->self_scope_id, &conn->control_socket);
    conn->local_timing_port = bind_port(conn->connection_ip_family, conn->self_ip_string,
                                        conn->self_scope_id, &conn->timing_socket);
    conn->local_audio_port = bind_port(conn->connection_ip_family, conn->self_ip_string,
                                       conn->self_scope_id, &conn->audio_socket);

    debug(3, "listening for audio, control and timing on ports %d, %d, %d.", conn->local_audio_port,
          conn->local_control_port, conn->local_timing_port);

    conn->reference_timestamp = 0;
    // pthread_create(&rtp_audio_thread, NULL, &rtp_audio_receiver, NULL);
    // pthread_create(&rtp_control_thread, NULL, &rtp_control_receiver, NULL);
    // pthread_create(&rtp_timing_thread, NULL, &rtp_timing_receiver, NULL);

    conn->request_sent = 0;
    conn->rtp_running = 1;

#ifdef CONFIG_METADATA
    send_ssnc_metadata('clip', strdup(conn->client_ip_string), strlen(conn->client_ip_string), 1);
    send_ssnc_metadata('svip', strdup(conn->self_ip_string), strlen(conn->self_ip_string), 1);
#endif
  }
}

void get_reference_timestamp_stuff(uint32_t *timestamp, uint64_t *timestamp_time,
                                   uint64_t *remote_timestamp_time, rtsp_conn_info *conn) {
  // types okay
  debug_mutex_lock(&conn->reference_time_mutex, 1000, 0);
  *timestamp = conn->reference_timestamp;
  *remote_timestamp_time = conn->remote_reference_timestamp_time;
  *timestamp_time =
      conn->remote_reference_timestamp_time - local_to_remote_time_difference_now(conn);
  // if ((*timestamp == 0) && (*timestamp_time == 0)) {
  //  debug(1,"Reference timestamp is invalid.");
  //}
  debug_mutex_unlock(&conn->reference_time_mutex, 0);
}

void clear_reference_timestamp(rtsp_conn_info *conn) {
  debug_mutex_lock(&conn->reference_time_mutex, 1000, 1);
  conn->reference_timestamp = 0;
  conn->remote_reference_timestamp_time = 0;
  debug_mutex_unlock(&conn->reference_time_mutex, 3);
}

int have_timestamp_timing_information(rtsp_conn_info *conn) {
  if (conn->reference_timestamp == 0)
    return 0;
  else
    return 1;
}

// set this to zero to use the rates supplied by the sources, which might not always be completely
// right...
const int use_nominal_rate = 0; // specify whether to use the nominal input rate, usually 44100 fps

int sanitised_source_rate_information(uint32_t *frames, uint64_t *time, rtsp_conn_info *conn) {
  int result = 1;
  uint32_t fs = conn->input_rate;
  *frames = fs;
  uint64_t one_fp = (uint64_t)(0x100000000); // one second in fp form
  *time = one_fp;
  if ((conn->initial_reference_time) && (conn->initial_reference_timestamp)) {
    //    uint32_t local_frames = conn->reference_timestamp - conn->initial_reference_timestamp;
    uint32_t local_frames =
        modulo_32_offset(conn->initial_reference_timestamp, conn->reference_timestamp);
    uint64_t local_time = conn->remote_reference_timestamp_time - conn->initial_reference_time;
    if ((local_frames == 0) || (local_time == 0) || (use_nominal_rate)) {
      result = 1;
    } else {
      double calculated_frame_rate = conn->input_rate;
      if (local_time)
        calculated_frame_rate = ((1.0 * local_frames) / local_time) * one_fp;
      else
        debug(1, "sanitised_source_rate_information: local_time is zero");
      if ((local_time == 0) || ((calculated_frame_rate / conn->input_rate) > 1.002) ||
          ((calculated_frame_rate / conn->input_rate) < 0.998)) {
        debug(3, "input frame rate out of bounds at %.2f fps.", calculated_frame_rate);
        result = 1;
      } else {
        *frames = local_frames;
        *time = local_time;
        result = 0;
      }
    }
  }
  return result;
}

// the timestamp is a timestamp calculated at the input rate
// the reference timestamps are denominated in terms of the input rate

int frame_to_local_time(uint32_t timestamp, uint64_t *time, rtsp_conn_info *conn) {
  debug_mutex_lock(&conn->reference_time_mutex, 1000, 0);
  int result = 0;
  uint64_t time_difference;
  uint32_t frame_difference;
  result = sanitised_source_rate_information(&frame_difference, &time_difference, conn);

  uint64_t timestamp_interval_time;
  uint64_t remote_time_of_timestamp;
  uint32_t timestamp_interval = modulo_32_offset(conn->reference_timestamp, timestamp);
  if (timestamp_interval <=
      conn->input_rate * 3600) { // i.e. timestamp was really after the reference timestamp
    timestamp_interval_time = (timestamp_interval * time_difference) /
                              frame_difference; // this is the nominal time, based on the
                                                // fps specified between current and
                                                // previous sync frame.
    remote_time_of_timestamp = conn->remote_reference_timestamp_time +
                               timestamp_interval_time; // based on the reference timestamp time
                                                        // plus the time interval calculated based
                                                        // on the specified fps.
  } else { // i.e. timestamp was actually before the reference timestamp
    timestamp_interval =
        modulo_32_offset(timestamp, conn->reference_timestamp); // fix the calculation
    timestamp_interval_time = (timestamp_interval * time_difference) /
                              frame_difference; // this is the nominal time, based on the
                                                // fps specified between current and
                                                // previous sync frame.
    remote_time_of_timestamp = conn->remote_reference_timestamp_time -
                               timestamp_interval_time; // based on the reference timestamp time
                                                        // plus the time interval calculated based
                                                        // on the specified fps.
  }
  *time = remote_time_of_timestamp - local_to_remote_time_difference_now(conn);
  debug_mutex_unlock(&conn->reference_time_mutex, 0);
  return result;
}

int local_time_to_frame(uint64_t time, uint32_t *frame, rtsp_conn_info *conn) {
  debug_mutex_lock(&conn->reference_time_mutex, 1000, 0);
  int result = 0;

  uint64_t time_difference;
  uint32_t frame_difference;
  result = sanitised_source_rate_information(&frame_difference, &time_difference, conn);

  // first, get from [local] time to remote time.
  uint64_t remote_time = time + local_to_remote_time_difference_now(conn);
  // next, get the remote time interval from the remote_time to the reference time
  uint64_t time_interval;

  // here, we calculate the time interval, in terms of remote time
  uint64_t offset = modulo_64_offset(conn->remote_reference_timestamp_time, remote_time);
  int reference_time_was_earlier = (offset <= (uint64_t)0x100000000 * 3600);
  if (reference_time_was_earlier) // if we haven't had a reference within the last hour, it'll be
                                  // taken as afterwards
    time_interval = remote_time - conn->remote_reference_timestamp_time;
  else
    time_interval = conn->remote_reference_timestamp_time - remote_time;

  // now, convert the remote time interval into frames using the frame rate we have observed or
  // which has been nominated
  uint32_t frame_interval = 0;
  if (time_difference)
    frame_interval = (time_interval * frame_difference) / time_difference;
  else
    debug(1, "local_time_to_frame: time_difference is zero");
  if (reference_time_was_earlier) {
    // debug(1,"Frame interval is %" PRId64 " frames.",frame_interval);
    *frame = (conn->reference_timestamp + frame_interval);
  } else {
    // debug(1,"Frame interval is %" PRId64 " frames.",-frame_interval);
    *frame = (conn->reference_timestamp - frame_interval);
  }
  debug_mutex_unlock(&conn->reference_time_mutex, 0);
  return result;
}

void rtp_request_resend(seq_t first, uint32_t count, rtsp_conn_info *conn) {
  if (conn->rtp_running) {
    // if (!request_sent) {
    // debug(2, "requesting resend of %d packets starting at %u.", count, first);
    //  request_sent = 1;
    //}

    char req[8]; // *not* a standard RTCP NACK
    req[0] = 0x80;
    req[1] = (char)0x55 | (char)0x80;            // Apple 'resend'
    *(unsigned short *)(req + 2) = htons(1);     // our sequence number
    *(unsigned short *)(req + 4) = htons(first); // missed seqnum
    *(unsigned short *)(req + 6) = htons(count); // count
    socklen_t msgsize = sizeof(struct sockaddr_in);
#ifdef AF_INET6
    if (conn->rtp_client_control_socket.SAFAMILY == AF_INET6) {
      msgsize = sizeof(struct sockaddr_in6);
    }
#endif
    uint64_t time_of_sending_fp = get_absolute_time_in_fp();
    uint64_t resend_error_backoff_time = (uint64_t)1000000 * 0.3; // 0.3 seconds
    resend_error_backoff_time = (resend_error_backoff_time << 32) / 1000000;
    if ((conn->rtp_time_of_last_resend_request_error_fp == 0) ||
        ((time_of_sending_fp - conn->rtp_time_of_last_resend_request_error_fp) >
         resend_error_backoff_time)) {
      if ((config.diagnostic_drop_packet_fraction == 0.0) ||
          (drand48() > config.diagnostic_drop_packet_fraction)) {
        // put a time limit on the sendto

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;

        if (setsockopt(conn->control_socket, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout,
                       sizeof(timeout)) < 0)
          debug(1, "Can't set timeout on resend request socket.");

        if (sendto(conn->control_socket, req, sizeof(req), 0,
                   (struct sockaddr *)&conn->rtp_client_control_socket, msgsize) == -1) {
          char em[1024];
          strerror_r(errno, em, sizeof(em));
          debug(2, "Error %d using sendto to request a resend: \"%s\".",
                errno, em);
          conn->rtp_time_of_last_resend_request_error_fp = time_of_sending_fp;
        } else {
          conn->rtp_time_of_last_resend_request_error_fp = 0;
        }

      } else {
        debug(
            3,
            "Dropping resend request packet to simulate a bad network. Backing off for 0.3 "
            "second.");
        conn->rtp_time_of_last_resend_request_error_fp = time_of_sending_fp;
      }
    } else {
      debug(1, "Suppressing a resend request due to a resend sendto error in the last 0.3 seconds.");
    }
  } else {
    // if (!request_sent) {
    debug(2, "rtp_request_resend called without active stream!");
    //  request_sent = 1;
    //}
  }
}
