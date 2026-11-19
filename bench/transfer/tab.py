#!/usr/bin/env python3
"""Tables from xfer_bench "R|test|name...|key=value|..." lines.

usage: tab.py LOG leg|pair|fan|hostor|conc|df|cont [substring filter ...]
"""
import statistics
import sys
from collections import defaultdict


def parse(path):
    rows = []
    for line in open(path):
        if not line.startswith("R|"):
            continue
        parts = line.rstrip("\n").split("|")
        row = {"test": parts[1], "tags": [], "kv": {}}
        for p in parts[2:]:
            if "=" in p and not p.startswith("p2p:") and ":" not in p.split("=")[0]:
                k, v = p.split("=", 1)
                row["kv"][k] = v
            else:
                row["tags"].append(p)
        rows.append(row)
    return rows


def f(row, key, default=float("nan")):
    try:
        return float(row["kv"][key])
    except (KeyError, ValueError):
        return default


def name(row, skip=()):
    keys = [k for k in row["kv"] if k not in (
        "bytes", "med_ms", "min_ms", "max_ms", "gibps", "ms_per_gib", "submit_ms", "mism", "reps",
        "legs", "agg_gibps", "leg_gibps", "ERR") and k not in skip]
    return " ".join(row["tags"] + [f"{k}={row['kv'][k]}" for k in keys])


def leg(rows):
    # GiB/s per (kind, n): median over the 4 GPUs, with range.
    acc = defaultdict(list)
    for r in rows:
        nm = " ".join(t for t in r["tags"] if not t.startswith("g")) + " " + " ".join(
            f"{k}={r['kv'][k]}" for k in ("host", "dev", "chunk", "blk") if k in r["kv"])
        acc[(nm, int(r["kv"]["n"]))].append(f(r, "gibps"))
    sizes = sorted({k[1] for k in acc})
    print("| leg | " + " | ".join(f"{s} MiB" for s in sizes) + " |")
    print("|---|" + "---|" * len(sizes))
    for nm in dict.fromkeys(k[0] for k in acc):
        cells = []
        for s in sizes:
            v = acc.get((nm, s))
            cells.append(f"{statistics.median(v):.1f} ({min(v):.1f}-{max(v):.1f})" if v else "-")
        print(f"| {nm} | " + " | ".join(cells) + " |")


def pair(rows):
    acc = defaultdict(list)
    bad = defaultdict(int)
    sub = defaultdict(list)
    for r in rows:
        nm = name(r, skip=("p2p", "n"))
        nm = " ".join(t for t in nm.split(" ") if ">" not in t)
        key = (nm, "P2P pairs" if r["kv"].get("p2p") == "1" else "cross pairs", int(r["kv"]["n"]))
        if "ERR" in r["kv"]:
            bad[key] += 1
            continue
        acc[key].append(f(r, "gibps"))
        sub[key].append(f(r, "submit_ms"))
        if r["kv"].get("mism", "0") != "0":
            bad[key] += 1
    sizes = sorted({k[2] for k in list(acc) + list(bad)})
    for cls in ("P2P pairs", "cross pairs"):
        print(f"\n**{cls}** GiB/s, median over ordered pairs (min-max) [pairs]; '!' = wrong bytes or error\n")
        print("| method | " + " | ".join(f"{s} MiB" for s in sizes) + " |")
        print("|---|" + "---|" * len(sizes))
        for nm in dict.fromkeys(k[0] for k in list(acc) + list(bad)):
            cells = []
            for s in sizes:
                v = acc.get((nm, cls, s))
                b = bad.get((nm, cls, s), 0)
                if v:
                    cells.append(f"{statistics.median(v):.1f} ({min(v):.1f}-{max(v):.1f}) [{len(v)}]" +
                                 (f" !{b}" if b else ""))
                else:
                    cells.append(f"!{b}" if b else "-")
            if any(c != "-" for c in cells):
                print(f"| {nm} | " + " | ".join(cells) + " |")


def listing(rows, cols):
    print("| config | " + " | ".join(cols) + " |")
    print("|---|" + "---|" * len(cols))
    for r in rows:
        if "ERR" in r["kv"]:
            print(f"| {name(r)} | ERR {r['kv']['ERR']} |")
            continue
        print(f"| {name(r)} | " + " | ".join(r["kv"].get(c, "-") for c in cols) + " |")


def df(rows):
    # one row per schedule, columns per B: ms (ms/GiB)
    acc = {}
    sizes = []
    for r in rows:
        b = int(r["kv"]["B"])
        if b not in sizes:
            sizes.append(b)
        nm = name(r, skip=("B",))
        if "ERR" in r["kv"]:
            acc[(nm, b)] = "ERR"
        else:
            flag = "" if r["kv"].get("mism", "0") == "0" else " !WRONG"
            acc[(nm, b)] = (f"{f(r, 'med_ms'):.1f} [{f(r, 'min_ms'):.1f}-{f(r, 'max_ms'):.1f}] "
                            f"({f(r, 'ms_per_gib'):.0f}){flag}")
    print("| schedule | " + " | ".join(f"B={s} MiB: ms [min-max] (ms/GiB)" for s in sizes) + " |")
    print("|---|" + "---|" * len(sizes))
    for nm in dict.fromkeys(k[0] for k in acc):
        print(f"| {nm} | " + " | ".join(acc.get((nm, s), "-") for s in sizes) + " |")


def main():
    path, what = sys.argv[1], sys.argv[2]
    filters = sys.argv[3:]
    allrows = parse(path)
    if what == "df":
        rows = [r for r in allrows if r["test"] == "df" and "bg" not in r["kv"]]
    elif what == "cont":
        rows = [r for r in allrows if r["test"] == "cont" or (r["test"] == "df" and "bg" in r["kv"])]
    else:
        rows = [r for r in allrows if r["test"] == what]
    if filters:
        rows = [r for r in rows if all(x in "|".join(r["tags"] + [f"{k}={v}" for k, v in r["kv"].items()])
                                       for x in filters)]
    if what == "leg":
        leg(rows)
    elif what == "pair":
        pair(rows)
    elif what in ("df",):
        df(rows)
    elif what == "conc":
        listing(rows, ["med_ms", "agg_gibps", "leg_gibps"])
    elif what == "cont":
        legs = [r for r in rows if "legs" in r["kv"]]
        df([r for r in rows if "legs" not in r["kv"]])
        if legs:
            listing(legs, ["med_ms", "agg_gibps", "leg_gibps"])
    else:
        listing(rows, ["med_ms", "min_ms", "max_ms", "gibps", "ms_per_gib", "submit_ms", "mism"])


if __name__ == "__main__":
    main()
