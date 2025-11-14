#include "nfapi_delay_mgmt.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>

#define US_IN_MS 1000ULL
#define NS_IN_US 1000ULL
#define NS_IN_MS 1000000ULL
#define FRAME_DURATION_US 10000ULL
#define FRAME_DURATION_NS 10000000ULL

static inline uint64_t timeval_to_us(const struct timeval *tv)
{
  return (uint64_t)tv->tv_sec * 1000000ULL + (uint64_t)tv->tv_usec;
}

static inline int64_t timeval_diff_us(const struct timeval *end, const struct timeval *begin)
{
  return (int64_t)(end->tv_sec - begin->tv_sec) * 1000000LL + (int64_t)(end->tv_usec - begin->tv_usec);
}

static nfapi_timing_window_config_t *get_window_config(nfapi_delay_mgmt_state_t *state, nfapi_msg_type_e type)
{
  switch (type) {
    case NFAPI_MSG_TYPE_DL_TTI:
      return &state->dl_tti_config;
    case NFAPI_MSG_TYPE_UL_TTI:
      return &state->ul_tti_config;
    case NFAPI_MSG_TYPE_UL_DCI:
      return &state->ul_dci_config;
    case NFAPI_MSG_TYPE_TX_DATA:
      return &state->tx_data_config;
    default:
      return NULL;
  }
}

static nfapi_jitter_state_t *get_jitter_state(nfapi_delay_mgmt_state_t *state, nfapi_msg_type_e type)
{
  switch (type) {
    case NFAPI_MSG_TYPE_DL_TTI:
      return &state->dl_tti_jitter;
    case NFAPI_MSG_TYPE_UL_TTI:
      return &state->ul_tti_jitter;
    case NFAPI_MSG_TYPE_UL_DCI:
      return &state->ul_dci_jitter;
    case NFAPI_MSG_TYPE_TX_DATA:
      return &state->tx_data_jitter;
    default:
      return NULL;
  }
}

static nfapi_message_stats_t *get_stats(nfapi_delay_mgmt_state_t *state, nfapi_msg_type_e type)
{
  switch (type) {
    case NFAPI_MSG_TYPE_DL_TTI:
      return &state->dl_tti_stats;
    case NFAPI_MSG_TYPE_UL_TTI:
      return &state->ul_tti_stats;
    case NFAPI_MSG_TYPE_UL_DCI:
      return &state->ul_dci_stats;
    case NFAPI_MSG_TYPE_TX_DATA:
      return &state->tx_data_stats;
    default:
      return NULL;
  }
}

static void reset_stats(nfapi_message_stats_t *stats)
{
  stats->latest_delay = INT32_MIN;
  stats->earliest_arrival = INT32_MAX;
  stats->num_on_time = 0;
  stats->num_too_early = 0;
  stats->num_too_late = 0;
}

static void update_stats(nfapi_message_stats_t *stats, int32_t delta, nfapi_msg_arrival_result_e result)
{
  if (delta > stats->latest_delay)
    stats->latest_delay = delta;
  if (delta < stats->earliest_arrival)
    stats->earliest_arrival = delta;

  switch (result) {
    case NFAPI_MSG_ARRIVAL_ON_TIME:
      stats->num_on_time++;
      break;
    case NFAPI_MSG_ARRIVAL_TOO_EARLY:
      stats->num_too_early++;
      break;
    case NFAPI_MSG_ARRIVAL_TOO_LATE:
      stats->num_too_late++;
      break;
  }
}

static uint64_t calc_slot_start_ns(uint16_t sfn, uint16_t slot, uint8_t scs)
{
  const uint64_t slot_duration_ns = NS_IN_MS >> (scs > 4 ? 4 : scs);
  return FRAME_DURATION_NS * sfn + slot_duration_ns * slot;
}

static uint64_t calc_slot_start_us(uint16_t sfn, uint16_t slot, uint8_t scs)
{
  const uint64_t slot_ns = calc_slot_start_ns(sfn, slot, scs);
  return (slot_ns + (NS_IN_US / 2ULL)) / NS_IN_US;
}

static uint64_t timestamp_from_ref(const nfapi_delay_mgmt_state_t *state, const struct timeval *recv_time)
{
  if (!state->time_reference_valid)
    return 0;
  return (uint64_t)timeval_diff_us(recv_time, &state->sfn_slot_zero_time);
}

void nfapi_delay_mgmt_init(nfapi_delay_mgmt_state_t *state)
{
  memset(state, 0, sizeof(*state));
  reset_stats(&state->dl_tti_stats);
  reset_stats(&state->ul_tti_stats);
  reset_stats(&state->ul_dci_stats);
  reset_stats(&state->tx_data_stats);
}

