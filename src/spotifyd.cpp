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

#include "spotifyd.hpp"
#include <fcntl.h>
#include <glib-unix.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>

#include <vector>

#define SPOTIFYD_VERSION "0.3.4"

genie::Spotifyd::Spotifyd(App *app) : app(app) {
  child_pid = -1;

  struct utsname un;
  if (!uname(&un)) {
    arch = un.machine;
  }
}

genie::Spotifyd::~Spotifyd() { close(); }

int genie::Spotifyd::download() {
  const gchar *dlArch;
  if (arch == "armv7l") {
    dlArch = "armhf-";
  } else if (arch == "aarch64") {
    dlArch = "arm64-";
  } else {
    dlArch = "";
  }

  gchar *cmd = g_strdup_printf(
      "curl "
      "https://github.com/stanford-oval/spotifyd/releases/download/v%s/"
      "spotifyd-linux-%sslim.tar.gz -L | tar -xvz -C %s",
      SPOTIFYD_VERSION, dlArch, app->config->cache_dir);
  int rc = system(cmd);
  g_free(cmd);

  return rc;
}

int genie::Spotifyd::check_version() {
  FILE *fp;
  char buf[128];

  gchar *cmd = g_strdup_printf(
      "%s/spotifyd --version", app->config->cache_dir);
  fp = popen(cmd, "r");
  if (fp == NULL) {
    g_free(cmd);
    return true;
  }
  g_free(cmd);

  bool update = false;
  if (fscanf(fp, "spotifyd %127s", buf) == 1) {
    if (strlen(buf) <= 0) {
      g_message("unable to get local spotifyd version, updating...");
      update = true;
    } else {
      if (strcmp(SPOTIFYD_VERSION, buf) != 0) {
        g_message("spotifyd local version %s, need %s, updating...", buf, SPOTIFYD_VERSION);
        update = true;
      }
    }
  }

  if (pclose(fp) == -1) {
    return true;
  }

  return update;
}

int genie::Spotifyd::init() {
  gchar *filePath = g_strdup_printf("%s/spotifyd", app->config->cache_dir);
  if (!g_file_test(filePath, G_FILE_TEST_IS_EXECUTABLE)) {
    download();
  } else {
    if (check_version()) {
      download();
    }
  }
  g_free(filePath);

  if (!username.empty() && !access_token.empty())
    return spawn();
  return true;
}

int genie::Spotifyd::close() {
  if (child_pid > 0) {
    g_debug("kill spotifyd pid %d\n", child_pid);
    return kill(child_pid, SIGINT);
  }
  return true;
}

void genie::Spotifyd::child_watch_cb(GPid pid, gint status, gpointer data) {
  Spotifyd *obj = reinterpret_cast<Spotifyd *>(data);
  g_print("Child %" G_PID_FORMAT " exited with rc %d\n", pid,
          g_spawn_check_exit_status(status, NULL));
  g_spawn_close_pid(pid);
}

int genie::Spotifyd::spawn() {
  gchar *filePath = g_strdup_printf("%s/spotifyd", app->config->cache_dir);
  const gchar *deviceName = "genie-cpp";
  const gchar *backend;
  std::string config_backend(app->config->audio_backend);
  if (config_backend == "pulse") {
    backend = "pulseaudio";
  } else {
    backend = "alsa";
  }

  gchar **envp;
  envp = g_get_environ();
  envp = g_environ_setenv(envp, "PULSE_PROP_media.role", "music", TRUE);

  std::vector<const gchar *> argv{
      filePath,        "--no-daemon",
      "--device",      app->config->audioOutputDeviceMusic,
      "--device-name", deviceName,
      "--device-type", "speaker",
      "--backend",     backend,
      "--username",    username.c_str(),
      "--token",       access_token.c_str(),
      nullptr,
  };

  g_debug("spawn spotifyd");
  for (int i = 0; argv[i]; i++) {
    g_debug("%s", argv[i]);
  }

  GError *gerror = NULL;
  g_spawn_async_with_pipes(NULL, (gchar **)argv.data(), envp,
                           G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &child_pid,
                           NULL, NULL, NULL, &gerror);
  if (gerror) {
    g_strfreev(envp);
    g_free(filePath);
    g_critical("Spawning spotifyd child failed: %s\n", gerror->message);
    g_error_free(gerror);
    return false;
  }

  g_strfreev(envp);
  g_free(filePath);

  g_child_watch_add(child_pid, child_watch_cb, this);
  g_print("spotifyd loaded, pid: %d\n", child_pid);
  return true;
}

bool genie::Spotifyd::set_credentials(const std::string &username,
                                      const std::string &access_token) {
  if (access_token.empty())
    return false;

  if (this->username == username && this->access_token == access_token)
    return true;

  this->username = username;
  this->access_token = access_token;
  g_debug("setting spotify username %s access token %s", this->username.c_str(),
          this->access_token.c_str());
  close();
  g_usleep(500);
  return spawn();
}
