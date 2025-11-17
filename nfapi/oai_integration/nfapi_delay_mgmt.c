#include "nfapi_delay_mgmt.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>
#include "debug.h"

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
  // Return absolute wallclock microseconds for jitter calculation and Node Sync
  // This must match VNF's transmit_timestamp calculation which uses absolute time
  if (!recv_time)
    return 0;
  return (uint64_t)((uint64_t)recv_time->tv_sec * 1000000ULL + (uint64_t)recv_time->tv_usec);
}

static uint64_t timestamp_relative_to_ref(const nfapi_delay_mgmt_state_t *state, const struct timeval *recv_time)
{
  // CRITICAL FIX: Return time relative to sfn_slot_zero_time reference for timing window checks
  // This ensures timing window calculations work correctly:
  // - slot_start_us is calculated as relative time from SFN/slot 0/0
  // - arrival_us must also be relative time from same reference point
  // - Both use the same time base for consistent window boundary checks
  if (!recv_time)
    return 0;
  
  if (!state || !state->time_reference_valid) {
    // No time reference yet - cannot calculate relative time
    // Return 0 to avoid invalid timing window checks during initialization
    NFAPI_TRACE(NFAPI_TRACE_WARN,
                "[DELAY-MGMT] Time reference not valid - cannot calculate relative timestamp");
    return 0;
  }
  
  // Return microseconds since sfn_slot_zero_time reference
  // This makes arrival_us directly comparable to slot_start_us from calc_slot_start_us()
  int64_t relative_us = timeval_diff_us(recv_time, &state->sfn_slot_zero_time);
  
  // Sanity check: relative time should be positive and reasonable (< 1 hour)
  if (relative_us < 0 || relative_us > 3600000000LL) {
    NFAPI_TRACE(NFAPI_TRACE_WARN,
                "[DELAY-MGMT] Invalid relative timestamp %lld µs - clock may have changed",
                (long long)relative_us);
    return 0;
  }
  
  return (uint64_t)relative_us;
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
                                         struct timeval *ref_time,
                                         uint16_t current_sfn,
                                         uint16_t current_slot)
{
  if (!ref_time || !state)
    return;

  // CRITICAL FIX: Calculate when SFN/Slot 0/0 would have occurred
  // This ensures slot_start_us calculations align with the time reference
  //
  // slot_start_us(sfn, slot) = time_from_sfn_0 in microseconds
  // For this to match arrival_us (time since reference), the reference must be
  // backdated to when SFN=0/Slot=0 actually occurred
  
  // Calculate how much time has elapsed since SFN/Slot 0/0
  uint64_t current_slot_time_us = calc_slot_start_us(current_sfn, current_slot, state->subcarrier_spacing);
  
  // Backdate the reference time to when SFN/Slot 0/0 occurred
  // sfn_slot_zero_time = current_time - elapsed_time_since_sfn_0
  int64_t seconds_offset = (int64_t)(current_slot_time_us / 1000000ULL);
  int64_t usec_offset = (int64_t)(current_slot_time_us % 1000000ULL);
  
  state->sfn_slot_zero_time.tv_sec = ref_time->tv_sec - seconds_offset;
  state->sfn_slot_zero_time.tv_usec = ref_time->tv_usec - usec_offset;
  
  // Handle underflow in microseconds
  if (state->sfn_slot_zero_time.tv_usec < 0) {
    state->sfn_slot_zero_time.tv_sec--;
    state->sfn_slot_zero_time.tv_usec += 1000000;
  }
  
  state->time_reference_valid = 1;
  state->last_timing_info_time = *ref_time;
  
  NFAPI_TRACE(NFAPI_TRACE_INFO,
              "[DELAY-MGMT] Set time reference at SFN.Slot=%u.%u (backdated %lld µs to SFN/Slot 0/0)",
              current_sfn,
              current_slot,
              (long long)current_slot_time_us);
}

