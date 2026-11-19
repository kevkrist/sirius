#!/usr/bin/env python3
"""final_tables.py calc|suite : final timing tables of the 2026-11-19 re-run (medians of hot iterations >= 2, ms).
calc : per query, delta vs the tip of the same tag (same lock holds) for every arm; tags searched in order.
suite: tag fs2, arms tip / g2 / f1 / bf2, per query + sum."""
import csv, glob, statistics as st, sys
B = "/opt/dlami/nvme/dfperf/runs/eager"
def hot(tag, arm, q):
    out = []
    for d in sorted(glob.glob(f"{B}/{tag}_{arm}[0-9]/tpch_*_q{q}")):
        try: rows = list(csv.DictReader(open(f"{d}/csv/runtimes.csv")))
        except Exception: continue
        t = {int(r["iteration"]): float(r["runtime_s"]) * 1000 for r in rows}
        out += [t[i] for i in sorted(t) if i >= 2]
    return out
def cell(tags, arm, q):
    for tag in tags:
        v, t = hot(tag, arm, q), hot(tag, "tip", q)
        if v and t: return st.median(v) - st.median(t), len(v), max(v) - min(v), tag
    return None
mode = sys.argv[1]
if mode == "calc":
    arms = [("pro", ["v1"]), ("o1", ["c4", "b2"]), ("k1", ["c4", "b2"]), ("k2", ["b2"]), ("k3", ["b2"]), ("o2", ["c4", "b2"]),
            ("bs", ["c3"]), ("bs2", ["c4"]), ("bs2k", ["c4"]), ("b02", ["c4"]), ("adm", ["c4"])]
    qs = [3, 8, 21, 16, 7, 5, 9, 12, 4, 10, 2, 6]
    print("| q | tip ms | " + " | ".join(a for a, _ in arms) + " |"); print("|" + "---|" * (len(arms) + 2))
    tot = {a: [0.0, 0] for a, _ in arms}
    for q in qs:
        t = hot("c4", "tip", q) or hot("b2", "tip", q)
        row = []
        for a, tags in arms:
            c = cell(tags, a, q)
            if c is None: row.append("-"); continue
            row.append(f"{c[0]:+.1f}"); tot[a][0] += c[0]; tot[a][1] += 1
        print(f"| {q} | {st.median(t):.1f} | " + " | ".join(row) + " |" if t else f"| {q} | - |")
    print("| sum (queries run) | | " + " | ".join(f"{v[0]:+.0f} ({v[1]})" for v in tot.values()) + " |")
else:
    arms = ["tip", "g2", "f1", "bf2"]; qs = range(1, 23); tot = {a: 0.0 for a in arms}; totc = {a: 0.0 for a in arms}; nq = 0
    print("| q | tip ms (n, spread) | g2 | f1 | bf2 | g2-tip | f1-tip | bf2-tip | f1 % |"); print("|" + "---|" * 9)
    for q in qs:
        v = {a: hot("fs2", a, q) for a in arms}
        if not v["tip"] or not v["f1"]: print(f"| {q} | incomplete |"); continue
        m = {a: (st.median(v[a]) if v[a] else None) for a in arms}; nq += 1
        for a in arms:
            if m[a] is not None: tot[a] += m[a]
            totc[a] += m[a] if m[a] is not None else m["f1"]     # g2 not run on pure controls: identical code path to f1 there
        f = lambda a: f"{m[a]:.1f} ({len(v[a])}, {max(v[a])-min(v[a]):.0f})" if m[a] is not None else "= f1"
        d = lambda a: f"{m[a]-m['tip']:+.1f}" if m[a] is not None else "-"
        print(f"| {q} | {f('tip')} | {f('g2')} | {f('f1')} | {f('bf2')} | {d('g2')} | {d('f1')} | {d('bf2')} | {100*(m['f1']-m['tip'])/m['tip']:+.1f} |")
    print(f"| sum ({nq} q) | {totc['tip']:.0f} | {totc['g2']:.0f} | {totc['f1']:.0f} | {totc['bf2']:.0f} | {totc['g2']-totc['tip']:+.0f} | {totc['f1']-totc['tip']:+.0f} | {totc['bf2']-totc['tip']:+.0f} | {100*(totc['f1']-totc['tip'])/totc['tip']:+.2f} |")
