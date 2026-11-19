#!/usr/bin/env python3
"""lpdiff.py <q...>: diff DF facts of the hot iteration between lp_tip, lp_kind, lp_bpk8 (df_summary.json of parse_df.py)."""
import json, sys
B = "/opt/dlami/nvme/dfperf/runs/vop4"
S = {l: json.load(open(f"{B}/lp_{l}/df_summary.json")) for l in ("tip", "kind", "bpk8")}
def keysig(j):
    return [(k.get("condition"), k.get("kind"), k.get("size"), k.get("valid", k.get("rows")), k.get("pushed")) for k in j["keys"]]
def pubs(j):
    return [(p["filters"], p["targets"], p["host"]) for p in j["pubs"]]
for q in sys.argv[1:]:
    t = S["tip"].get(q)
    if t is None: continue
    print(f"## q{q}")
    for l in ("kind", "bpk8"):
        o = S[l].get(q)
        if o is None: print(f"  {l}: missing"); continue
        diffs = []
        for f in ("constructions", "keys", "declines", "reasons", "layouts", "errors", "fallback"):
            if t[f] != o[f]: diffs.append(f"{f}: tip {t[f]} vs {o[f]}")
        tj = {j["join"]: j for j in t["joins"]}; oj = {j["join"]: j for j in o["joins"]}
        if set(tj) != set(oj): diffs.append(f"join set differs {sorted(tj)} vs {sorted(oj)}")
        for jn in sorted(set(tj) & set(oj)):
            a, b = tj[jn], oj[jn]
            if len(a["started"]) != len(b["started"]): diffs.append(f"j{jn} started {len(a['started'])} vs {len(b['started'])}")
            if pubs(a) != pubs(b): diffs.append(f"j{jn} published (filters,targets,HOST) {pubs(a)} vs {pubs(b)}")
            ka, kb = keysig(a), keysig(b)
            if ka != kb:
                for x, y in zip(ka, kb):
                    if x != y: diffs.append(f"j{jn} key c{x[0]}: {x[1]} size={x[2]} valid={x[3]} pushed={x[4]} -> {y[1]} size={y[2]} valid={y[3]} pushed={y[4]}")
                if len(ka) != len(kb): diffs.append(f"j{jn} key count {len(ka)} vs {len(kb)}")
        if t["kept"] != o["kept"]:
            for k in sorted(set(t["kept"]) | set(o["kept"])):
                if t["kept"].get(k) != o["kept"].get(k): diffs.append(f"scan probes ran/requested/complete {k}: [chunks, kept, probed] tip {t['kept'].get(k)} vs {o['kept'].get(k)}")
        if t["df_ops"] != o["df_ops"]:
            for k in sorted(set(t["df_ops"]) | set(o["df_ops"])):
                x, y = t["df_ops"].get(k), o["df_ops"].get(k)
                if (x and y and (x["rows_in"], x["rows_out"], x["tasks"]) != (y["rows_in"], y["rows_out"], y["tasks"])) or not (x and y):
                    diffs.append(f"DYNAMIC_FILTER op {k}: rows in->out tip {x and (x['rows_in'], x['rows_out'], x['tasks'])} vs {y and (y['rows_in'], y['rows_out'], y['tasks'])}")
        print(f"  {l}: " + ("identical DF facts" if not diffs else f"{len(diffs)} difference(s)"))
        for d in diffs: print("     -", d)