uint32_t nfapi_delay_mgmt_get_transmit_timestamp(nfapi_delay_mgmt_state_t *state)
{
  // CRITICAL FIX: Return absolute wallclock microseconds for consistency with VNF
  // Previously returned relative time from state->time_reference, which was incompatible
  // with VNF's transmit_timestamp calculation
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
    struct timeval *receive_time,
    int32_t *delta_out)
{
  nfapi_timing_window_config_t *cfg = get_window_config(state, msg_type);
  nfapi_message_stats_t *stats = get_stats(state, msg_type);
  if (!cfg || !stats || !cfg->enabled || !receive_time)
    return NFAPI_MSG_ARRIVAL_ON_TIME;

  // CRITICAL FIX: Per SCF-222 Section 2.6, check message arrival against timing window
  // The timing window is defined relative to the target slot start time:
  //   window_start = slot_start - timing_offset
  //   window_end = window_start - timing_window
  // A message is on-time if it arrives within [window_end, window_start]
  
  // Calculate target slot start time in microseconds (relative to SFN/slot 0/0)
  const uint64_t slot_start_us = calc_slot_start_us(sfn, slot, state->subcarrier_spacing);
  
  // Calculate timing window boundaries
  // window_start: timing_offset microseconds before slot start
  // window_end: timing_window microseconds before window_start
  const uint64_t window_start_us = slot_start_us - cfg->timing_offset_us;
  const uint64_t window_end_us = window_start_us - cfg->timing_window_us;
  
  // Get actual arrival time (relative to same SFN/slot 0/0 reference)
  const uint64_t arrival_us = timestamp_relative_to_ref(state, receive_time);
  
  // If time reference is not valid, skip timing window check
  if (arrival_us == 0) {
    return NFAPI_MSG_ARRIVAL_ON_TIME;
  }
  
  // Calculate delta from ideal arrival point (window_start)
  // Positive delta = message arrived after window_start (late)
  // Negative delta = message arrived before window_start (early, but may still be on-time if within window)
  int64_t delta_signed = (int64_t)arrival_us - (int64_t)window_start_us;
  
  // Clamp delta to int32_t range for reporting (should never overflow in practice)
  int32_t delta = (delta_signed > INT32_MAX) ? INT32_MAX : 
                  (delta_signed < INT32_MIN) ? INT32_MIN : (int32_t)delta_signed;
  
  // Determine if message arrival is on-time, too early, or too late
  nfapi_msg_arrival_result_e result;
  if (arrival_us < window_end_us) {
    // Arrived before window opened (too early)
    result = NFAPI_MSG_ARRIVAL_TOO_EARLY;
  } else if (arrival_us > window_start_us) {
    // Arrived after window closed (too late)
    result = NFAPI_MSG_ARRIVAL_TOO_LATE;
  } else {
    // Arrived within timing window (on-time)
    result = NFAPI_MSG_ARRIVAL_ON_TIME;
  }

  update_stats(stats, delta, result);
  if (delta_out)
    *delta_out = delta;
  
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

  // CRITICAL FIX: Handle 32-bit wraparound for transmit_timestamp
  // Both VNF and PNF use gettimeofday() which returns 64-bit microseconds since epoch,
  // but transmit_timestamp is 32-bit and wraps every ~71 minutes (2^32 microseconds).
  // We need to compute the transit time using 32-bit arithmetic to handle wraparound correctly.
  const uint64_t arrival_us = timestamp_from_ref(state, receive_time);
  
  if (arrival_us == 0 || transmit_timestamp == 0)
    return;

  // Calculate transit time using 32-bit wraparound arithmetic
  // Cast arrival_us to 32-bit (keeping only lower 32 bits) to match transmit_timestamp
  const uint32_t arrival_us_32 = (uint32_t)(arrival_us & 0xFFFFFFFFULL);
  
  // Calculate difference handling wraparound: if arrival < transmit, it wrapped
  // Use unsigned subtraction which naturally handles wraparound in 32-bit space
  uint32_t transit_time = arrival_us_32 - transmit_timestamp;
  
  // Sanity check: transit time should be < 1 second for reasonable fronthaul
  // Large values (> 2^31) indicate wraparound in the wrong direction or clock skew
  if (transit_time > 1000000U) {  // > 1 second
    NFAPI_TRACE(NFAPI_TRACE_WARN,
                "[DELAY-MGMT] Invalid transit time %u µs (arrival=%u, transmit=%u) - possible clock skew",
                transit_time,
                arrival_us_32,
                transmit_timestamp);
    return;
  }

  if (!jitter->initialized) {
    jitter->previous_transit_time = transit_time;
    jitter->initialized = 1;
    return;
  }

  // RFC 3550 jitter calculation: J = J + (|D| - J) / 16
  // where D = (R_i - R_{i-1}) - (S_i - S_{i-1}) = transit_time_i - transit_time_{i-1}
  const int32_t d = (int32_t)transit_time - (int32_t)jitter->previous_transit_time;
  jitter->previous_transit_time = transit_time;
  
  // CRITICAL FIX: Prevent underflow in jitter calculation
  // When |d| < jitter, the subtraction (|d| - jitter) would underflow in uint32_t arithmetic
  // Use signed arithmetic to handle this correctly
  const int32_t abs_d = (d < 0) ? -d : d;
  const int32_t jitter_delta = (abs_d - (int32_t)jitter->jitter) >> 4;
  const int32_t new_jitter = (int32_t)jitter->jitter + jitter_delta;
  
  // Clamp to valid range [0, reasonable max]
  // Jitter > 100ms indicates serious network issues or measurement error
  if (new_jitter < 0) {
    jitter->jitter = 0;
  } else if (new_jitter > 100000) {  // 100ms max
    jitter->jitter = 100000;
  } else {
    jitter->jitter = (uint32_t)new_jitter;
  }
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

  // Report jitter as 0 if not yet initialized (avoid reporting uninitialized data)
  timing_info->dl_tti_jitter = state->dl_tti_jitter.initialized ? state->dl_tti_jitter.jitter : 0;
  timing_info->tx_data_request_jitter = state->tx_data_jitter.initialized ? state->tx_data_jitter.jitter : 0;
  timing_info->ul_tti_jitter = state->ul_tti_jitter.initialized ? state->ul_tti_jitter.jitter : 0;
  timing_info->ul_dci_jitter = state->ul_dci_jitter.initialized ? state->ul_dci_jitter.jitter : 0;

  // Report latest_delay as 0 if no messages received yet (INT32_MIN sentinel value)
  timing_info->dl_tti_latest_delay = state->dl_tti_stats.latest_delay == INT32_MIN ? 0 : state->dl_tti_stats.latest_delay;
  timing_info->tx_data_request_latest_delay = state->tx_data_stats.latest_delay == INT32_MIN ? 0 : state->tx_data_stats.latest_delay;
  timing_info->ul_tti_latest_delay = state->ul_tti_stats.latest_delay == INT32_MIN ? 0 : state->ul_tti_stats.latest_delay;
  timing_info->ul_dci_latest_delay = state->ul_dci_stats.latest_delay == INT32_MIN ? 0 : state->ul_dci_stats.latest_delay;

  timing_info->dl_tti_earliest_arrival = state->dl_tti_stats.earliest_arrival == INT32_MAX ? 0 : state->dl_tti_stats.earliest_arrival;
  timing_info->tx_data_request_earliest_arrival = state->tx_data_stats.earliest_arrival == INT32_MAX ? 0 : state->tx_data_stats.earliest_arrival;
  timing_info->ul_tti_earliest_arrival = state->ul_tti_stats.earliest_arrival == INT32_MAX ? 0 : state->ul_tti_stats.earliest_arrival;
  timing_info->ul_dci_earliest_arrival = state->ul_dci_stats.earliest_arrival == INT32_MAX ? 0 : state->ul_dci_stats.earliest_arrival;

  // Increment timing info counter for tracking synchronization progress
  state->timing_info_count++;

  // CRITICAL FIX: Reset stats after building timing info to prevent stale values
  // Per SCF-222 spec, stats should be cleared after each timing info report
  reset_stats(&state->dl_tti_stats);
  reset_stats(&state->tx_data_stats);
  reset_stats(&state->ul_tti_stats);
  reset_stats(&state->ul_dci_stats);
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
