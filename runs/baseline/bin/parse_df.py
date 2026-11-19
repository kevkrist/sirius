#!/usr/bin/env python3
"""Parse one per-query Sirius trace log (performance_test.py split) into DF construction facts.

Usage: parse_df.py <label_dir> [q ...]  -> markdown on stdout, JSON at <label_dir>/df_summary.json
Uses the LAST query window in each log (hot iteration); also counts constructions in the first one.
Association of 'construction started' / 'published' / bloom 'finish' lines to a join is positional
(nearest preceding 'partition strategy' / nearest following [dynamic_filter_key] line); a mismatch
between bloom finish bytes and the join's bloom key sizes is flagged with '!'.
"""
import glob, json, os, re, sys
from collections import defaultdict, OrderedDict

R_STRAT = re.compile(r"sirius_physical_hash_join id (\d+) partition strategy: (\d+) partitions \((\d+) GPUs\), build side (\d+) bytes, (\d+) rows.*?Join Type: (\w+)\. Join Mode: (\w+)\s*(\[broadcast\])?")
R_EXCH = re.compile(r"partition_exchange resolved (\d+) inputs on sizing input \d+: (\d+) partitions, mode (\w+), sized from (\d+) bytes and (\d+) rows")
R_DOM = re.compile(r"\[dynamic_filter_domain\] path=(\w+) condition=(\d+) rows=(\d+) domain=(\d+) provenance=(\w+) eligible=(\d) threshold=([\d.]+) decision=(\w+)")
R_START = re.compile(r"construction started: (\d+) key\(s\), (\d+) batch\(es\) on (\d+) GPU\(s\)")
R_PUB = re.compile(r"constructed filters published( \(Bloom through HOST staging\))?: (\d+) filter\(s\) pushed to (\d+) target\(s\), ([\d.]+) ms after the final contribution")
R_FIN = re.compile(r"accumulated_bloom_builder::finish\] root GPU (\d+) reduced and replicated (\d+) key\(s\), (\d+) bytes in all, through (.+?): (\d+) contributing source\(s\), (\d+) target\(s\)")
R_KEY = re.compile(r"\[dynamic_filter_key\] (.*)$")
R_SETTLED = re.compile(r"accumulation settled: outcome=(\S+)")
R_DECL = re.compile(r"(construction declined.*|.*constructs no dynamic filter.*|construction skipped.*|Bloom filters refused.*|passed through unfiltered.*)")
R_OP = re.compile(r"\[GPU:(\d+)\] Pipeline (\d+): operator (\w+) \(id=(\d+)\) task=(\d+) (executing on|produced) (\d+) batches, num rows: ([\d ]*), size: (\d+) bytes.*?(?:execution time: ([\d.]+) ms)?")
R_OPT = re.compile(r"execution time: ([\d.]+) ms")
R_JOINKEPT = re.compile(r"join filters kept (\d+)/(\d+) rows \((\d+) of (\d+) probe\(s\) ran, complete=(\w+)\)")
R_DECODE = re.compile(r"decode (APPLIED|NOT applied \(plain output\)) .*join_filters=(\d+) survivors=(-?\d+)/(\d+)")
R_CKPT = re.compile(r"cached checkpoint: (\d+) -> (\d+) rows with (\d+) dynamic filters")
R_PIPE = re.compile(r"^Pipeline #(\d+): (.*)$")
R_WIRED = re.compile(r"Wired hash join with (\d+) dynamic-filter probe target\(s\) \((.*?)\)")
R_TS = re.compile(r"^\[(\d+-\d+-\d+ \d+:\d+:\d+\.\d+)\]")


def windows(lines):
    """Return list of (start,end) line index ranges of transparent-execution query windows."""
    out, start = [], None
    for i, l in enumerate(lines):
        if "QueryBegin: transparent_execution" in l:
            start = i
        elif start is not None and "[window] end" in l:
            out.append((start, i, "outcome=ok" in l))
            start = None
    return out


