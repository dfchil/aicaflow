#!/usr/bin/env python3
import argparse
import collections
import json
import os
from typing import Any, Dict, List, Optional


GM_INSTRUMENTS = {
    0: "Acoustic Grand Piano", 1: "Bright Acoustic Piano", 2: "Electric Grand Piano", 3: "Honky-tonk Piano",
    4: "Electric Piano 1", 5: "Electric Piano 2", 6: "Harpsichord", 7: "Clavinet", 8: "Celesta",
    9: "Glockenspiel", 10: "Music Box", 11: "Vibraphone", 12: "Marimba", 13: "Xylophone",
    14: "Tubular Bells", 15: "Dulcimer", 16: "Drawbar Organ", 17: "Percussive Organ", 18: "Rock Organ",
    19: "Church Organ", 20: "Reed Organ", 21: "Accordion", 22: "Harmonica", 23: "Tango Accordion",
    24: "Acoustic Guitar (nylon)", 25: "Acoustic Guitar (steel)", 26: "Electric Guitar (jazz)",
    27: "Electric Guitar (clean)", 28: "Electric Guitar (muted)", 29: "Overdriven Guitar",
    30: "Distortion Guitar", 31: "Guitar harmonics", 32: "Acoustic Bass", 33: "Electric Bass (finger)",
    34: "Electric Bass (pick)", 35: "Fretless Bass", 36: "Slap Bass 1", 37: "Slap Bass 2",
    38: "Synth Bass 1", 39: "Synth Bass 2", 40: "Violin", 41: "Viola", 42: "Cello", 43: "Contrabass",
    44: "Tremolo Strings", 45: "Pizzicato Strings", 46: "Orchestral Harp", 47: "Timpani",
    48: "String Ensemble 1", 49: "String Ensemble 2", 50: "Synth Strings 1", 51: "Synth Strings 2",
    52: "Choir Aahs", 53: "Voice Oohs", 54: "Synth Voice", 55: "Orchestra Hit", 56: "Trumpet",
    57: "Trombone", 58: "Tuba", 59: "Muted Trumpet", 60: "French Horn", 61: "Brass Section",
    62: "Synth Brass 1", 63: "Synth Brass 2", 64: "Soprano Sax", 65: "Alto Sax", 66: "Tenor Sax",
    67: "Baritone Sax", 68: "Oboe", 69: "English Horn", 70: "Bassoon", 71: "Clarinet", 72: "Piccolo",
    73: "Flute", 74: "Recorder", 75: "Pan Flute", 76: "Blown Bottle", 77: "Shakuhachi", 78: "Whistle",
    79: "Ocarina", 80: "Lead 1 (square)", 81: "Lead 2 (sawtooth)", 82: "Lead 3 (calliope)",
    83: "Lead 4 (chiff)", 84: "Lead 5 (charang)", 85: "Lead 6 (voice)", 86: "Lead 7 (fifths)",
    87: "Lead 8 (bass + lead)", 88: "Pad 1 (new age)", 89: "Pad 2 (warm)", 90: "Pad 3 (polysynth)",
    91: "Pad 4 (choir)", 92: "Pad 5 (bowed)", 93: "Pad 6 (metallic)", 94: "Pad 7 (halo)",
    95: "Pad 8 (sweep)", 96: "FX 1 (rain)", 97: "FX 2 (soundtrack)", 98: "FX 3 (crystal)",
    99: "FX 4 (atmosphere)", 100: "FX 5 (brightness)", 101: "FX 6 (goblins)", 102: "FX 7 (echoes)",
    103: "FX 8 (sci-fi)", 104: "Sitar", 105: "Banjo", 106: "Shamisen", 107: "Koto", 108: "Kalimba",
    109: "Bag pipe", 110: "Fiddle", 111: "Shanai", 112: "Tinkle Bell", 113: "Agogo", 114: "Steel Drums",
    115: "Woodblock", 116: "Taiko Drum", 117: "Melodic Tom", 118: "Synth Drum", 119: "Reverse Cymbal",
    120: "Guitar Fret Noise", 121: "Breath Noise", 122: "Seashore", 123: "Bird Tweet",
    124: "Telephone Ring", 125: "Helicopter", 126: "Applause", 127: "Gunshot"
}


def load_json(path: str):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def flatten_ranges(ranges: List[List[int]]) -> List[int]:
    programs: List[int] = []
    for pair in ranges:
        if len(pair) != 2:
            continue
        start, end = pair
        if start > end:
            start, end = end, start
        for p in range(max(0, start), min(127, end) + 1):
            programs.append(p)
    return programs


