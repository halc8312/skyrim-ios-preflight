"""Summarize a completed build-5 phone measurement. Times are host wall times."""
import argparse
from collections import Counter
import json
from pathlib import Path
import re
from statistics import median


APIS = ("legacy_async", "metal4_async")
MODES = ("idle", "contended")
MAX_BYTES = 2 * 1024 * 1024


def require(ok, message):
    if not ok:
        raise ValueError(message)


def natural(data, key):
    number = data[key]
    require(type(number) is int and number >= 0, f"Invalid nonnegative integer {key}")
    return number


def milliseconds(values):
    if not values:
        return None
    return round(median(values) / 1_000_000, 6)


def read_record(path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, f"Duplicate JSON key {key}")
            result[key] = value
        return result

    with path.open("rb") as stream:
        raw = stream.read(MAX_BYTES + 1)
    require(len(raw) <= MAX_BYTES, "Record exceeds the phone's 2 MiB storage limit")
    return json.loads(raw, object_pairs_hook=unique)


def observe(trial, source_ids, options_id):
    jobs = trial["jobs"]
    require(len(jobs) == 3 and [j["role_index"] for j in jobs] == [0, 1, 2],
            "Expected two prefetch requests followed by the urgent request")
    for job in jobs:
        source = job["source_sha256"]
        require(isinstance(source, str) and re.fullmatch("[a-f0-9]{64}", source)
                and source not in source_ids, "Missing or reused source fingerprint")
        source_ids.add(source)
        require(job["status"] == "passed" and job["oracle_verified"] is True
                and job["options_sha256"] == options_id, "Unverified job or inconsistent options")
        entered = natural(job, "api_enter_ns")
        require(natural(job, "enqueued_ns") <= entered <= natural(job, "callback_enter_ns")
                and entered <= natural(job, "api_return_ns"), "Inconsistent request timestamps")

    urgent = jobs[2]
    start, end = urgent["api_enter_ns"], urgent["callback_enter_ns"]
    prefetch = jobs[:2]
    require(all(j["api_return_ns"] <= start for j in prefetch), "Prefetch API launches out of order")
    # Both prefetches have entered before the urgent request. Their overlapping
    # portions start at the same point, so the later callback determines the union.
    pending = sum(j["callback_enter_ns"] > start for j in prefetch)
    overlap = max(0, min(end, max(j["callback_enter_ns"] for j in prefetch)) - start)
    queue_wait = start - urgent["enqueued_ns"]
    require(natural(trial, "prefetch_inflight_at_urgent_api_enter") == pending
            and natural(trial, "urgent_prefetch_overlap_ns") == overlap
            and trial["host_overlap_observed"] is (overlap > 0), "Recorded overlap disagrees with timestamps")
    require(natural(trial, "urgent_request_to_callback_ns") == end - start
            and natural(trial, "urgent_queue_delay_ns") == queue_wait
            and natural(trial, "urgent_enqueue_to_callback_ns") == end - start + queue_wait,
            "Recorded urgent wait disagrees with timestamps")
    require(trial["mode"] != "idle" or pending == 0, "Idle control submitted before prefetch completed")
    return end - start, queue_wait, overlap


def summarize(record):
    require(record["status"] == "passed" and record["finished"] is True
            and record["recovered_incomplete"] is False, "A completed phone export is required")
    comparison = record["comparison"]
    require(comparison["schema"] == 5 and comparison["status"] == "passed"
            and comparison["contention_control_verified"] is True, "Expected a completed build-5 comparison")
    control = comparison["contention_control"]
    require(control["schema"] == 2 and control["status"] == "passed"
            and control["physical_iphone_verified"] is True and len(control["trials"]) == 32,
            "Expected a completed physical 32-trial first-use comparison")
    panel = control["panel_namespace"]
    require(type(control["metal4_first"]) is bool and record["metal4_first"] is control["metal4_first"]
            and comparison["metal4_first_requested"] is control["metal4_first"], "Inconsistent starting API")
    first_group = 41 if control["metal4_first"] else 9
    require(panel == f"contention_v2_groups_{first_group}_{first_group + 31}"
            and natural(control, "source_group_first") == first_group
            and natural(control, "source_group_last") == first_group + 31,
            "Unexpected source group range")
    options_id = control["options_sha256"]
    require(re.fullmatch("[a-f0-9]{64}", options_id), "Missing common compile options fingerprint")

    sources, groups = set(), set()
    costs = {}
    positions, lanes = Counter(), Counter()
    for index, trial in enumerate(control["trials"]):
        block, position = divmod(index, 4)
        api, mode = trial["api"], trial["mode"]
        condition = 2 * APIS.index(api) + MODES.index(mode)
        expected = ((0, 1, 3, 2)[position] + block) % 4
        if control["metal4_first"]:
            expected ^= 2
        lane = natural(trial, "lane_index")
        group = natural(trial, "source_group")
        require(trial["status"] == "passed" and trial["panel_namespace"] == panel
                and natural(trial, "block_index") == block and natural(trial, "position_index") == position
                and natural(trial, "condition_index") == condition == expected
                and trial["condition_id"] == f"{api.removesuffix('_async')}_{mode}"
                and trial["first_assigned_in_run"] is True
                and trial["planned_overlap"] is (mode == "contended")
                and trial["actual_overlap_evaluated"] is True, "Unexpected trial order or assignment")
        require(lane == (condition + block // 2) % 4 and group == first_group + 4 * block + lane
                and group not in groups,
                "Missing or repeated source group")
        require(trial["prefetch_qos"] == trial["urgent_qos"] == "user_initiated",
                "First-use comparison requires a single submission priority")
        require((block, api, mode) not in costs, "Repeated condition in one block")
        groups.add(group)
        positions[condition, position] += 1
        lanes[condition, lane] += 1
        costs[block, api, mode] = observe(trial, sources, options_id)
    require(len(positions) == len(lanes) == 16 and set(positions.values()) == set(lanes.values()) == {2}
            and len(sources) == 96, "Unbalanced positions, lanes or source identities")

    cells, contrasts = [], {}
    for mode in MODES:
        for api in APIS:
            rows = [costs[block, api, mode] for block in range(8)]
            overlapped = [wait for wait, _, overlap in rows if overlap > 0]
            cells.append({"api": api, "planned_mode": mode, "scheduled": len(rows),
                          "observed_overlap": len(overlapped),
                          "median_urgent_api_to_callback_ms": milliseconds([r[0] for r in rows]),
                          "max_urgent_api_to_callback_ms": round(max(r[0] for r in rows) / 1_000_000, 6),
                          "median_urgent_queue_wait_ms": milliseconds([r[1] for r in rows]),
                          "median_urgent_ms_when_overlap_observed": milliseconds(overlapped)})
        # Each block includes one of each condition on distinct source groups.
        differences = [costs[b, APIS[1], mode][0] - costs[b, APIS[0], mode][0] for b in range(8)]
        contrasts[mode] = {"blocks": len(differences),
                           "median_within_block_metal4_minus_legacy_ms": milliseconds(differences),
                           "blocks_with_shorter_metal4_wait": sum(d < 0 for d in differences)}
    return {"panel": panel, "normal_source_count": len(sources), "cells": cells,
            "descriptive_contrasts": contrasts,
            "scope": "All scheduled trials contribute to the primary median. Each block uses distinct sources. "
                     "Host overlap, OS cache state and Skyrim frame time are different observations."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("record", type=Path)
    args = parser.parse_args()
    try:
        summary = summarize(read_record(args.record))
    except (OSError, KeyError, TypeError, ValueError) as exc:
        parser.exit(1, f"Could not summarize first-use result: {exc}\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
