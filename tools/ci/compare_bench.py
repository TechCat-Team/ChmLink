#!/usr/bin/env python3
"""Gate chmlink-bench results against a recorded baseline.

    compare_bench.py RESULT.jsonl [RESULT.jsonl ...] --env environment.json
                     [--baseline tools/ci/bench-baseline.json]
                     [--markdown summary.md] [--require-baseline]
    compare_bench.py ... --update-baseline --commit SHA --driver 0.3.1.0

Several result files are repeats of the same run; every metric is gated on
its median, and error counters on their worst value. The baseline is chosen by
hardware profile (CPU model and logical processors), so a different machine
needs its own entry.

Exit status: 0 pass (or no baseline yet), 1 gate failed, 2 usage or data
error, 3 baseline required but missing.
"""
import argparse
import datetime
import json
import statistics
import sys

# metric -> (kind, default tolerance). Kinds:
#   higher  value may drop at most tol percent below the baseline
#   lower   value may rise at most tol percent above the baseline
#   drops   a loss percentage: allowed max(base * 1.5, base + tol)
#   zero    must be zero regardless of the baseline
#   info    recorded and reported, never gated
RULES = {
    "tx_pps": ("higher", 15.0),
    "rx_pps": ("higher", 15.0),
    "tx_drop_pct": ("drops", 0.5),
    "rx_loss_pct": ("drops", 0.1),
    "p50_us": ("lower", 25.0),
    "p99_us": ("lower", 40.0),
    "p999_us": ("info", 0.0),
    "max_us": ("info", 0.0),
    "lost": ("zero", 0.0),
    "errors": ("zero", 0.0),
    "loopback_pps": ("info", 0.0),
}
WORST = {"errors", "lost"}          # aggregated by max across repeats, not median
DRIFT_WARN = 0.25                   # loopback speed differs from the baseline by more


class DataError(Exception):
    pass


def scenario_name(record):
    if record["scenario"] == "loopback":
        return "loopback_%dB" % record["payload"]
    return record["scenario"]


def percent(part, whole):
    return 100.0 * part / whole if whole else 0.0


def metrics_of(record):
    """Reduce one result line to the metrics this tool gates."""
    out = {}
    if record["scenario"] == "latency":
        for key in ("p50_us", "p99_us", "p999_us", "max_us", "lost"):
            out[key] = float(record[key])
    elif record["scenario"] == "loopback":
        out["loopback_pps"] = float(record["received_pps"])
    else:
        errors = 0
        if "tx_delivered_pps" in record:
            drops = record["drv_tx_queue_drops"]
            out["tx_pps"] = float(record["tx_delivered_pps"])
            out["tx_drop_pct"] = percent(drops, record["drv_tx_packets"] + drops)
            errors += record.get("tx_consumer_error", 0)
        if "rx_received_pps" in record:
            out["rx_pps"] = float(record["rx_received_pps"])
            out["rx_loss_pct"] = float(record["rx_loss_pct"])
            errors += record.get("rx_inject_errors", 0) + record.get("rx_last_error", 0)
        out["errors"] = float(errors)
    return out


def load_results(paths):
    """{scenario: {metric: [one value per repeat]}}"""
    merged = {}
    for path in paths:
        seen = set()
        with open(path, encoding="utf-8-sig") as handle:
            for number, line in enumerate(handle, 1):
                line = line.strip()
                if not line.startswith("{"):
                    continue
                try:
                    record = json.loads(line)
                except ValueError as error:
                    raise DataError("%s:%d: %s" % (path, number, error))
                if record.get("event") != "result":
                    continue
                name = scenario_name(record)
                seen.add(name)
                for metric, value in metrics_of(record).items():
                    merged.setdefault(name, {}).setdefault(metric, []).append(value)
    if not merged:
        raise DataError("no benchmark results found")
    return merged


def aggregate(merged):
    out = {}
    for name, metrics in merged.items():
        out[name] = {metric: (max(values) if metric in WORST else statistics.median(values))
                     for metric, values in metrics.items()}
    return out


def profile_key(env):
    return "%s|%s" % (str(env.get("Cpu", "unknown")).strip(), env.get("Logical", "?"))


def number(value):
    return "%.0f" % value if abs(value) >= 100 else "%.3g" % value


def judge(metric, base, value, tolerances):
    """Returns (status, limit text). Status is PASS, FAIL or INFO."""
    kind, default = RULES[metric]
    tol = tolerances.get(metric, default)
    if kind == "info":
        return "INFO", ""
    if kind == "zero":
        return ("PASS" if value == 0 else "FAIL"), "== 0"
    if base is None:
        return "NEW", ""
    if kind == "higher":
        limit = base * (1 - tol / 100.0)
        return ("PASS" if value >= limit else "FAIL"), ">= " + number(limit)
    if kind == "lower":
        limit = base * (1 + tol / 100.0)
        return ("PASS" if value <= limit else "FAIL"), "<= " + number(limit)
    limit = max(base * 1.5, base + tol)
    return ("PASS" if value <= limit else "FAIL"), "<= " + number(limit)