def build_program_family_map(profile: Dict) -> Dict[int, str]:
    out: Dict[int, str] = {}
    for fam in profile.get("families", []):
        name = fam.get("name")
        if not name:
            continue
        for p in flatten_ranges(fam.get("program_ranges", [])):
            out[p] = name
    return out


def family_keywords(profile: Dict) -> Dict[str, List[str]]:
    out: Dict[str, List[str]] = {}
    for fam in profile.get("families", []):
        name = fam.get("name")
        if not name:
            continue
        out[name] = [str(k).lower() for k in fam.get("keywords", [])]
    return out


def family_playback_policies(profile: Dict) -> Dict[str, Dict[str, float]]:
    out: Dict[str, Dict[str, float]] = {}
    for fam in profile.get("families", []):
        name = fam.get("name")
        if not name:
            continue
        pol = fam.get("playback_policy", {})
        out[name] = {
            "note_trim_ms": int(pol.get("note_trim_ms", 0)),
            "min_hold_ms": int(pol.get("min_hold_ms", 16)),
            "velocity_gamma": float(pol.get("velocity_gamma", 1.0)),
            "velocity_gain": float(pol.get("velocity_gain", 1.0)),
            "release_bias": int(pol.get("release_bias", 0)),
        }
    return out


def score_entry(entry: Dict, target_program: int, target_family: Optional[str], fam_keywords: Dict[str, List[str]], override_keywords: List[str]) -> int:
    score = 0
    gm_idx = entry.get("gm_idx")
    hay = " ".join([
        str(entry.get("filename", "")),
        str(entry.get("rel_path", "")),
        str(entry.get("source", "")),
        str(entry.get("gm_name", "")),
    ]).lower()

    if gm_idx == target_program:
        score += 200
    elif isinstance(gm_idx, int):
        if abs(gm_idx - target_program) <= 2:
            score += 40
        if (gm_idx // 8) == (target_program // 8):
            score += 25

    if target_family and target_family in fam_keywords:
        for kw in fam_keywords[target_family]:
            if kw and kw in hay:
                score += 12

    for kw in override_keywords:
        if kw and kw in hay:
            score += 18

    if "one shot" in hay or "oneshot" in hay:
        score -= 5
    if "loop" in hay:
        score += 3

    return score


def choose_entry(entries: List[Dict], target_program: int, target_family: Optional[str], fam_keywords: Dict[str, List[str]], override_keywords: List[str]) -> Dict:
    best = entries[0]
    best_score = score_entry(best, target_program, target_family, fam_keywords, override_keywords)
    for entry in entries[1:]:
        sc = score_entry(entry, target_program, target_family, fam_keywords, override_keywords)
        if sc > best_score:
            best = entry
            best_score = sc
    return best


def build_patch_map(wavetable_map: List[Dict], profile: Dict) -> List[Dict]:
    if not wavetable_map:
        raise ValueError("wavetable map is empty")

    prog_to_family = build_program_family_map(profile)
    fam_keys = family_keywords(profile)
    fam_policy = family_playback_policies(profile)
    prog_override = {
        int(k): [str(v).lower() for v in vals]
        for k, vals in profile.get("program_keyword_overrides", {}).items()
    }

    out: List[Dict] = []
    for program in range(128):
        family = prog_to_family.get(program, "unclassified")
        chosen = choose_entry(wavetable_map, program, family, fam_keys, prog_override.get(program, []))
        policy = fam_policy.get(family, {
            "note_trim_ms": 0,
            "min_hold_ms": 16,
            "velocity_gamma": 1.0,
            "velocity_gain": 1.0,
            "release_bias": 0,
        })
        out.append({
            "id": chosen.get("id"),
            "gm_idx": program,
            "gm_name": GM_INSTRUMENTS.get(program, "Unknown"),
            "filename": chosen.get("filename"),
            "rel_path": chosen.get("rel_path"),
            "source": chosen.get("source"),
            "patch_family": family,
            "policy_note_trim_ms": int(policy["note_trim_ms"]),
            "policy_min_hold_ms": int(policy["min_hold_ms"]),
            "policy_velocity_gamma": float(policy["velocity_gamma"]),
            "policy_velocity_gain": float(policy["velocity_gain"]),
            "policy_release_bias": int(policy["release_bias"])
        })
    return out


def build_coverage_data(patch_map: List[Dict]) -> Dict:
    by_id: Dict[int, List[int]] = collections.defaultdict(list)
    by_family: Dict[str, Dict[str, Any]] = {}

    for row in patch_map:
        sid = int(row.get("id", 0) or 0)
        gm_idx = int(row.get("gm_idx", 0) or 0)
        fam = str(row.get("patch_family", "unclassified"))
        by_id[sid].append(gm_idx)

        fam_bucket = by_family.setdefault(fam, {"programs": [], "sample_ids": set()})
        fam_bucket["programs"].append(gm_idx)
        fam_bucket["sample_ids"].add(sid)

    duplicate_samples = []
    for sid, programs in by_id.items():
        if len(programs) > 1:
            duplicate_samples.append({
                "id": sid,
                "program_count": len(programs),
                "programs": sorted(programs)
            })
    duplicate_samples.sort(key=lambda x: x["program_count"], reverse=True)

    family_summary = []
    for fam, bucket in sorted(by_family.items()):
        family_summary.append({
            "family": fam,
            "program_count": len(bucket["programs"]),
            "unique_sample_count": len(bucket["sample_ids"]),
            "programs": sorted(bucket["programs"])
        })

    return {
        "totals": {
            "program_count": len(patch_map),
            "unique_sample_count": len(by_id),
            "duplicate_sample_count": len(duplicate_samples)
        },
        "duplicate_samples": duplicate_samples,
        "family_summary": family_summary,
        "program_assignments": sorted(patch_map, key=lambda x: int(x.get("gm_idx", 0)))
    }


def write_text_report(path: str, coverage: Dict) -> None:
    lines: List[str] = []
    totals = coverage["totals"]
    lines.append("Family Patch Coverage Report")
    lines.append("============================")
    lines.append(f"Programs: {totals['program_count']}")
    lines.append(f"Unique samples: {totals['unique_sample_count']}")
    lines.append(f"Duplicate sample IDs: {totals['duplicate_sample_count']}")
    lines.append("")

    lines.append("Family Summary")
    lines.append("--------------")
    for fam in coverage["family_summary"]:
        lines.append(
            f"{fam['family']}: programs={fam['program_count']} unique_samples={fam['unique_sample_count']}"
        )
    lines.append("")

    lines.append("Top Duplicate Samples")
    lines.append("---------------------")
    if not coverage["duplicate_samples"]:
        lines.append("(none)")
    else:
        for item in coverage["duplicate_samples"][:20]:
            plist = ",".join(str(p) for p in item["programs"])
            lines.append(f"id={item['id']} programs={item['program_count']} [{plist}]")
    lines.append("")

    lines.append("Program Assignments")
    lines.append("-------------------")
    for row in coverage["program_assignments"]:
        lines.append(
            f"{int(row['gm_idx']):03d} | {row['gm_name']} | {row['patch_family']} | id={row['id']} | {row['filename']}"
        )

    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser(description="Build a family-driven patch map for midi2afx.")
    parser.add_argument("--map", default="input/wavetables.map", help="Input wavetable map JSON")
    parser.add_argument("--profiles", default="input/family_patch_profiles.json", help="Family patch profile JSON")
    parser.add_argument("--out", default="input/wavetables.family.map", help="Output map path")
    parser.add_argument("--coverage-json", default="input/wavetables.family.coverage.json", help="Coverage report JSON path")
    parser.add_argument("--coverage-txt", default="input/wavetables.family.coverage.txt", help="Coverage report text path")
    args = parser.parse_args()

    wavetable_map = load_json(args.map)
    profile = load_json(args.profiles)
    patch_map = build_patch_map(wavetable_map, profile)

    out_dir = os.path.dirname(args.out)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(patch_map, f, indent=4)

    coverage = build_coverage_data(patch_map)
    with open(args.coverage_json, "w", encoding="utf-8") as f:
        json.dump(coverage, f, indent=4)
    write_text_report(args.coverage_txt, coverage)

    print(f"Wrote {args.out} with {len(patch_map)} program patches")
    print(
        "Coverage: "
        f"programs={coverage['totals']['program_count']} "
        f"unique_samples={coverage['totals']['unique_sample_count']} "
        f"duplicate_ids={coverage['totals']['duplicate_sample_count']}"
    )
    print(f"Coverage JSON: {args.coverage_json}")
    print(f"Coverage TXT: {args.coverage_txt}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())