"""
Offline baseline trainer.

Let the daemon run for a while (30+ minutes of normal usage), then run
this to train one Isolation Forest per process on everything collected
so far. Models are saved to ml/models.joblib so they can be reused or
inspected later.

    python3 ml/train.py --log logs/metrics.json
"""

import argparse
import json

import joblib
from sklearn.ensemble import IsolationForest

# reuse the exact same feature logic as the live detector, so a model
# trained here behaves identically there
from anomaly_detector import make_features, BASELINE_SIZE


def load_features(log_path):
    """Read the whole metrics log and group feature rows by process name."""
    by_process = {}
    prev_row = {}

    with open(log_path) as f:
        for line in f:
            try:
                row = json.loads(line)
            except json.JSONDecodeError:
                continue
            name = row["name"]
            by_process.setdefault(name, []).append(
                make_features(row, prev_row.get(name))
            )
            prev_row[name] = row

    return by_process


def main():
    parser = argparse.ArgumentParser(description="Train baseline models")
    parser.add_argument("--log", default="logs/metrics.json")
    parser.add_argument("--out", default="ml/models.joblib")
    args = parser.parse_args()

    by_process = load_features(args.log)

    models = {}
    skipped = 0
    for name, rows in by_process.items():
        # too few readings = the model would just memorize noise
        if len(rows) < BASELINE_SIZE:
            skipped += 1
            continue

        model = IsolationForest(
            n_estimators=100,
            contamination=0.05,
            random_state=42,
        )
        model.fit(rows)
        models[name] = model
        print(f"trained '{name}' on {len(rows)} readings")

    joblib.dump(models, args.out)
    print(f"\nsaved {len(models)} models to {args.out} "
          f"({skipped} processes skipped — under {BASELINE_SIZE} readings)")


if __name__ == "__main__":
    main()