def compare(current, baseline_profile, tolerances):
    rows, failed = [], False
    base_scenarios = baseline_profile.get("scenarios", {}) if baseline_profile else {}
    for name in sorted(current):
        for metric in sorted(current[name]):
            value = current[name][metric]
            base = base_scenarios.get(name, {}).get(metric)
            status, limit = judge(metric, base, value, tolerances) if baseline_profile else \
                (("FAIL" if (RULES[metric][0] == "zero" and value != 0) else "NEW"), "")
            if status == "FAIL":
                failed = True
            delta = "" if not base else "%+.1f%%" % (100.0 * (value - base) / base)
            rows.append((name, metric, base, value, delta, limit, status))
    return rows, failed


def drift(current, baseline_profile):
    """Ratio of this machine's loopback UDP speed to the baseline's, or None."""
    if not baseline_profile:
        return None
    ratios = []
    for name, metrics in current.items():
        base = baseline_profile.get("scenarios", {}).get(name, {}).get("loopback_pps")
        if name.startswith("loopback_") and base:
            ratios.append(metrics["loopback_pps"] / base)
    return statistics.median(ratios) if ratios else None


def fmt(value):
    if value is None:
        return "-"
    return "%.0f" % value if abs(value) >= 1000 else "%.3g" % value


def markdown(rows, key, repeats, ratio, failed, have_baseline):
    lines = ["## ChmLink benchmark: %s" % ("FAIL" if failed else "PASS"), "",
             "Profile: `%s`, %d repeat(s), median per metric." % (key, repeats)]
    if not have_baseline:
        lines.append("\nNo baseline for this profile: nothing was gated except error counters.")
    if ratio is not None and abs(ratio - 1) > DRIFT_WARN:
        lines.append("\n**Warning:** loopback UDP is %.0f%% of its baseline speed; this machine is "
                     "unusually %s, so throughput gates may mislead. Re-run before trusting a failure."
                     % (100 * ratio, "slow" if ratio < 1 else "fast"))
    lines += ["", "| scenario | metric | baseline | current | delta | limit | |", "|---|---|---:|---:|---:|---|---|"]
    for name, metric, base, value, delta, limit, status in rows:
        mark = {"FAIL": "**FAIL**", "PASS": "ok"}.get(status, status.lower())
        lines.append("| %s | %s | %s | %s | %s | %s | %s |" % (name, metric, fmt(base), fmt(value), delta, limit, mark))
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("results", nargs="+")
    parser.add_argument("--env", required=True, help="environment.json written by tools/bench.ps1")
    parser.add_argument("--baseline", default="tools/ci/bench-baseline.json")
    parser.add_argument("--markdown")
    parser.add_argument("--require-baseline", action="store_true")
    parser.add_argument("--update-baseline", action="store_true")
    parser.add_argument("--commit", default="")
    parser.add_argument("--driver", default="")
    args = parser.parse_args(argv)
    try:
        with open(args.env, "rb") as handle:
            raw = handle.read()
        try:
            env = json.loads(raw.decode("utf-8-sig"))
        except UnicodeDecodeError:   # written by an older bench.ps1 in the system code page (GBK here)
            env = json.loads(raw.decode("gb18030"))
        current = aggregate(load_results(args.results))
        try:
            with open(args.baseline, encoding="utf-8") as handle:
                store = json.load(handle)
        except FileNotFoundError:
            store = {"version": 1, "tolerances": {}, "profiles": {}}
    except (DataError, ValueError, OSError) as error:
        print("error: %s" % error, file=sys.stderr)
        return 2
    key = profile_key(env)
    if args.update_baseline:
        store.setdefault("profiles", {})[key] = {
            "recorded": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "commit": args.commit, "driver": args.driver, "repeats": len(args.results),
            "scenarios": {name: {m: round(v, 4) for m, v in metrics.items()} for name, metrics in current.items()},
        }
        with open(args.baseline, "w", encoding="utf-8") as handle:
            json.dump(store, handle, indent=2, sort_keys=True)
            handle.write("\n")
        print("Baseline for %s updated (%d scenarios)." % (key, len(current)))
        return 0
    profile = store.get("profiles", {}).get(key)
    if profile is None and args.require_baseline:
        print("error: no baseline for profile %s; run with --update-baseline after review." % key, file=sys.stderr)
        return 3
    rows, failed = compare(current, profile, store.get("tolerances", {}))
    report = markdown(rows, key, len(args.results), drift(current, profile), failed, profile is not None)
    if args.markdown:
        with open(args.markdown, "w", encoding="utf-8") as handle:
            handle.write(report)
    print(report)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
