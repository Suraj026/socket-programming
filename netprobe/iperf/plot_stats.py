import csv
import matplotlib.pyplot as plt
import sys
import os

csv_file = sys.argv[1] if len(sys.argv) > 1 else "throughput_stats.csv"

if not os.path.exists(csv_file):
    print(f"File not found: {csv_file}")
    sys.exit(1)

times, throughputs, delays = [], [], []

with open(csv_file) as f:
    reader = csv.DictReader(f)
    for row in reader:
        times.append(int(row["time_s"]))
        throughputs.append(float(row["throughput_mbps"]))
        delays.append(float(row["avg_delay_ms"]))

fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(10, 7), sharex=True)
fig.suptitle("UDP Echo Throughput Measurement", fontsize=14, fontweight="bold")

# Throughput
ax1.plot(times, throughputs, color="steelblue", linewidth=2, marker="o", markersize=4)
ax1.fill_between(times, throughputs, alpha=0.15, color="steelblue")
ax1.set_ylabel("Throughput (Mbps)", fontsize=11)
ax1.set_title("Throughput vs Time")
ax1.grid(True, linestyle="--", alpha=0.5)
ax1.set_ylim(bottom=0)

# Delay
ax2.plot(times, delays, color="tomato", linewidth=2, marker="s", markersize=4)
ax2.fill_between(times, delays, alpha=0.15, color="tomato")
ax2.set_ylabel("Avg Delay (ms)", fontsize=11)
ax2.set_xlabel("Time (s)", fontsize=11)
ax2.set_title("Average Round-Trip Delay vs Time")
ax2.grid(True, linestyle="--", alpha=0.5)
ax2.set_ylim(bottom=0)

plt.tight_layout()
out_file = "throughput_plot.png"
plt.savefig(out_file, dpi=150)
print(f"Plot saved to {out_file}")
plt.show()