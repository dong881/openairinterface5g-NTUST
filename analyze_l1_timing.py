#!/usr/bin/env python3
"""L1 Downlink Timing Analysis - Enhanced Statistics"""

import csv
import sys
import statistics

def analyze_timing(csv_file):
    """Analyze L1 downlink timing with RB/MCS correlation"""

    data = {
        'num_rbs': [], 'mcs': [],
        'memory_clear_ns': [], 'prs_gen_ns': [], 'ssb_gen_ns': [],
        'pdcch_gen_ns': [], 'pdsch_gen_ns': [], 'csirs_gen_ns': [],
        'phase_rot_ns': [], 'total_ns': [],
        'pdsch_encoding_ns': [], 'pdsch_scrambling_ns': [],
        'pdsch_modulation_ns': [], 'pdsch_layer_map_ns': [],
        'pdsch_precoding_ns': [], 'pdsch_re_map_ns': []
    }

    pdsch_slots = 0
    total_slots = 0

    # Read CSV - only PDSCH slots
    with open(csv_file, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            total_slots += 1
            if int(row.get('pdsch_gen_ns', 0)) == 0:
                continue

            pdsch_slots += 1
            for key in data.keys():
                if key in row:
                    data[key].append(int(row[key]))

    if pdsch_slots == 0:
        print("⚠️  No PDSCH slots found")
        return

    # === HEADER ===
    print("="*80)
    print(f"L1 DOWNLINK TIMING ANALYSIS: {pdsch_slots}/{total_slots} PDSCH slots")
    print("="*80)

    # === CORE TIMING ===
    print(f"\n{'Function':<20} {'Min (us)':<12} {'Avg (us)':<12} {'Max (us)':<12} {'StdDev (us)':<12} {'%Total':<8}")
    print("-"*80)

    avg_total = statistics.mean(data['total_ns'])

    for key in ['memory_clear_ns', 'ssb_gen_ns', 'pdcch_gen_ns',
                'pdsch_gen_ns', 'csirs_gen_ns', 'phase_rot_ns', 'total_ns']:
        if not data[key]:
            continue

        vals = data[key]
        min_us = min(vals) / 1000
        avg_us = statistics.mean(vals) / 1000
        max_us = max(vals) / 1000
        std_us = statistics.stdev(vals) / 1000 if len(vals) > 1 else 0
        pct = 100 * statistics.mean(vals) / avg_total if key != 'total_ns' else 100.0

        name = key.replace('_ns', '').replace('_', ' ').title()
        print(f"{name:<20} {min_us:>10.2f}   {avg_us:>10.2f}   {max_us:>10.2f}   {std_us:>10.2f}   {pct:>6.1f}%")

    # === PDSCH BREAKDOWN ===
    print(f"\n{'PDSCH Stages':<20} {'Min (us)':<12} {'Avg (us)':<12} {'Max (us)':<12} {'StdDev (us)':<12} {'%PDSCH':<8}")
    print("-"*80)

    avg_pdsch = statistics.mean(data['pdsch_gen_ns'])

    for key in ['pdsch_encoding_ns', 'pdsch_scrambling_ns', 'pdsch_modulation_ns',
                'pdsch_layer_map_ns', 'pdsch_precoding_ns', 'pdsch_re_map_ns']:
        if not data[key]:
            continue

        vals = [v for v in data[key] if v > 0]
        if not vals:
            continue

        min_us = min(vals) / 1000
        avg_us = statistics.mean(vals) / 1000
        max_us = max(vals) / 1000
        std_us = statistics.stdev(vals) / 1000 if len(vals) > 1 else 0
        pct = 100 * statistics.mean(vals) / avg_pdsch

        name = key.replace('pdsch_', '').replace('_ns', '').replace('_', ' ').title()
        print(f"  {name:<18} {min_us:>10.2f}   {avg_us:>10.2f}   {max_us:>10.2f}   {std_us:>10.2f}   {pct:>6.1f}%")

    # === RB STATISTICS ===
    print("\n" + "="*80)
    print("RESOURCE BLOCK ALLOCATION ANALYSIS")
    print("="*80)

    if data['num_rbs']:
        rb_values = [v for v in data['num_rbs'] if v > 0]
        if rb_values:
            min_rbs = min(rb_values)
            avg_rbs = statistics.mean(rb_values)
            max_rbs = max(rb_values)
            std_rbs = statistics.stdev(rb_values) if len(rb_values) > 1 else 0

            print(f"RB Count:  Min={min_rbs:>4}  Avg={avg_rbs:>6.1f}  Max={max_rbs:>4}  StdDev={std_rbs:>8.2f}")

            # RB distribution
            rb_counts = {}
            for rb in rb_values:
                rb_counts[rb] = rb_counts.get(rb, 0) + 1

            print(f"\nTop 5 RB Allocations:")
            for rb, count in sorted(rb_counts.items(), key=lambda x: x[1], reverse=True)[:5]:
                pct = 100 * count / len(rb_values)
                print(f"  {rb:>3} RBs: {count:>5} slots ({pct:>5.1f}%)")

            # Time per RB
            if len(data['num_rbs']) == len(data['pdsch_gen_ns']):
                time_per_rb = [t/rb for t, rb in zip(data['pdsch_gen_ns'], data['num_rbs']) if rb > 0]
                if time_per_rb:
                    avg_time_per_rb = statistics.mean(time_per_rb) / 1000
                    print(f"\nAvg PDSCH time per RB: {avg_time_per_rb:.3f} us/RB")

    # === MCS STATISTICS ===
    print("\n" + "="*80)
    print("MODULATION AND CODING SCHEME (MCS) ANALYSIS")
    print("="*80)

    if data['mcs']:
        mcs_values = [v for v in data['mcs'] if v >= 0]
        if mcs_values:
            min_mcs = min(mcs_values)
            avg_mcs = statistics.mean(mcs_values)
            max_mcs = max(mcs_values)
            std_mcs = statistics.stdev(mcs_values) if len(mcs_values) > 1 else 0

            print(f"MCS Index: Min={min_mcs:>4}  Avg={avg_mcs:>6.1f}  Max={max_mcs:>4}  StdDev={std_mcs:>8.2f}")

            # MCS distribution
            mcs_counts = {}
            for mcs in mcs_values:
                mcs_counts[mcs] = mcs_counts.get(mcs, 0) + 1

            print(f"\nTop 5 MCS Values:")
            for mcs, count in sorted(mcs_counts.items(), key=lambda x: x[1], reverse=True)[:5]:
                pct = 100 * count / len(mcs_values)
                # Determine modulation
                if mcs <= 9:
                    mod = "QPSK"
                elif mcs <= 16:
                    mod = "16QAM"
                elif mcs <= 28:
                    mod = "64QAM"
                else:
                    mod = "256QAM"
                print(f"  MCS {mcs:>2} ({mod:>6}): {count:>5} slots ({pct:>5.1f}%)")

    # === RB vs TIMING CORRELATION ===
    if data['num_rbs'] and len(data['num_rbs']) == len(data['pdsch_gen_ns']):
        print("\n" + "="*80)
        print("RB ALLOCATION vs PROCESSING TIME")
        print("="*80)

        # Group by RB ranges
        rb_timing = {}
        for rb, pdsch_time, total_time in zip(data['num_rbs'], data['pdsch_gen_ns'], data['total_ns']):
            if rb == 0:
                continue
            rb_bucket = (rb // 50) * 50  # Bucket by 50 RBs
            if rb_bucket not in rb_timing:
                rb_timing[rb_bucket] = {'pdsch': [], 'total': []}
            rb_timing[rb_bucket]['pdsch'].append(pdsch_time)
            rb_timing[rb_bucket]['total'].append(total_time)

        if rb_timing:
            print(f"{'RB Range':<15} {'Count':<8} {'Avg PDSCH (us)':<18} {'Avg Total (us)':<18}")
            print("-"*80)
            for rb_bucket in sorted(rb_timing.keys()):
                count = len(rb_timing[rb_bucket]['pdsch'])
                avg_pdsch = statistics.mean(rb_timing[rb_bucket]['pdsch']) / 1000
                avg_total = statistics.mean(rb_timing[rb_bucket]['total']) / 1000
                rb_range = f"{rb_bucket}-{rb_bucket+49}"
                print(f"{rb_range:<15} {count:<8} {avg_pdsch:<18.2f} {avg_total:<18.2f}")

    # === MCS vs TIMING CORRELATION ===
    if data['mcs'] and len(data['mcs']) == len(data['pdsch_gen_ns']):
        print("\n" + "="*80)
        print("MCS vs PROCESSING TIME")
        print("="*80)

        # Group by modulation type
        mcs_timing = {}
        for mcs, pdsch_time, total_time in zip(data['mcs'], data['pdsch_gen_ns'], data['total_ns']):
            if mcs < 0:
                continue
            # Group by modulation
            if mcs <= 9:
                mod_group = "QPSK (0-9)"
            elif mcs <= 16:
                mod_group = "16QAM (10-16)"
            elif mcs <= 28:
                mod_group = "64QAM (17-28)"
            else:
                mod_group = "256QAM (29+)"

            if mod_group not in mcs_timing:
                mcs_timing[mod_group] = {'pdsch': [], 'total': [], 'mcs_values': []}
            mcs_timing[mod_group]['pdsch'].append(pdsch_time)
            mcs_timing[mod_group]['total'].append(total_time)
            mcs_timing[mod_group]['mcs_values'].append(mcs)

        if mcs_timing:
            print(f"{'Modulation':<20} {'Count':<8} {'Avg MCS':<10} {'Avg PDSCH (us)':<18} {'Avg Total (us)':<18}")
            print("-"*80)
            for mod_group in ["QPSK (0-9)", "16QAM (10-16)", "64QAM (17-28)", "256QAM (29+)"]:
                if mod_group in mcs_timing:
                    count = len(mcs_timing[mod_group]['pdsch'])
                    avg_mcs = statistics.mean(mcs_timing[mod_group]['mcs_values'])
                    avg_pdsch = statistics.mean(mcs_timing[mod_group]['pdsch']) / 1000
                    avg_total = statistics.mean(mcs_timing[mod_group]['total']) / 1000
                    print(f"{mod_group:<20} {count:<8} {avg_mcs:<10.1f} {avg_pdsch:<18.2f} {avg_total:<18.2f}")

    # === BOTTLENECK ANALYSIS ===
    print("\n" + "="*80)
    print("BOTTLENECK ANALYSIS")
    print("="*80)

    slot_budget = 500000  # 500us for 30kHz SCS
    avg_total_us = statistics.mean(data['total_ns']) / 1000

    if statistics.mean(data['total_ns']) > slot_budget:
        overrun = avg_total_us - 500
        print(f"⚠️  OVER BUDGET: {avg_total_us:.1f}us avg (500us target, +{overrun:.1f}us over)")
    else:
        margin = 100 * (1 - statistics.mean(data['total_ns']) / slot_budget)
        print(f"✓ WITHIN BUDGET: {avg_total_us:.1f}us avg ({margin:.1f}% margin)")

    # Slow slots
    slow_slots = [v for v in data['total_ns'] if v > slot_budget]
    if slow_slots:
        print(f"\n⚠️  {len(slow_slots)} slots exceeded 500us budget:")
        print(f"   Slowest slot: {max(data['total_ns'])/1000:.2f}us")
        print(f"   Percentage: {100*len(slow_slots)/pdsch_slots:.1f}%")

    # Stage bottleneck
    stage_times = {
        'Memory Clear': statistics.mean(data['memory_clear_ns']),
        'SSB': statistics.mean(data['ssb_gen_ns']) if data['ssb_gen_ns'] else 0,
        'PDCCH': statistics.mean(data['pdcch_gen_ns']),
        'PDSCH': statistics.mean(data['pdsch_gen_ns']),
        'CSI-RS': statistics.mean(data['csirs_gen_ns']) if data['csirs_gen_ns'] else 0,
        'Phase Rot': statistics.mean(data['phase_rot_ns'])
    }

    bottleneck = max(stage_times, key=stage_times.get)
    bottleneck_time = stage_times[bottleneck] / 1000
    bottleneck_pct = 100 * stage_times[bottleneck] / statistics.mean(data['total_ns'])

    print(f"\n📊 Overall Bottleneck: {bottleneck} ({bottleneck_time:.1f}us, {bottleneck_pct:.1f}%)")

    # PDSCH bottleneck - handle cases where some stages may be all zeros (e.g., precoding with PMI=0 fast path)
    def safe_mean(values):
        filtered = [v for v in values if v > 0]
        return statistics.mean(filtered) if filtered else 0

    pdsch_stages = {
        'Encoding': safe_mean(data['pdsch_encoding_ns']),
        'Scrambling': safe_mean(data['pdsch_scrambling_ns']),
        'Modulation': safe_mean(data['pdsch_modulation_ns']),
        'Layer Map': safe_mean(data['pdsch_layer_map_ns']),
        'Precoding': safe_mean(data['pdsch_precoding_ns']),
        'RE Mapping': safe_mean(data['pdsch_re_map_ns'])
    }

    # Filter out zero stages for bottleneck analysis
    active_stages = {k: v for k, v in pdsch_stages.items() if v > 0}
    if active_stages:
        pdsch_bottleneck = max(active_stages, key=active_stages.get)
        pdsch_bottleneck_time = active_stages[pdsch_bottleneck] / 1000
        pdsch_bottleneck_pct = 100 * active_stages[pdsch_bottleneck] / statistics.mean(data['pdsch_gen_ns'])
        print(f"   PDSCH Bottleneck: {pdsch_bottleneck} ({pdsch_bottleneck_time:.1f}us, {pdsch_bottleneck_pct:.1f}%)")

    # Show if precoding is skipped (PMI=0 fast path active)
    precoding_values = [v for v in data['pdsch_precoding_ns'] if v > 0]
    if not precoding_values:
        print(f"   ✓ Precoding: SKIPPED (PMI=0 fast path active for all slots)")

    print("="*80)

if __name__ == '__main__':
    csv_file = 'l1_downlink_timing.csv'
    if len(sys.argv) > 1:
        csv_file = sys.argv[1]

    try:
        analyze_timing(csv_file)
    except FileNotFoundError:
        print(f"Error: '{csv_file}' not found")
        sys.exit(1)
    except Exception as e:
        print(f"Error: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
