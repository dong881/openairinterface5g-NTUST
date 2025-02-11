/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this file
 * except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/epoll.h>
#include <netdb.h>
#include <pthread.h>

#include <common/utils/assertions.h>
#include <common/utils/LOG/log.h>
#include <common/utils/load_module_shlib.h>
#include <common/utils/telnetsrv/telnetsrv.h>
#include <common/config/config_userapi.h>
#include "common_lib.h"
#include "shm_td_iq_channel.h"

// Simulator role
typedef enum { ROLE_SERVER = 1, ROLE_CLIENT } role;

#define ROLE_CLIENT_STRING "client"
#define ROLE_SERVER_STRING "server"

#define SHM_RADIO_SECTION "shm_radio"
#define TIME_SCALE_HLP \
  "sample time scale. 1.0 means realtime. Values > 1 mean faster than realtime. Values < 1 mean slower than realtime\n"

// clang-format off
#define SHM_RADIO_PARAMS_DESC \
  { \
     {"channel_name", "shared memory channel name\n",  0, .strptr = &channel_name,               .defstrval = "shm_radio_channel", TYPE_STRING, 0}, \
     {"role",         "either client or server\n",     0, .strptr = &role,                       .defstrval = ROLE_CLIENT_STRING,  TYPE_STRING, 0}, \
     {"timescale",    TIME_SCALE_HLP,                  0, .dblptr = &shm_radio_state->timescale, .defdblval = 1.0,                 TYPE_DOUBLE, 0}, \
  };
// clang-format on

typedef struct {
  int role;
  char channel_name[40];
  ShmTDIQChannel *channel;
  uint64_t last_received_sample;
  pthread_t timing_thread;
  bool run_timing_thread;
  double timescale;
  double sample_rate;
  uint64_t tx_samples_late;
  uint64_t tx_early;
  uint64_t tx_samples_total;
  uint64_t rx_samples_late;
  uint64_t rx_early;
  uint64_t rx_samples_total;
  double average_tx_budget;
} shm_radio_state_t;

static void shm_radio_readconfig(shm_radio_state_t *shm_radio_state)
{
  char *channel_name = NULL;
  char *role = NULL;
  paramdef_t shm_radio_params[] = SHM_RADIO_PARAMS_DESC;
  int ret = config_get(config_get_if(), shm_radio_params, sizeofArray(shm_radio_params), SHM_RADIO_SECTION);
  AssertFatal(ret >= 0, "configuration couldn't be performed\n");
  strncpy(shm_radio_state->channel_name, channel_name, sizeof(shm_radio_state->channel_name) - 1);
  if (strncmp(role, ROLE_CLIENT_STRING, strlen(ROLE_CLIENT_STRING)) == 0) {
    shm_radio_state->role = ROLE_CLIENT;
  } else if (strncmp(role, ROLE_SERVER_STRING, strlen(ROLE_SERVER_STRING)) == 0) {
    shm_radio_state->role = ROLE_SERVER;
  } else {
    AssertFatal(false, "Invalid role configuration\n");
  }
}

static void *shm_radio_timing_job(void *arg)
{
  shm_radio_state_t *shm_radio_state = arg;
  struct timespec timestamp;
  if (clock_gettime(CLOCK_REALTIME, &timestamp)) {
    LOG_E(UTIL, "clock_gettime failed\n");
    exit(1);
  }
  double leftover_samples = 0;
  while (shm_radio_state->run_timing_thread) {
    struct timespec current_time;
    if (clock_gettime(CLOCK_REALTIME, &current_time)) {
      LOG_E(UTIL, "clock_gettime failed\n");
      exit(1);
    }
    uint64_t diff = (current_time.tv_sec - timestamp.tv_sec) * 1000000000 + (current_time.tv_nsec - timestamp.tv_nsec);
    timestamp = current_time;
    double samples_to_produce = shm_radio_state->sample_rate * shm_radio_state->timescale * diff / 1e9;

    // Attempt to correct compounding rounding error
    leftover_samples += samples_to_produce - (uint64_t)samples_to_produce;
    if (leftover_samples > 1.0f) {
      samples_to_produce += 1;
      leftover_samples -= 1;
    }
    shm_td_iq_channel_produce_samples(shm_radio_state->channel, samples_to_produce);
    usleep(1);
  }
  return 0;
}

static int shm_radio_connect(openair0_device *device)
{
  shm_radio_state_t *shm_radio_state = (shm_radio_state_t *)device->priv;
  if (shm_radio_state->role == ROLE_SERVER) {
    shm_radio_state->channel = shm_td_iq_channel_create(shm_radio_state->channel_name, 1, 1);
    shm_radio_state->run_timing_thread = true;
    while (!shm_td_iq_channel_is_connected(shm_radio_state->channel)) {
      LOG_I(HW, "Waiting for client\n");
      sleep(1);
    }
    int ret = pthread_create(&shm_radio_state->timing_thread, NULL, shm_radio_timing_job, shm_radio_state);
    AssertFatal(ret == 0, "pthread_create() failed: errno: %d, %s\n", errno, strerror(errno));
  } else {
    shm_radio_state->channel = shm_td_iq_channel_connect(shm_radio_state->channel_name, 10);
  }
  return 0;
}