def plan_window_start(lines, wstart):
    # the plan/pipeline print and 'Wired' lines precede 'transparent_execution' slightly: back up to the SQL QueryBegin
    i = wstart
    while i > 0 and "QueryBegin: instance" not in lines[i]:
        i -= 1
    return i


def parse_window(lines):
    joins = OrderedDict()  # join id -> dict
    order = []  # construction events in order
    cur_join = None
    pending = []  # started/fin/pub events waiting for a key line
    ops = defaultdict(lambda: dict(tasks=0, rows_in=0, rows_out=0, ms=0.0, max_ms=0.0, gpus=set(), bytes_in=0))
    opname = {}
    oppipe = {}
    declines = defaultdict(int)
    kept = defaultdict(lambda: [0, 0, 0])  # (ran, requested, complete) -> [n, kept, in]
    decode = defaultdict(lambda: [0, 0, 0])
    ckpt = [0, 0, 0]
    pipes = {}
    pipe_out = {}
    wired = []
    last_pipe = None
    doms = []
    for l in lines:
        m = R_PIPE.match(l)
        if m:
            last_pipe = int(m.group(1)); pipes[last_pipe] = m.group(2).strip(); continue
        if last_pipe is not None and l.startswith("  Output:"):
            pipe_out[last_pipe] = l.strip(); continue
        if "Wired hash join" in l:
            m = R_WIRED.search(l)
            if m: wired.append(m.group(2))
            continue
        m = R_OP.search(l)
        if m:
            gpu, pipe, name, oid, task, what, nb, rows, size, _ = m.groups()
            oid = int(oid); opname[oid] = name; oppipe[oid] = int(pipe)
            o = ops[oid]
            nrows = [int(x) for x in rows.split()]
            if what == "executing on":
                o["tasks"] += 1; o["rows_in"] += sum(nrows); o["gpus"].add(int(gpu)); o["bytes_in"] += int(size)
            else:
                # PARTITION in broadcast mode replicates the rows per partition; record raw sum
                o["rows_out"] += sum(nrows)
                t = R_OPT.search(l)
                if t:
                    ms = float(t.group(1)); o["ms"] += ms; o["max_ms"] = max(o["max_ms"], ms)
            continue
        m = R_STRAT.search(l)
        if m:
            jid = int(m.group(1))
            joins.setdefault(jid, dict(join=jid, keys=[], started=[], pubs=[], fins=[], doms=[]))
            joins[jid].update(partitions=int(m.group(2)), gpus=int(m.group(3)), build_bytes=int(m.group(4)),
                              build_rows=int(m.group(5)), jtype=m.group(6), jmode=m.group(7), broadcast=bool(m.group(8)))
            cur_join = jid
            continue
        m = R_EXCH.search(l)
        if m and cur_join is not None and int(m.group(4)) == joins[cur_join].get("build_bytes"):
            joins[cur_join]["exch_mode"] = m.group(3)
            continue
        m = R_DOM.search(l)
        if m:
            d = dict(path=m.group(1), condition=int(m.group(2)), rows=int(m.group(3)), domain=int(m.group(4)),
                     provenance=m.group(5), eligible=int(m.group(6)), decision=m.group(8))
            doms.append(d)
            if cur_join is not None: joins[cur_join]["doms"].append(d)
            continue
        m = R_START.search(l)
        if m:
            ev = dict(kind="start", keys=int(m.group(1)), batches=int(m.group(2)), gpus=int(m.group(3)), join_guess=cur_join)
            pending.append(ev); continue
        m = R_FIN.search(l)
        if m:
            pending.append(dict(kind="fin", root=int(m.group(1)), keys=int(m.group(2)), bytes=int(m.group(3)),
                                transport=m.group(4), sources=int(m.group(5)), targets=int(m.group(6)))); continue
        m = R_PUB.search(l)
        if m:
            pending.append(dict(kind="pub", host=bool(m.group(1)), filters=int(m.group(2)), targets=int(m.group(3)),
                                ms=float(m.group(4)))); continue
        m = R_KEY.search(l)
        if m:
            kv = dict(x.split("=", 1) for x in m.group(1).split() if "=" in x)
            jid = int(kv.get("join", -1))
            j = joins.setdefault(jid, dict(join=jid, keys=[], started=[], pubs=[], fins=[], doms=[]))
            j["keys"].append(kv)
            for ev in pending:
                if ev["kind"] == "start": j["started"].append(ev)
                elif ev["kind"] == "fin": j["fins"].append(ev)
                else: j["pubs"].append(ev)
            pending = []
            continue
        m = R_JOINKEPT.search(l)
        if m:
            k = (int(m.group(3)), int(m.group(4)), m.group(5))
            kept[k][0] += 1; kept[k][1] += int(m.group(1)); kept[k][2] += int(m.group(2)); continue
        m = R_DECODE.search(l)
        if m:
            k = (m.group(1)[:3], int(m.group(2)))
            decode[k][0] += 1
            if int(m.group(3)) >= 0: decode[k][1] += int(m.group(3)); decode[k][2] += int(m.group(4))
            continue
        m = R_CKPT.search(l)
        if m:
            ckpt[0] += 1; ckpt[1] += int(m.group(1)); ckpt[2] += int(m.group(2)); continue
        if "[dynamic_filter" in l or "build_input_certifier" in l or "plan_construction" in l:
            m = R_DECL.search(l)
            if m:
                s = re.sub(r"^\[[^]]*\] \[\w+\] \[[^]]*\] ", "", l.strip())
                declines[s[:300]] += 1
    return dict(joins=joins, ops=ops, opname=opname, oppipe=oppipe, declines=declines, kept=kept, decode=decode,
                ckpt=ckpt, pipes=pipes, pipe_out=pipe_out, wired=wired, pending=pending, doms=doms)


