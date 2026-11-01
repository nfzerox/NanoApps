#!/usr/bin/env python3
"""Decode one 512-byte NanoApps boot-reporter read without touching a device."""

from __future__ import annotations

import json
from pathlib import Path
import struct
import sys

REPORT_MAGIC = 0x32505452
PHASES = {
    0: "publisher_not_entered",
    1: "publisher_entered",
    2: "c6_reporter_ready",
    3: "deferred_loader_armed",
}
REASONS = {
    0: "none",
    1: "c6_route_mismatch",
    2: "atomic_claim_unavailable",
    3: "action_slot_not_stock",
    4: "action_hook_publish_failed",
}
MODES = {
    0x10: "User", 0x11: "FIQ", 0x12: "IRQ", 0x13: "Supervisor",
    0x17: "Abort", 0x1B: "Undefined", 0x1F: "System",
}
SELECTOR_SLOTS = {0: "not_recorded", 1: "A", 2: "B", 3: "resource_legacy", 4: "disk"}
SELECTOR_RESULTS = {
    0: "not_recorded",
    1: "disk_resident_absent_or_invalid",
    2: "resident_hooks_invalid",
    3: "resident_hooks_valid",
    4: "waiting_for_disk",
}
ACTION_KINDS = {1: "automatic_main", 2: "settings_fallback"}
LAUNCH_RESULTS = {
    1: "executable_load_failed",
    2: "new_executable_started",
    3: "retained_executable_reused",
    4: "new_executable_entry_failed",
    5: "retained_executable_entry_failed",
}


def main() -> None:
    if len(sys.argv) not in (2, 4) or (len(sys.argv) == 4 and
                                      sys.argv[2] != "--preferred-slot"):
        raise SystemExit(
            f"usage: {Path(sys.argv[0]).name} C6_READ.bin "
            "[--preferred-slot A|B]")
    preferred_slot = None
    if len(sys.argv) == 4:
        preferred_slot = sys.argv[3].upper()
        if preferred_slot not in ("A", "B"):
            raise SystemExit("--preferred-slot must be A or B")
    data = Path(sys.argv[1]).read_bytes()
    if data == b"\x02\x00\x00\x00":
        print(json.dumps({"reader": "stock", "result": "reporter_unavailable"}, indent=2))
        return
    if len(data) < 0x74:
        raise SystemExit(f"short C6 response: {len(data)} bytes")
    magic, started, cpsr, phase, reason = struct.unpack_from("<5I", data, 0x2C)
    if magic != REPORT_MAGIC:
        raise SystemExit(f"unknown report magic: {magic:#010x}")
    (resident, registered, app_count, preloaded, fail_mask, launched,
     app_id, app_kind, entry) = struct.unpack_from("<9I", data, 0x50)
    mode = cpsr & 0x1F
    result = {
        "reader": "nanoapps_candidate",
        "publisher_started": bool(started),
        "cpsr": f"0x{cpsr:08x}",
        "cpu_mode": MODES.get(mode, f"unknown_{mode:#x}"),
        "publisher_phase_code": phase,
        "publisher_phase": PHASES.get(phase, "unknown"),
        "publisher_reason_code": reason,
        "publisher_reason": REASONS.get(reason, "unknown"),
        "resident_loaded": bool(resident),
        "apps_registered": bool(registered),
        "app_count": app_count,
        "executables_preloaded": preloaded,
        "preload_failure_mask": f"0x{fail_mask:08x}",
        "first_app_launched": bool(launched),
        "first_app_id": f"0x{app_id:08x}" if launched else None,
        "first_app_kind": app_kind if launched else None,
        "first_app_entry": f"0x{entry:08x}" if launched else None,
    }
    if len(data) >= 0x15C:
        states = {0: "not_applied", 1: "applied", 2: "already_applied"}
        bt, dock = struct.unpack_from("<2I", data, 0x154)
        result["bluetooth_fix"] = states.get(bt, "unknown")
        result["dock_fix"] = states.get(dock, "unknown")
    if len(data) >= 0x84:
        selected, selector_result, resident_base, resident_span = \
            struct.unpack_from("<4I", data, 0x74)
        result.update({
            "selector_slot_code": selected,
            "selector_slot": SELECTOR_SLOTS.get(selected, "unknown"),
            "selector_result_code": selector_result,
            "selector_result": SELECTOR_RESULTS.get(selector_result, "unknown"),
            "resident_base": f"0x{resident_base:08x}" if resident_base else None,
            "resident_span": resident_span,
        })
        if preferred_slot:
            actual_slot = SELECTOR_SLOTS.get(selected, "unknown")
            result.update({
                "preferred_slot": preferred_slot,
                "fallback_to_a": preferred_slot == "B" and actual_slot == "A",
            })
    if len(data) >= 0x94:
        history, count, last_action, ready_mask = struct.unpack_from(
            "<4I", data, 0x84)
        retained = min(count, 16)
        actions = []
        encoded = history
        for _ in range(retained):
            actions.append(ACTION_KINDS.get(encoded & 3, "unknown"))
            encoded >>= 2
        actions.reverse()
        readiness = {
            "home_action_recognised": bool(ready_mask & 1),
            "heap_ready": bool(ready_mask & 2),
            "disabled_marker_absent": bool(ready_mask & 4),
            "arena_allocated": bool(ready_mask & 8),
            "loader_file_ready": bool(ready_mask & 16),
        }
        result.update({
            "home_actions_seen": count,
            "home_action_history_retained": actions,
            "last_home_action_code": last_action,
            "last_home_action": ACTION_KINDS.get(last_action, "none"),
            "last_home_readiness_mask": f"0x{ready_mask:08x}",
            "last_home_readiness": readiness,
            "automatic_load_completed": bool(
                last_action == 1 and resident and selector_result == 3),
            "settings_fallback_completed": bool(
                last_action == 2 and resident and selector_result == 3),
        })
    if len(data) >= 0xA0:
        unknown_hash, unknown_length, unknown_count = struct.unpack_from(
            "<3I", data, 0x94)
        result.update({
            "unrecognised_action_count": unknown_count,
            "last_unrecognised_action_fnv1a32": (
                f"0x{unknown_hash:08x}" if unknown_count else None),
            "last_unrecognised_action_length": (
                unknown_length if unknown_count else None),
        })
    if len(data) >= 0x14C:
        boot_free, boot_largest, launch_count = struct.unpack_from(
            "<3I", data, 0xA0)
        launches = []
        for index in range(8):
            sequence, app_id, status, free_heap, largest = struct.unpack_from(
                "<5I", data, 0xAC + index * 20)
            if sequence and sequence <= launch_count:
                launches.append({
                    "sequence": sequence,
                    "app_id": f"0x{app_id:08x}",
                    "result_code": status,
                    "result": LAUNCH_RESULTS.get(status, "unknown"),
                    "free_heap": free_heap,
                    "largest_free_block": largest,
                })
        launches.sort(key=lambda record: record["sequence"])
        result.update({
            "free_heap_after_boot": boot_free,
            "largest_free_block_after_boot": boot_largest,
            "app_launch_count": launch_count,
            "app_launch_history_retained": launches,
        })
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
