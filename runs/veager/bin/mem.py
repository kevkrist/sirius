#!/usr/bin/env python3
"""Verifier: per-iteration peak GPU memory from Quent NDJSON.
 batch  = bytes of data batches resident (Stationary..Destructed) in a GPU memory space
 resv   = batch + reservations of tasks between Preparing and Finalizing on that space
Usage: mem.py <telemetry_root_prefix> <arm_label> [arm_label...]   e.g. mem.py $R/telemetry/eager_r7_ tip1 f11
Prints per query: max over GPUs of the per-iteration peak (median over hot iterations 2..), in GiB."""
import sys, json, glob, os, statistics, collections
G = 1024.0**3
def load(d, kind):
    for f in glob.glob(f"{d}/{kind}/*.ndjson"):
        for l in open(f):
            j = json.loads(l); (ev, body), = j["data"].items()
            yield j["id"], j["timestamp"], ev, body
def session(d):
    gpu = {}
    for i, ts, ev, b in load(d, "MemorySpace"):
        if ev == "Declaration" and "tier=GPU" in b["label"]:
            gpu[i] = (int(b["label"].split("device_id=")[1].split(",")[0]), b["bounds"]["bytes"])
    wins = {}
    for i, ts, ev, b in load(d, "Query"):
        wins.setdefault(i, {})[ev] = ts
    wins = sorted((w["Executing"], w["Finished"]) for w in wins.values() if "Executing" in w and "Finished" in w)
    ev_b, ev_r = [], []   # (ts, space, delta)
    cur = {}
    for i, ts, ev, b in sorted(load(d, "DataBatch"), key=lambda x: (x[1], x[3].get("seq", 0))):
        if ev == "Stationary":
            if i in cur: ev_b.append((ts, cur[i][0], -cur[i][1]))
            sp = b["memory"]["target"]; by = b["memory"]["data"]["bytes"]
            cur[i] = (sp, by); ev_b.append((ts, sp, by))
        elif ev == "Destructed" and i in cur:
            ev_b.append((ts, cur[i][0], -cur[i][1])); del cur[i]
    cur = {}
    for i, ts, ev, b in sorted(load(d, "Task"), key=lambda x: x[1]):
        r = b.get("reservation") if isinstance(b, dict) else None
        if ev in ("Preparing", "Computing") and r and r.get("data"):
            if i not in cur:
                cur[i] = (r["target"], r["data"]["bytes"]); ev_r.append((ts, cur[i][0], cur[i][1]))
        elif ev == "Finalizing" and i in cur:
            ev_r.append((ts, cur[i][0], -cur[i][1])); del cur[i]
    out = []
    for (a, z) in wins:
        res = {}
        for name, evs in (("batch", ev_b), ("resv", ev_b + ev_r)):
            lvl = collections.Counter(); peak = collections.Counter()
            for ts, sp, dl in sorted(evs, key=lambda e: (e[0], e[2])):
                if ts > z: break
                lvl[sp] += dl
                if ts >= a and sp in gpu and lvl[sp] > peak[sp]: peak[sp] = lvl[sp]
            res[name] = max([peak[s] for s in gpu] or [0]) / G
            res[name + "_sum"] = sum(peak[s] for s in gpu) / G
        out.append(res)
    lim = min(v[1] for v in gpu.values()) / G if gpu else 0
    return out, lim
if __name__ == "__main__":
    pre, arms = sys.argv[1], sys.argv[2:]
    qs = sorted({int(os.path.basename(p)[1:]) for a in arms for p in glob.glob(f"{pre}{a}/q*")})
    print("| q | " + " | ".join(f"{a} batch / batch+resv (GiB, worst GPU)" for a in arms) + " | limit |")
    print("|---|" + "---|" * (len(arms) + 1))
    worst = {a: (0, 0, None) for a in arms}
    for q in qs:
        cells = []; lim = 0
        for a in arms:
            ds = glob.glob(f"{pre}{a}/q{q}/*")
            if not ds: cells.append("-"); continue
            its, lim = session(ds[0]); hot = its[2:] if len(its) > 2 else its[1:]
            if not hot: cells.append("-"); continue
            b = statistics.median(x["batch"] for x in hot); r = statistics.median(x["resv"] for x in hot)
            bm = max(x["batch"] for x in hot); rm = max(x["resv"] for x in hot)
            cells.append(f"{b:.1f} / {r:.1f} (max {bm:.1f} / {rm:.1f})")
            if rm > worst[a][1]: worst[a] = (bm, rm, q)
        print(f"| {q} | " + " | ".join(cells) + f" | {lim:.1f} |")
    for a in arms: print(f"worst {a}: batch {worst[a][0]:.1f} GiB, batch+resv {worst[a][1]:.1f} GiB at q{worst[a][2]}")