static int shm_radio_write(openair0_device *device,
                           openair0_timestamp timestamp,
                           void **samplesVoid,
                           int nsamps,
                           int nbAnt,
                           int flags)
{
  timestamp -= device->openair0_cfg->command_line_sample_advance;
  shm_radio_state_t *shm_radio_state = (shm_radio_state_t *)device->priv;
  uint64_t sample = shm_td_iq_channel_get_current_sample(shm_radio_state->channel);
  int64_t diff = timestamp - sample;
  double budget = diff / (shm_radio_state->sample_rate / 1e6);
  shm_radio_state->average_tx_budget = .05 * budget + .95 * shm_radio_state->average_tx_budget;

  int ret = shm_td_iq_channel_tx(shm_radio_state->channel, timestamp, nsamps, 0, samplesVoid[0]);
  if (ret == CHANNEL_ERROR_TOO_LATE) {
    shm_radio_state->tx_samples_late += nsamps;
  } else if (ret == CHANNEL_ERROR_TOO_EARLY) {
    shm_radio_state->tx_early += 1;
  }
  shm_radio_state->tx_samples_total += nsamps;
  return nsamps;
}

static int shm_radio_read(openair0_device *device, openair0_timestamp *ptimestamp, void **samplesVoid, int nsamps, int nbAnt)
{
  shm_radio_state_t *shm_radio_state = (shm_radio_state_t *)device->priv;
  shm_td_iq_channel_wait(shm_radio_state->channel, shm_radio_state->last_received_sample + nsamps);
  int ret = shm_td_iq_channel_rx(shm_radio_state->channel, shm_radio_state->last_received_sample, nsamps, 0, samplesVoid[0]);
  if (ret == CHANNEL_ERROR_TOO_LATE) {
    shm_radio_state->rx_samples_late += nsamps;
  } else if (ret == CHANNEL_ERROR_TOO_EARLY) {
    shm_radio_state->rx_early += 1;
  }
  shm_radio_state->rx_samples_total += nsamps;
  *ptimestamp = shm_radio_state->last_received_sample;
  shm_radio_state->last_received_sample += nsamps;
  return nsamps;
}

static void shm_radio_end(openair0_device *device)
{
  shm_radio_state_t *shm_radio_state = (shm_radio_state_t *)device->priv;
  if (shm_radio_state->role == ROLE_SERVER) {
    shm_radio_state->run_timing_thread = false;
    int ret = pthread_join(shm_radio_state->timing_thread, NULL);
    AssertFatal(ret == 0, "pthread_join() failed: errno: %d, %s\n", errno, strerror(errno));
  }
  LOG_I(HW,
        "SHM_RADIO: Realtime issues: TX %.2f%%, RX %.2f%%\n",
        shm_radio_state->tx_samples_late / (float)shm_radio_state->tx_samples_total * 100,
        shm_radio_state->rx_samples_late / (float)shm_radio_state->rx_samples_total * 100);
  LOG_I(HW,
        "SHM_RADIO: Read/write too early (suspected radio implementaton error) TX: %lu, RX: %lu\n",
        shm_radio_state->tx_early,
        shm_radio_state->rx_early);
  LOG_I(HW, "SHM_RADIO: Average TX budget %.3lf uS\n", shm_radio_state->average_tx_budget);
  shm_td_iq_channel_destroy(shm_radio_state->channel);
}

static int shm_radio_stub(openair0_device *device)
{
  return 0;
}
static int shm_radio_stub2(openair0_device *device, openair0_config_t *openair0_cfg)
{
  return 0;
}

__attribute__((__visibility__("default"))) int device_init(openair0_device *device, openair0_config_t *openair0_cfg)
{
  shm_radio_state_t *shm_radio_state = calloc(1, sizeof(shm_radio_state_t));
  shm_radio_readconfig(shm_radio_state);
  LOG_I(HW,
        "Running as %s\n",
        shm_radio_state->role == ROLE_SERVER ? "server: waiting for client to connect"
                                             : "client: will connect to a shm_radio server");
  device->trx_start_func = shm_radio_connect;
  device->trx_reset_stats_func = shm_radio_stub;
  device->trx_end_func = shm_radio_end;
  device->trx_stop_func = shm_radio_stub;
  device->trx_set_freq_func = shm_radio_stub2;
  device->trx_set_gains_func = shm_radio_stub2;
  device->trx_write_func = shm_radio_write;
  device->trx_read_func = shm_radio_read;

  device->type = RFSIMULATOR;
  device->openair0_cfg = &openair0_cfg[0];
  device->priv = shm_radio_state;
  device->trx_write_init = shm_radio_stub;
  shm_radio_state->last_received_sample = 0;
  shm_radio_state->sample_rate = openair0_cfg->sample_rate;
  return 0;
}
