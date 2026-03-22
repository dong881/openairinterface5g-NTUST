#!/usr/bin/env python3
"""
CSI-RS Impact Analysis Tool
Analyzes how CSI-RS affects PDSCH throughput and identifies optimization opportunities
"""

import csv
import sys
import statistics
import matplotlib.pyplot as plt

def analyze_csirs_impact(timing_csv):
    """Analyze CSI-RS impact on performance"""

    # Separate slots with and without CSI-RS
    slots_with_csirs = []
    slots_without_csirs = []

    with open(timing_csv, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            # Only analyze PDSCH slots
            if int(row.get('pdsch_gen_ns', 0)) == 0:
                continue

            csirs_time = int(row.get('csirs_gen_ns', 0))
            slot_data = {
                'frame': int(row['frame']),
                'slot': int(row['slot']),
                'total_ns': int(row['total_ns']),
                'pdsch_ns': int(row['pdsch_gen_ns']),
                'csirs_ns': csirs_time,
                'memory_clear_ns': int(row['memory_clear_ns']),
                'phase_rot_ns': int(row['phase_rot_ns']),
                'encoding_ns': int(row.get('pdsch_encoding_ns', 0)),
                're_mapping_ns': int(row.get('pdsch_re_map_ns', 0))
            }

            if csirs_time > 0:
                slots_with_csirs.append(slot_data)
            else:
                slots_without_csirs.append(slot_data)

    print("=" * 80)
    print("CSI-RS IMPACT ANALYSIS")
    print("=" * 80)

    # Basic statistics
    print(f"\nSlots with CSI-RS: {len(slots_with_csirs)}")
    print(f"Slots without CSI-RS: {len(slots_without_csirs)}")

    if not slots_with_csirs:
        print("\n⚠️  No CSI-RS slots found!")
        return

    # Compare average processing times
    print("\n" + "-" * 80)
    print("PROCESSING TIME COMPARISON")
    print("-" * 80)
    print(f"{'Metric':<30} {'With CSI-RS':<15} {'Without CSI-RS':<15} {'Difference':<15}")
    print("-" * 80)

    metrics = ['total_ns', 'pdsch_ns', 'memory_clear_ns', 'phase_rot_ns',
               'encoding_ns', 're_mapping_ns']

    impact_summary = {}

    for metric in metrics:
        if slots_without_csirs:
            avg_with = statistics.mean([s[metric] for s in slots_with_csirs]) / 1000  # to us
            avg_without = statistics.mean([s[metric] for s in slots_without_csirs]) / 1000
            diff = avg_with - avg_without
            diff_pct = 100 * diff / avg_without if avg_without > 0 else 0

            name = metric.replace('_ns', '').replace('_', ' ').title()
            print(f"{name:<30} {avg_with:>12.2f} us {avg_without:>12.2f} us {diff:>+8.2f} us ({diff_pct:+.1f}%)")

            impact_summary[metric] = {
                'with_csirs': avg_with,
                'without_csirs': avg_without,
                'impact_us': diff,
                'impact_pct': diff_pct
            }

    # CSI-RS specific timing
    print("\n" + "-" * 80)
    print("CSI-RS TIMING ANALYSIS")
    print("-" * 80)

    csirs_times = [s['csirs_ns'] for s in slots_with_csirs]
    print(f"CSI-RS Generation Time:")
    print(f"  Min:     {min(csirs_times)/1000:>8.2f} us")
    print(f"  Average: {statistics.mean(csirs_times)/1000:>8.2f} us")
    print(f"  Max:     {max(csirs_times)/1000:>8.2f} us")
    print(f"  Std Dev: {statistics.stdev(csirs_times)/1000 if len(csirs_times) > 1 else 0:>8.2f} us")

    # Identify CSI-RS overhead
    print("\n" + "-" * 80)
    print("CSI-RS OVERHEAD ANALYSIS")
    print("-" * 80)

    if slots_without_csirs:
        # Direct overhead (CSI-RS generation time)
        avg_csirs_time = statistics.mean(csirs_times) / 1000
        avg_total_with = statistics.mean([s['total_ns'] for s in slots_with_csirs]) / 1000
        direct_overhead_pct = 100 * avg_csirs_time / avg_total_with

        print(f"Direct CSI-RS overhead: {avg_csirs_time:.2f} us ({direct_overhead_pct:.1f}% of slot time)")

        # Indirect overhead (increased PDSCH processing)
        pdsch_impact = impact_summary.get('pdsch_ns', {}).get('impact_us', 0)
        if pdsch_impact > 0:
            print(f"Indirect PDSCH impact: {pdsch_impact:.2f} us additional PDSCH processing")

        # Total overhead
        total_impact = impact_summary.get('total_ns', {}).get('impact_us', 0)
        total_impact_pct = impact_summary.get('total_ns', {}).get('impact_pct', 0)
        print(f"Total slot time impact: {total_impact:.2f} us ({total_impact_pct:.1f}% increase)")

    # Performance degradation analysis
    print("\n" + "-" * 80)
    print("PERFORMANCE DEGRADATION INDICATORS")
    print("-" * 80)

    # Check if slots with CSI-RS are more likely to exceed budget
    slot_budget_ns = 500000  # 500us for 30kHz SCS

    if slots_without_csirs:
        violations_with = sum(1 for s in slots_with_csirs if s['total_ns'] > slot_budget_ns)
        violations_without = sum(1 for s in slots_without_csirs if s['total_ns'] > slot_budget_ns)

        viol_rate_with = 100 * violations_with / len(slots_with_csirs)
        viol_rate_without = 100 * violations_without / len(slots_without_csirs)

        print(f"Slots exceeding 500us budget:")
        print(f"  With CSI-RS:    {violations_with:>4} / {len(slots_with_csirs):>4} ({viol_rate_with:.1f}%)")
        print(f"  Without CSI-RS: {violations_without:>4} / {len(slots_without_csirs):>4} ({viol_rate_without:.1f}%)")

        if viol_rate_with > viol_rate_without + 5:
            print("\n⚠️  CSI-RS significantly increases real-time violations!")

    # Identify bottlenecks specific to CSI-RS slots
    print("\n" + "-" * 80)
    print("BOTTLENECK ANALYSIS FOR CSI-RS SLOTS")
    print("-" * 80)

    # Find which operation is most impacted by CSI-RS
    max_impact = max(impact_summary.items(), key=lambda x: abs(x[1].get('impact_us', 0)))
    print(f"Most impacted function: {max_impact[0].replace('_ns', '').replace('_', ' ').title()}")
    print(f"  Impact: {max_impact[1]['impact_us']:.2f} us ({max_impact[1]['impact_pct']:.1f}%)")

    # Recommendations
    print("\n" + "=" * 80)
    print("OPTIMIZATION RECOMMENDATIONS")
    print("=" * 80)

    recommendations = []

    # Check direct CSI-RS overhead
    if direct_overhead_pct > 5:
        recommendations.append("• High CSI-RS generation overhead (>5%) - Consider reducing CSI-RS frequency")

    # Check PDSCH impact
    if impact_summary.get('pdsch_ns', {}).get('impact_pct', 0) > 10:
        recommendations.append("• Significant PDSCH processing increase with CSI-RS - Check RE allocation conflicts")

    # Check RE mapping impact
    if impact_summary.get('re_mapping_ns', {}).get('impact_pct', 0) > 20:
        recommendations.append("• RE mapping heavily impacted - CSI-RS may be fragmenting PDSCH allocation")

    # Check encoding impact
    if impact_summary.get('encoding_ns', {}).get('impact_pct', 0) > 15:
        recommendations.append("• Encoding time increased - Possibly due to reduced TBS requiring different code rates")

    # Check violation rate
    if slots_without_csirs and viol_rate_with > viol_rate_without * 1.5:
        recommendations.append("• CSI-RS causes frequent deadline misses - Consider less dense CSI-RS configuration")

    if recommendations:
        for rec in recommendations:
            print(rec)
    else:
        print("✓ CSI-RS overhead appears acceptable")

    # CSI-RS configuration suggestions
    print("\n" + "-" * 80)
    print("CSI-RS CONFIGURATION SUGGESTIONS")
    print("-" * 80)

    # Estimate CSI-RS frequency
    total_slots = len(slots_with_csirs) + len(slots_without_csirs)
    csirs_frequency = len(slots_with_csirs) / total_slots if total_slots > 0 else 0

    print(f"Current CSI-RS frequency: {csirs_frequency*100:.1f}% of PDSCH slots")

    if csirs_frequency > 0.2:
        print("→ Consider increasing CSI-RS period (currently >20% of slots)")

    if avg_csirs_time > 10:
        print("→ Consider reducing CSI-RS density or number of ports")

    if impact_summary.get('total_ns', {}).get('impact_us', 0) > 50:
        print("→ Consider using ZP-CSI-RS instead of NZP-CSI-RS for some measurements")

    print("\n" + "=" * 80)

if __name__ == '__main__':
    csv_file = sys.argv[1] if len(sys.argv) > 1 else '/home/kelvin/openairinterface5g/l1_downlink_timing.csv'

    try:
        analyze_csirs_impact(csv_file)
    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)