void nfapi_delay_mgmt_configure_window(nfapi_delay_mgmt_state_t *state,
                                       nfapi_msg_type_e msg_type,
                                       uint32_t timing_offset_us,
                                       uint16_t timing_window_us)
{
  nfapi_timing_window_config_t *cfg = get_window_config(state, msg_type);
  if (!cfg)
    return;

  cfg->timing_offset_us = timing_offset_us;
  cfg->timing_window_us = timing_window_us;
  cfg->enabled = timing_window_us > 0;
}

void nfapi_delay_mgmt_configure_timing_info(nfapi_delay_mgmt_state_t *state,
                                            uint8_t mode,
                                            uint8_t period)
{
  state->timing_info_mode = mode;
  state->timing_info_period = period;
  state->slot_counter = 0;
}

void nfapi_delay_mgmt_set_time_reference(nfapi_delay_mgmt_state_t *state,
                                         struct timeval *ref_time)
{
  if (!ref_time)
    return;

  state->sfn_slot_zero_time = *ref_time;
  state->time_reference_valid = 1;
  state->last_timing_info_time = *ref_time;
}

uint32_t nfapi_delay_mgmt_get_transmit_timestamp(nfapi_delay_mgmt_state_t *state)
{
  if (!state->time_reference_valid)
    return 0;

  struct timeval now;
  gettimeofday(&now, NULL);
  return (uint32_t)timestamp_from_ref(state, &now);
}

nfapi_msg_arrival_result_e nfapi_delay_mgmt_check_message_arrival(
    nfapi_delay_mgmt_state_t *state,
    nfapi_msg_type_e msg_type,
    uint16_t sfn,
    uint16_t slot,
    uint32_t transmit_timestamp,
    struct timeval *receive_time)
{
  nfapi_timing_window_config_t *cfg = get_window_config(state, msg_type);
  nfapi_message_stats_t *stats = get_stats(state, msg_type);
  if (!cfg || !stats || !cfg->enabled || !receive_time)
    return NFAPI_MSG_ARRIVAL_ON_TIME;

  const uint64_t slot_start_us = calc_slot_start_us(sfn, slot, state->subcarrier_spacing);
  const uint64_t expected_us = (cfg->timing_offset_us >= slot_start_us)
                                   ? 0
                                   : slot_start_us - cfg->timing_offset_us;
  const uint64_t actual_us = timestamp_from_ref(state, receive_time);
  int32_t delta = 0;
  if (actual_us >= expected_us)
    delta = (int32_t)(actual_us - expected_us);
  else
    delta = -(int32_t)(expected_us - actual_us);

  nfapi_msg_arrival_result_e result = NFAPI_MSG_ARRIVAL_ON_TIME;
  const int32_t window = (int32_t)cfg->timing_window_us;

  if (delta > window)
    result = NFAPI_MSG_ARRIVAL_TOO_LATE;
  else if (delta < -window)
    result = NFAPI_MSG_ARRIVAL_TOO_EARLY;

  update_stats(stats, delta, result);
  (void)transmit_timestamp;
  return result;
}

void nfapi_delay_mgmt_update_jitter(nfapi_delay_mgmt_state_t *state,
                                    nfapi_msg_type_e msg_type,
                                    uint32_t transmit_timestamp,
                                    struct timeval *receive_time)
{
  nfapi_jitter_state_t *jitter = get_jitter_state(state, msg_type);
  if (!jitter || !receive_time)
    return;

  const uint64_t arrival_us = timestamp_from_ref(state, receive_time);
  if (!state->time_reference_valid || arrival_us == 0)
    return;

  const uint32_t transit_time = (arrival_us >= transmit_timestamp)
                                    ? (uint32_t)(arrival_us - transmit_timestamp)
                                    : 0;

  if (!jitter->initialized) {
    jitter->previous_transit_time = transit_time;
    jitter->initialized = 1;
    return;
  }

  const int32_t d = (int32_t)transit_time - (int32_t)jitter->previous_transit_time;
  jitter->previous_transit_time = transit_time;
  jitter->jitter += ((d < 0 ? -d : d) - jitter->jitter) >> 4;
}

static uint32_t slots_since_last(const nfapi_delay_mgmt_state_t *state,
                                 uint16_t sfn,
                                 uint16_t slot)
{
  const uint16_t slots_per_frame = nfapi_delay_mgmt_get_slots_per_frame(state->subcarrier_spacing);
  const uint32_t current = (uint32_t)sfn * slots_per_frame + slot;
  const uint32_t previous = (uint32_t)state->last_sfn * slots_per_frame + state->last_slot;

  if (current >= previous)
    return current - previous;

  const uint32_t frame_slots = slots_per_frame * 1024U;
  return (current + frame_slots) - previous;
}

