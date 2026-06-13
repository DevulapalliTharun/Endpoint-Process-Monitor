"""
Anomaly detector for endpoint-monitor.

The C++ daemon writes one JSON line per process every 5 seconds into
logs/metrics.json. This script tails that file, learns what "normal"
looks like for each process (Isolation Forest), and writes an alert
whenever a process starts behaving unlike its own baseline.

Run it in a second terminal, next to the daemon:

    python3 ml/anomaly_detector.py --log logs/metrics.json --alerts logs/alerts.log
"""

import argparse
import json
import time

from sklearn.ensemble import IsolationForest

# How many readings of a process we collect before training its model.
# At one reading every 5 seconds, 100 readings ~= 8 minutes of baseline.
BASELINE_SIZE = 100

# Only alert if the anomaly score is below this threshold.
# Scores range from ~0 (barely anomalous) to -0.5 (very anomalous).
# -0.05 cuts out the noise — only genuinely unusual behavior gets flagged.
SCORE_THRESHOLD = -0.05

# kernel threads and interrupt handlers — not useful to monitor,
# they show up as noise. Any process whose name starts with one of
# these prefixes is silently skipped.
KERNEL_PREFIXES = (
    "irq/", "kworker/", "ksoftirqd/", "migration/", "idle_inject/",
    "cpuhp/", "kthread", "rcu_", "UVM ", "nvidia-drm/", "card2-",
    "nvidia-modeset/", "vidmem", "pool_workqueue",
)

# user-space processes that are permanently noisy on this machine
# (their anomaly scores are always borderline due to variable behavior)
IGNORE_PROCESSES = {"dbus-daemon", "dbus"}


def make_features(row, prev):
    """Turn one JSON reading into the numeric list the model sees.

    cpu_delta / mem_delta capture *sudden change*, which matters as much
    as the absolute value — jumping from 5% to 60% CPU is suspicious
    even if 60% on its own might be fine for some processes.
    """
    cpu_delta = row["cpu_percent"] - prev["cpu_percent"] if prev else 0.0
    mem_delta = row["memory_rss_mb"] - prev["memory_rss_mb"] if prev else 0.0
    return [
        row["cpu_percent"],
        row["memory_rss_mb"],
        row["disk_read_bytes"],
        row["disk_write_bytes"],
        row["open_connections"],
        cpu_delta,
        mem_delta,
    ]


def follow(path):
    """Yield lines from the file forever, like `tail -f`.

    Starts from the beginning, so data collected before we launched
    still counts toward the baseline. When there's nothing new, wait
    a bit and try again.
    """
    while True:
        try:
            f = open(path)
            break
        except FileNotFoundError:
            print(f"waiting for {path} to appear (is the daemon running?)")
            time.sleep(3)

    with f:
        while True:
            line = f.readline()
            if line:
                yield line
            else:
                time.sleep(2)  # daemon hasn't written anything new yet


def write_alert(alerts_path, row, score):
    """Append one alert line and echo it to the console.

    The score comes from decision_function(): more negative = the model
    is more confident this reading is an outlier.
    """
    line = (
        f"[ALERT] {row['timestamp']} | {row['name']} (pid {row['pid']}) | "
        f"cpu: {row['cpu_percent']:.1f}% | mem: {row['memory_rss_mb']:.0f} MB | "
        f"conns: {row['open_connections']} | score: {score:.3f}\n"
    )
    with open(alerts_path, "a") as f:
        f.write(line)
    print(line, end="")


def main():
    parser = argparse.ArgumentParser(description="Isolation Forest anomaly detector")
    parser.add_argument("--log", default="logs/metrics.json")
    parser.add_argument("--alerts", default="logs/alerts.log")
    args = parser.parse_args()

    baselines = {}  # process name -> feature rows collected so far
    models = {}     # process name -> its trained IsolationForest
    prev_row = {}   # process name -> previous reading (for the deltas)

    print(f"reading {args.log}, alerts go to {args.alerts}")
    print(f"each process needs {BASELINE_SIZE} readings before detection starts\n")

    for line in follow(args.log):
        try:
            row = json.loads(line)
        except json.JSONDecodeError:
            continue  # half-written line, the daemon is mid-write — skip

        name = row["name"]

        # skip kernel threads and permanently noisy system processes
        if any(name.startswith(p) for p in KERNEL_PREFIXES):
            continue
        if name in IGNORE_PROCESSES:
            continue

        features = make_features(row, prev_row.get(name))
        prev_row[name] = row

        if name in models:
            # score first — decision_function gives a continuous value,
            # more negative = more anomalous. We use the score directly
            # rather than predict() so we can apply our own threshold.
            score = models[name].decision_function([features])[0]
            if score < SCORE_THRESHOLD:
                write_alert(args.alerts, row, score)
        else:
            # Still collecting this process's baseline.
            baselines.setdefault(name, []).append(features)
            if len(baselines[name]) >= BASELINE_SIZE:
                model = IsolationForest(
                    n_estimators=100,
                    contamination=0.01,  # only 1% of baseline treated as anomalous
                    random_state=42,
                )
                model.fit(baselines[name])
                models[name] = model
                del baselines[name]  # raw rows aren't needed anymore
                print(f"baseline trained for '{name}' "
                      f"({BASELINE_SIZE} readings, watching it now)")


if __name__ == "__main__":
    main()
