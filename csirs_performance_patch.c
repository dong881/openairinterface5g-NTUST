// CSI-RS Performance Measurement Additions
// Add these measurements to identify CSI-RS impact on throughput

// 1. MEASUREMENT STRUCTURE (add to phy_procedures_nr_gNB.c)
typedef struct {
    // Timing measurements
    long csirs_generation_ns;
    long csirs_re_mapping_ns;

    // Resource measurements
    int csirs_symbols_per_slot;    // Number of OFDM symbols with CSI-RS
    int csirs_res_per_symbol;       // Number of REs used for CSI-RS per symbol
    int csirs_total_res;            // Total REs used for CSI-RS in slot

    // PDSCH impact
    int pdsch_res_available;        // REs available for PDSCH after CSI-RS
    int pdsch_res_lost;             // REs lost to CSI-RS
    float pdsch_throughput_impact;  // Percentage of throughput lost

    // Scheduling conflicts
    int scheduling_conflicts;       // Times PDSCH and CSI-RS collided
    int symbols_blocked;            // OFDM symbols blocked by CSI-RS

    // CSI-RS configuration
    int csirs_period;              // CSI-RS periodicity in slots
    int csirs_density;             // Frequency density
    int csirs_ports;               // Number of CSI-RS ports
    int csirs_type;                // NZP-CSI-RS or ZP-CSI-RS

} csirs_impact_metrics_t;

// 2. KEY MEASUREMENTS TO ADD

// A. In nr_generate_csi_rs() - measure RE consumption:
void measure_csirs_re_usage(nfapi_nr_dl_tti_csi_rs_pdu_rel15_t *csi_params) {
    // Calculate number of REs used by CSI-RS
    int symbols = get_num_symbols(csi_params->symb_l0, csi_params->symb_l1);
    int res_per_rb = get_res_per_rb(csi_params->row, csi_params->freq_density);
    int total_res = symbols * res_per_rb * csi_params->nr_of_rbs;

    // Log to measurement file
    fprintf(csirs_log, "%d,%d,%d,%d\n", frame, slot, symbols, total_res);
}

// B. In PDSCH scheduling - measure available REs after CSI-RS:
int calculate_pdsch_res_with_csirs(int slot) {
    int total_res = 12 * 14 * num_rbs;  // Total REs in slot
    int csirs_res = get_csirs_res_in_slot(slot);
    int dmrs_res = get_dmrs_res_in_slot(slot);
    int pdcch_res = get_pdcch_res_in_slot(slot);

    int available = total_res - csirs_res - dmrs_res - pdcch_res;
    return available;
}

// C. In MAC scheduler - detect CSI-RS/PDSCH conflicts:
void detect_scheduling_conflicts(int slot) {
    // Check if PDSCH and CSI-RS are scheduled on same symbols
    uint16_t pdsch_symbol_mask = get_pdsch_symbols(slot);
    uint16_t csirs_symbol_mask = get_csirs_symbols(slot);

    if (pdsch_symbol_mask & csirs_symbol_mask) {
        conflicts++;
        blocked_symbols = __builtin_popcount(pdsch_symbol_mask & csirs_symbol_mask);
    }
}

// 3. THROUGHPUT IMPACT CALCULATION
float calculate_throughput_impact() {
    // Theoretical max throughput without CSI-RS
    int max_tbs_no_csirs = calculate_tbs(total_res, mcs, layers);

    // Actual throughput with CSI-RS
    int actual_tbs = calculate_tbs(total_res - csirs_res, mcs, layers);

    // Percentage impact
    float impact = 100.0 * (max_tbs_no_csirs - actual_tbs) / max_tbs_no_csirs;
    return impact;
}

// 4. CSV OUTPUT FORMAT for CSI-RS analysis
// csirs_impact.csv:
// frame,slot,csirs_symbols,csirs_res,pdsch_res_available,pdsch_res_lost,throughput_loss_%,conflicts

// 5. ADDITIONAL MEASUREMENTS TO CONSIDER:

// A. CSI-RS periodicity impact
void measure_csirs_periodicity_impact() {
    // Measure how often CSI-RS occurs and its pattern
    static int csirs_pattern[100];  // Track CSI-RS occurrence pattern
    csirs_pattern[slot % 100] = has_csirs(slot) ? 1 : 0;

    // Analyze pattern for optimization opportunities
    if (slot % 100 == 99) {
        analyze_pattern(csirs_pattern);
    }
}

// B. UE feedback delay impact
void measure_csi_feedback_latency() {
    // Measure time between CSI-RS transmission and CSI report reception
    struct timespec csirs_tx_time, csi_report_rx_time;
    long feedback_latency_ns = timespec_diff(&csirs_tx_time, &csi_report_rx_time);

    // Track if stale CSI is causing suboptimal MCS selection
    if (feedback_latency_ns > threshold) {
        stale_csi_count++;
    }
}

// C. Beamforming efficiency
void measure_beamforming_gain() {
    // Compare throughput with/without CSI-RS based beamforming
    int tput_with_csirs_beam = get_current_throughput();
    int tput_without_beam = get_default_beam_throughput();

    float beam_gain = (float)tput_with_csirs_beam / tput_without_beam;

    // If gain < 1.0, CSI-RS overhead exceeds beamforming benefit
    if (beam_gain < 1.0) {
        LOG_W(MAC, "CSI-RS overhead exceeds beamforming gain: %.2f\n", beam_gain);
    }
}

// 6. CONFIGURATION OPTIMIZATION SUGGESTIONS:

// A. Dynamic CSI-RS adaptation
void optimize_csirs_config() {
    // Reduce CSI-RS frequency if channel is stable
    if (channel_variation < threshold) {
        increase_csirs_period();  // Less frequent CSI-RS
    }

    // Reduce CSI-RS density if full bandwidth not needed
    if (!need_full_bandwidth_csi) {
        reduce_freq_density();  // Fewer REs per RB
    }

    // Use ZP-CSI-RS instead of NZP if possible
    if (can_use_zp_csirs()) {
        switch_to_zp_csirs();  // Zero power = no interference
    }
}

// 7. KEY METRICS TO MONITOR:

typedef struct {
    // Performance indicators
    float avg_throughput_mbps;
    float throughput_loss_percent;
    int avg_mcs;
    float bler;

    // Resource utilization
    float re_utilization_percent;
    float csirs_overhead_percent;

    // Scheduling efficiency
    int scheduling_conflicts_per_second;
    float avg_tb_size;

    // CSI quality
    float csi_report_accuracy;
    int stale_csi_reports;

} csirs_kpi_t;