int nfapi_delay_mgmt_should_send_timing_info(nfapi_delay_mgmt_state_t *state,
                                             uint16_t sfn,
                                             uint16_t slot,
                                             int force_aperiodic)
{
  const uint32_t advanced_slots = slots_since_last(state, sfn, slot);
  state->last_sfn = sfn;
  state->last_slot = slot;
  state->slot_counter += advanced_slots;

  bool send = false;
  if ((state->timing_info_mode & 0x1) && state->timing_info_period > 0) {
    if (state->slot_counter >= state->timing_info_period) {
      send = true;
      state->slot_counter = 0;
    }
  }

  if (!send && (state->timing_info_mode & 0x2) && force_aperiodic)
    send = true;

  if (send)
    gettimeofday(&state->last_timing_info_time, NULL);

  return send ? 1 : 0;
}

void nfapi_delay_mgmt_build_timing_info(nfapi_delay_mgmt_state_t *state,
                                        nfapi_nr_timing_info_t *timing_info)
{
  if (!timing_info)
    return;

  memset(timing_info, 0, sizeof(*timing_info));
  timing_info->last_sfn = state->last_sfn;
  timing_info->last_slot = state->last_slot;

  struct timeval now;
  gettimeofday(&now, NULL);
  timing_info->time_since_last_timing_info = (uint32_t)timeval_diff_us(&now, &state->last_timing_info_time);

  timing_info->dl_tti_jitter = state->dl_tti_jitter.jitter;
  timing_info->tx_data_request_jitter = state->tx_data_jitter.jitter;
  timing_info->ul_tti_jitter = state->ul_tti_jitter.jitter;
  timing_info->ul_dci_jitter = state->ul_dci_jitter.jitter;

  timing_info->dl_tti_latest_delay = state->dl_tti_stats.latest_delay;
  timing_info->tx_data_request_latest_delay = state->tx_data_stats.latest_delay;
  timing_info->ul_tti_latest_delay = state->ul_tti_stats.latest_delay;
  timing_info->ul_dci_latest_delay = state->ul_dci_stats.latest_delay;

  timing_info->dl_tti_earliest_arrival = state->dl_tti_stats.earliest_arrival == INT32_MAX ? 0 : state->dl_tti_stats.earliest_arrival;
  timing_info->tx_data_request_earliest_arrival = state->tx_data_stats.earliest_arrival == INT32_MAX ? 0 : state->tx_data_stats.earliest_arrival;
  timing_info->ul_tti_earliest_arrival = state->ul_tti_stats.earliest_arrival == INT32_MAX ? 0 : state->ul_tti_stats.earliest_arrival;
  timing_info->ul_dci_earliest_arrival = state->ul_dci_stats.earliest_arrival == INT32_MAX ? 0 : state->ul_dci_stats.earliest_arrival;
}

void nfapi_delay_mgmt_process_dl_node_sync(nfapi_delay_mgmt_state_t *state,
                                           nfapi_nr_dl_node_sync_t *dl_sync,
                                           struct timeval *receive_time)
{
  if (!state || !dl_sync)
    return;

  state->t1 = dl_sync->t1;
  if (receive_time)
    state->t2 = (uint32_t)timeval_to_us(receive_time);
}

void nfapi_delay_mgmt_build_ul_node_sync(nfapi_delay_mgmt_state_t *state,
                                         nfapi_nr_ul_node_sync_t *ul_sync)
{
  if (!state || !ul_sync)
    return;

  struct timeval now;
  gettimeofday(&now, NULL);

  ul_sync->t1 = state->t1;
  ul_sync->t2 = state->t2;
  ul_sync->t3 = (uint32_t)timeval_to_us(&now);
  state->t3 = ul_sync->t3;
}

void nfapi_delay_mgmt_process_ul_node_sync(nfapi_delay_mgmt_state_t *state,
                                           nfapi_nr_ul_node_sync_t *ul_sync)
{
  if (!state || !ul_sync)
    return;

  state->t1 = ul_sync->t1;
  state->t2 = ul_sync->t2;
  state->t3 = ul_sync->t3;
}

uint64_t nfapi_delay_mgmt_calc_slot_time_us(uint16_t sfn, uint16_t slot, uint8_t subcarrier_spacing)
{
  return calc_slot_start_us(sfn, slot, subcarrier_spacing);
}

uint16_t nfapi_delay_mgmt_get_slots_per_frame(uint8_t subcarrier_spacing)
{
  const uint8_t scs = subcarrier_spacing > 4 ? 4 : subcarrier_spacing;
  return (uint16_t)(10U * (1U << scs));
}
