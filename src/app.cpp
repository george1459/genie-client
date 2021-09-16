// -*- mode: cpp; indent-tabs-mode: nil; c-basic-offset: 2 -*-
//
// This file is part of Genie
//
// Copyright 2021 The Board of Trustees of the Leland Stanford Junior University
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <glib-unix.h>
#include <glib.h>

#include "app.hpp"
#include "audioinput.hpp"
#include "audioplayer.hpp"
#include "config.hpp"
#include "conversation_client.hpp"
#include "dns_controller.hpp"
#include "evinput.hpp"
#include "leds.hpp"
#include "spotifyd.hpp"
#include "stt.hpp"

double time_diff(struct timeval x, struct timeval y) {
  return (((double)y.tv_sec * 1000000 + (double)y.tv_usec) -
          ((double)x.tv_sec * 1000000 + (double)x.tv_usec));
}

double time_diff_ms(struct timeval x, struct timeval y) {
  return time_diff(x, y) / 1000;
}

genie::App::App() {
  is_processing = FALSE;

  // initialize a shared SoupSession to be used by all outgoing connections
  soup_session =
      auto_gobject_ptr<SoupSession>(soup_session_new(), adopt_mode::owned);
  // enable the wss support
  const gchar *wss_aliases[] = {"wss", NULL};
  g_object_set(soup_session.get(), SOUP_SESSION_HTTPS_ALIASES, wss_aliases,
               NULL);
}

genie::App::~App() { g_main_loop_unref(main_loop); }

int genie::App::exec() {
  PROF_PRINT("start main loop\n");

  main_loop = g_main_loop_new(NULL, FALSE);

  g_unix_signal_add(SIGINT, sig_handler, main_loop);
  g_unix_signal_add(SIGTERM, sig_handler, main_loop);

  config = std::make_unique<Config>();
  config->load();

  g_setenv("GST_REGISTRY_UPDATE", "no", true);

  audio_player = std::make_unique<AudioPlayer>(this);

  stt = std::make_unique<STT>(this);

  audio_input = std::make_unique<AudioInput>(this);
  audio_input->init();

  spotifyd = std::make_unique<Spotifyd>(this);
  spotifyd->init();

  conversation_client = std::make_unique<ConversationClient>(this);
  conversation_client->init();

  ev_input = std::make_unique<EVInput>(this);
  ev_input->init();

  leds = std::make_unique<Leds>(this);
  leds->init();
  leds->set_active(false);

  if (config->dns_controller_enabled)
    dns_controller = std::make_unique<DNSController>();

  this->current_state = new state::Sleeping(this);
  this->current_state->enter();

  g_debug("start main loop\n");
  g_main_loop_run(main_loop);
  g_debug("main loop returned\n");

  return 0;
}

gboolean genie::App::sig_handler(gpointer data) {
  GMainLoop *loop = reinterpret_cast<GMainLoop *>(data);
  g_main_loop_quit(loop);
  return G_SOURCE_REMOVE;
}

void genie::App::duck() {
  system("amixer -D hw:audiocodec cset name='hd' 128");
}

void genie::App::unduck() {
  system("amixer -D hw:audiocodec cset name='hd' 255");
}

void genie::App::print_processing_entry(const char *name, double duration_ms,
                                        double total_ms) {
  g_print("%12s: %5.3lf ms (%3d%%)\n", name, duration_ms,
          (int)((duration_ms / total_ms) * 100));
}

/**
 * @brief Track an event that is part of a turn's remote processing.
 *
 * We want to keep a close eye the performance of our
 *
 * @param eventType
 */
void genie::App::track_processing_event(ProcessingEvent_t eventType) {
  // Unless we are starting a turn or already in a turn just bail out. This
  // avoids tracking the "Hi..." and any other messages at connect.
  if (!(eventType == ProcessingEvent_t::BEGIN || is_processing)) {
    return;
  }

  switch (eventType) {
    case ProcessingEvent_t::BEGIN:
      gettimeofday(&tStartProcessing, NULL);
      is_processing = true;
      break;
    case ProcessingEvent_t::START_STT:
      gettimeofday(&tStartSTT, NULL);
      break;
    case ProcessingEvent_t::END_STT:
      gettimeofday(&tEndSTT, NULL);
      break;
    case ProcessingEvent_t::START_GENIE:
      gettimeofday(&tStartGenie, NULL);
      break;
    case ProcessingEvent_t::END_GENIE:
      gettimeofday(&tEndGenie, NULL);
      break;
    case ProcessingEvent_t::START_TTS:
      gettimeofday(&tStartTTS, NULL);
      break;
    case ProcessingEvent_t::END_TTS:
      gettimeofday(&tEndTTS, NULL);
      break;
    case ProcessingEvent_t::FINISH:
      int total_ms = time_diff_ms(tStartProcessing, tEndTTS);

      g_print("############# Processing Performance #################\n");
      print_processing_entry(
          "Pre-STT", time_diff_ms(tStartProcessing, tStartSTT), total_ms);
      print_processing_entry("STT", time_diff_ms(tStartSTT, tEndSTT), total_ms);
      print_processing_entry("STT->Genie", time_diff_ms(tEndSTT, tStartGenie),
                             total_ms);
      print_processing_entry("Genie", time_diff_ms(tStartGenie, tEndGenie),
                             total_ms);
      print_processing_entry("Genie->TTS", time_diff_ms(tEndGenie, tStartTTS),
                             total_ms);
      print_processing_entry("TTS", time_diff_ms(tStartTTS, tEndTTS), total_ms);
      g_print("------------------------------------------------------\n");
      print_processing_entry("Total", total_ms, total_ms);
      g_print("######################################################\n");

      is_processing = false;
      break;
  }
}
