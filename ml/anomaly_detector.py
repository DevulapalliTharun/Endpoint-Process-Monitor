"""
Anomaly detector for endpoint-monitor.

The C++ daemon writes one JSON line per process every 5 seconds into
logs/metrics.json. This script tails that file, learns what "normal"
looks like for each process (Isolation Forest), and writes an alert
whenever a process starts behaving unlike its own baseline.

Run it in a second terminal alongside the daemon:

    .venv/bin/python ml/anomaly_detector.py --log logs/metrics.json --alerts logs/alerts.log
"""

import argparse
import json
import time

from sklearn.ensemble import IsolationForest

# How many readings of a process we collect before training its model.
# At one reading every 5 seconds, 100 readings = ~8 minutes of baseline.
# We need enough data so the model learns the "normal" range, not just noise.
BASELINE_SIZE = 100

# Only alert if the anomaly score is below this threshold.
# decision_function() gives scores: close to 0 = borderline, negative = anomalous.
# -0.05 cuts out noise — only genuinely unusual behavior gets flagged.
# Without this, the model raises alerts on tiny fluctuations too.
SCORE_THRESHOLD = -0.05

# Kernel threads look like processes but they're OS internals, not real apps.
# They always show "weird" metrics which would flood the detector with false alerts.
# We skip them entirely by checking if the process name starts with these prefixes.
KERNEL_PREFIXES = (
    "irq/", "kworker/", "ksoftirqd/", "migration/", "idle_inject/",
    "cpuhp/", "kthread", "rcu_", "UVM ", "nvidia-drm/", "card2-",
    "nvidia-modeset/", "vidmem", "pool_workqueue",
)

# Some user-space processes are permanently noisy on this machine
# (their behavior changes so randomly that any model flags them constantly).
IGNORE_PROCESSES = {"dbus-daemon", "dbus"}


def make_features(row, prev):
    """Turn one JSON reading into the numeric list the ML model sees.

    We use 7 numbers to describe a process at one point in time:
      - cpu_percent, memory_rss_mb, disk_read_bytes, disk_write_bytes,
        open_connections: the raw values at this moment
      - cpu_delta, mem_delta: how much these changed since the last reading

    Why deltas? A jump from 5% to 60% CPU is suspicious even if 60% alone
    might be normal for some processes. The delta captures sudden change,
    which is often the real signal of anomalous behavior.
    """
    cpu_delta = row["cpu_percent"]   - prev["cpu_percent"]   if prev else 0.0
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
    """Yield new lines from the file forever, like `tail -f` in a terminal.

    Starts from the beginning of the file, so data collected before we
    launched still counts toward the baseline. When there's nothing new,
    we wait a bit and try again — this gives us live streaming from a file.
    """
    # wait for the file to appear (the daemon might not have started yet)
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
                yield line      # new data — send it to whoever called follow()
            else:
                time.sleep(2)   # no new data yet — wait and try again


def write_alert(alerts_path, row, score):
    """Append one alert line to the alerts file and print it to the console.

    The score comes from decision_function():
      - close to 0  = barely anomalous (filtered out by SCORE_THRESHOLD)
      - around -0.5 = the model is very confident this is an outlier
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
    parser.add_argument("--log",    default="logs/metrics.json")
    parser.add_argument("--alerts", default="logs/alerts.log")
    args = parser.parse_args()

    baselines = {}  # process name -> list of feature rows collected before model is ready
    models    = {}  # process name -> its trained IsolationForest (after BASELINE_SIZE readings)
    prev_row  = {}  # process name -> previous reading (needed to compute cpu_delta, mem_delta)

    print(f"reading {args.log}, alerts go to {args.alerts}")
    print(f"each process needs {BASELINE_SIZE} readings before detection starts\n")

    for line in follow(args.log):
        # parse the JSON line that the C++ daemon wrote
        try:
            row = json.loads(line)
        except json.JSONDecodeError:
            continue  # daemon was mid-write when we read — skip this incomplete line

        name = row["name"]

        # skip kernel threads — they are OS internals, not real user applications
        if any(name.startswith(p) for p in KERNEL_PREFIXES):
            continue
        if name in IGNORE_PROCESSES:
            continue

        # build the 7-number feature vector for this reading
        features = make_features(row, prev_row.get(name))
        prev_row[name] = row  # save for next reading's delta calculation

        if name in models:
            # Model is trained — score this reading.
            # We use decision_function() instead of predict() because it gives
            # a continuous score, letting us tune sensitivity with SCORE_THRESHOLD.
            # predict() only gives -1 or +1, which is too coarse.
            score = models[name].decision_function([features])[0]
            if score < SCORE_THRESHOLD:
                write_alert(args.alerts, row, score)
        else:
            # Still collecting baseline data for this process.
            # We wait for BASELINE_SIZE readings before training —
            # fewer readings would give a model that just memorizes noise.
            baselines.setdefault(name, []).append(features)

            if len(baselines[name]) >= BASELINE_SIZE:
                # enough data — train the Isolation Forest now
                model = IsolationForest(
                    n_estimators=100,   # number of trees — more = more stable decisions
                    contamination=0.01, # assume only 1% of baseline was already anomalous
                    random_state=42,    # fixed seed so results are reproducible
                )
                model.fit(baselines[name])
                models[name] = model
                del baselines[name]  # raw rows no longer needed — free the memory
                print(f"baseline trained for '{name}' "
                      f"({BASELINE_SIZE} readings, watching it now)")


if __name__ == "__main__":
    main()