def build_partition_of(w):
    """join id -> build-side PARTITION op id, via pipeline print (PARTITION -> CONCAT -> HASH_JOIN port build)."""
    # pipelines: idx -> "A (id=x) -> B (id=y)"; outputs "-> CONCAT [port: default,...]"
    res = {}
    first_op = {}
    last_op = {}
    for p, desc in w["pipes"].items():
        ids = re.findall(r"(\w+) \(id=(\d+)\)", desc)
        if ids:
            first_op[p] = (ids[0][0], int(ids[0][1])); last_op[p] = (ids[-1][0], int(ids[-1][1]))
    return first_op, last_op


def fmt_bytes(b):
    b = float(b)
    for u in ("B", "KB", "MB", "GB"):
        if b < 1024 or u == "GB": return f"{b:.0f}{u}" if u == "B" else f"{b:.1f}{u}"
        b /= 1024


def main():
    label_dir = sys.argv[1]
    qs = [int(x) for x in sys.argv[2:]] or list(range(1, 23))
    summary = {}
    for q in qs:
        fs = glob.glob(f"{label_dir}/tpch_*_q{q}/log_dir/sirius_*.log")
        if not fs:
            print(f"\n### q{q}\nNO LOG"); continue
        raw = open(sorted(fs)[-1], errors="replace").read().splitlines()
        wins = windows(raw)
        if not wins:
            print(f"\n### q{q}\nNO QUERY WINDOW"); continue
        ws, we, ok = wins[-1]
        ps = plan_window_start(raw, ws)
        w = parse_window(raw[ps:we + 1])
        first = parse_window(raw[plan_window_start(raw, wins[0][0]):wins[0][1] + 1]) if len(wins) > 1 else None
        t0 = R_TS.match(raw[ps]); t1 = R_TS.match(raw[we])
        fallback = sum(1 for l in raw if re.search(r"fallback", l, re.I))
        errs = sum(1 for l in raw if re.search(r"Error in Sirius|bad_alloc|out of memory|illegal memory", l))
        retries = sum(1 for l in raw if "reschedule (retry" in l)
        print(f"\n### q{q}")
        print(f"windows={len(wins)} last_ok={ok} fallback_lines={fallback} error_lines={errs} oom_retries={retries} "
              f"wired_joins={len(w['wired'])} hot window {t0.group(1)[11:] if t0 else '?'} -> {t1.group(1)[11:] if t1 else '?'}")
        js = [j for j in w["joins"].values()]
        qsum = dict(constructions=0, bloom_bytes=0, keys=0, kinds=defaultdict(int), pub_ms=[], host=0, batches=[],
                    reasons=defaultdict(int), layouts=defaultdict(int), declines=dict(w["declines"]))
        if js:
            print("\n| join | type/mode | layout (parts, exch) | build rows | build bytes | constr: keys/batches/GPUs | key: cond kind size valid/rows domain(prov) bits/valid pushed reason | bloom finish: bytes root src/tgt transport | published: filters->targets, ms after final, HOST | build PARTITION (rows match): id tasks sum_ms max_ms |")
            print("|---|---|---|---|---|---|---|---|---|---|")
        for j in js:
            lay = f"{j.get('partitions','?')}p {j.get('exch_mode','?')}{' [broadcast]' if j.get('broadcast') else ''}"
            st = "; ".join(f"{s['keys']}k/{s['batches']}b/{s['gpus']}G" for s in j["started"]) or "-"
            dom = {d["condition"]: d for d in j["doms"]}
            ks = []
            for k in j["keys"]:
                c = int(k.get("condition", -1)); d = dom.get(c)
                valid = int(k.get("valid", 0)); size = int(k.get("size", 0))
                bits = f"{size * 8 / valid:.1f}" if valid and k.get("kind") == "bloom" else "-"
                ds = f"{d['domain']}({d['provenance']},{d['decision']})" if d else "?"
                ks.append(f"c{c} {k.get('kind')} {fmt_bytes(size)} {valid}/{k.get('rows')} dom={ds} bits={bits} pushed={k.get('pushed')} zone={k.get('zone_map')} reason={k.get('reason')}")
                qsum["keys"] += 1; qsum["kinds"][k.get("kind")] += 1
                if k.get("kind") == "bloom": qsum["bloom_bytes"] += size
                if k.get("reason", "-") != "-": qsum["reasons"][k.get("reason")] += 1
            kb = sum(int(k.get("size", 0)) for k in j["keys"] if k.get("kind") == "bloom")
            fb = sum(f["bytes"] for f in j["fins"])
            fin = "; ".join(f"{fmt_bytes(f['bytes'])} root{f['root']} {f['sources']}/{f['targets']} {f['transport']}" for f in j["fins"]) or "-"
            if j["fins"] and fb != kb: fin += " !bytes-mismatch"
            bpl = [dict(id=oid, tasks=o["tasks"], sum_ms=round(o["ms"], 1), max_ms=round(o["max_ms"], 1)) for oid, o in sorted(w["ops"].items())
                   if w["opname"][oid] == "PARTITION" and o["rows_in"] == j.get("build_rows")]
            bp = "; ".join(f"id={b['id']} {b['tasks']}t {b['sum_ms']} {b['max_ms']}" for b in bpl) or "?"
            pub = "; ".join(f"{p['filters']}->{p['targets']}, {p['ms']}ms, {'HOST' if p['host'] else 'no'}" for p in j["pubs"]) or "-"
            print(f"| {j['join']} | {j.get('jtype','?')}/{j.get('jmode','?')} | {lay} | {j.get('build_rows','?')} | {fmt_bytes(j.get('build_bytes',0))} | {st} | {'<br>'.join(ks) or '-'} | {fin} | {pub} | {bp} |")
            j["build_partition"] = bpl
            qsum["constructions"] += len(j["started"])
            qsum["layouts"][lay] += 1
            for s in j["started"]: qsum["batches"].append(s["batches"])
            for p in j["pubs"]:
                qsum["pub_ms"].append(p["ms"]); qsum["host"] += int(p["host"])
        if not js:
            print("no hash-join partition strategy / DF key lines")
        if w["pending"]:
            print(f"\nunattached DF events (no following key line): {w['pending']}")
        if w["declines"]:
            print("\nDeclines/skips/refusals:")
            for s, n in w["declines"].items(): print(f"- x{n}: {s}")
        # consumer side
        dfops = [(oid, o) for oid, o in sorted(w["ops"].items()) if w["opname"][oid] == "DYNAMIC_FILTER"]
        if dfops:
            print("\nDYNAMIC_FILTER operators (trace): " + "; ".join(
                f"id={oid} [{w['pipes'].get(w['oppipe'][oid], '?')}] tasks={o['tasks']} rows {o['rows_in']}->{o['rows_out']} keep={o['rows_out'] / o['rows_in']:.3f} ms={o['ms']:.1f}" if o["rows_in"] else f"id={oid} tasks={o['tasks']} rows 0"
                for oid, o in dfops))
        if w["kept"]:
            print("Compressed-scan join-filter probes (ran of requested, complete): " + "; ".join(
                f"{k[0]}/{k[1]} complete={k[2]}: chunks={v[0]} kept {v[1]}/{v[2]} = {v[1] / max(v[2], 1):.4f}" for k, v in sorted(w["kept"].items())))
        if w["decode"]:
            print("Compressed-scan decode outcomes (applied?, #join_filters): " + "; ".join(
                f"{k[0]},jf={k[1]}: n={v[0]}" + (f" survivors {v[1]}/{v[2]}" if v[2] else "") for k, v in sorted(w["decode"].items())))
        if w["ckpt"][0]:
            print(f"Scan cached checkpoints with folded DFs: n={w['ckpt'][0]} rows {w['ckpt'][1]}->{w['ckpt'][2]}")
        # build partition ops
        parts = [(oid, o) for oid, o in sorted(w["ops"].items()) if w["opname"][oid] == "PARTITION"]
        if parts:
            print("PARTITION operators (trace): " + "; ".join(
                f"id={oid} tasks={o['tasks']} rows_in={o['rows_in']} gpus={len(o['gpus'])} sum_ms={o['ms']:.1f} max_ms={o['max_ms']:.1f}" for oid, o in parts))
        if first is not None:
            c0 = sum(len(j["started"]) for j in first["joins"].values()); k0 = sum(len(j["keys"]) for j in first["joins"].values())
            kinds0 = sorted((k.get("join"), k.get("condition"), k.get("kind"), k.get("size"), k.get("pushed")) for j in first["joins"].values() for k in j["keys"])
            kinds1 = sorted((k.get("join"), k.get("condition"), k.get("kind"), k.get("size"), k.get("pushed")) for j in w["joins"].values() for k in j["keys"])
            print(f"iter0 vs hot: constructions {c0} vs {qsum['constructions']}, key lines {k0} vs {qsum['keys']}, key outcomes identical={kinds0 == kinds1}")
        qsum["kinds"] = dict(qsum["kinds"]); qsum["reasons"] = dict(qsum["reasons"]); qsum["layouts"] = dict(qsum["layouts"])
        qsum["df_ops"] = {oid: dict(rows_in=o["rows_in"], rows_out=o["rows_out"], ms=o["ms"], tasks=o["tasks"]) for oid, o in dfops}
        qsum["kept"] = {f"{k[0]}/{k[1]}/{k[2]}": v for k, v in w["kept"].items()}
        qsum["fallback"] = fallback; qsum["errors"] = errs
        qsum["joins"] = [dict(join=j["join"], layout=f"{j.get('partitions')}p {j.get('exch_mode')}", broadcast=j.get("broadcast"),
                              build_rows=j.get("build_rows"), build_bytes=j.get("build_bytes"), started=j["started"], pubs=j["pubs"],
                              fins=j["fins"], keys=j["keys"], doms=j["doms"], build_partition=j.get("build_partition"), jtype=j.get("jtype"), jmode=j.get("jmode")) for j in js]
        summary[q] = qsum
    json.dump(summary, open(os.path.join(label_dir, "df_summary.json"), "w"), indent=1, default=list)


if __name__ == "__main__":
    main()
