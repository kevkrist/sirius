#!/usr/bin/env python3
"""Parse per-query Sirius trace logs of the BASE (c4fa3638) or the TIP (pr2570) into comparable DF facts.

Usage: parse_both.py <label_dir> <out.json>     (label_dir holds tpch_*_q<N>/sirius/q<N>/sirius.log)
Uses the LAST query window (hot iteration). Works on both log dialects:
  base: '[publish_dynamic_filters] dynamic filter key N: build_rows=.. -> kind' + 'dynamic-filter publication: pushed'
        + 'accumulated Bloom started/published' (PARTITION-side accumulation that already exists at base)
  tip : '[dynamic_filter_key] ... join= kind= size= pushed= reason=' + 'construction started' + 'constructed filters published'
Base key lines carry no join id: they are attached to the hash join whose measured build rows equal build_rows
(ambiguous matches are flagged).
"""
import glob, json, re, sys
from collections import defaultdict
from datetime import datetime

R_TS = re.compile(r"^\[(\d+-\d+-\d+ \d+:\d+:\d+\.\d+)\]")
R_STRAT = re.compile(r"sirius_physical_hash_join id (\d+) partition strategy: (\d+) partitions \((\d+) GPUs\), build side (\d+) bytes, (\d+) rows.*?Join Type: (\w+)\. Join Mode: (\w+)\s*(\[broadcast\])?")
R_OP = re.compile(r"\[GPU:(\d+)\] Pipeline (\d+): operator (\w+) \(id=(\d+)\) task=(\d+) (executing on|produced) (\d+) batches, num rows: ([\d ]*), size: (\d+) bytes")
R_OPT = re.compile(r"execution time: ([\d.]+) ms")
R_SINK = re.compile(r"Pipeline (\d+): operator (\w+) \(id=(\d+)\) (sink|prepare) execution time: ([\d.]+) ms")
R_JOINKEPT = re.compile(r"join filters kept (\d+)/(\d+) rows \((\d+) of (\d+) probe\(s\) ran, complete=(\w+)\)")
R_CKPT = re.compile(r"cached checkpoint: (\d+) -> (\d+) rows with (\d+) dynamic filters")
R_WIRED = re.compile(r"Wired hash join with (\d+) dynamic-filter probe target\(s\) \((.*?)\)")
R_DOM = re.compile(r"\[dynamic_filter_domain\] path=(\w+) condition=(\d+) rows=(\d+) domain=(\d+) provenance=(\w+) eligible=(\d) threshold=([\d.]+) decision=(\w+)")
# base
R_BKEY = re.compile(r"dynamic filter key (\d+): build_rows=(\d+) \(valid=(\d+)\) zone_map=(\w+) membership: in_list_set=(\d+)B bloom=(\d+)B L2=(\d+)B inlist_max_l2_fraction=[\d.]+ -> (\w+)")
R_BPUB = re.compile(r"dynamic-filter publication: pushed (\d+) dynamic filter\(s\) across (\d+) active target\(s\) of (\d+) wired target\(s\) \((\d+) build rows, (\d+) bound keys of (\d+) admitted\)")
R_BACC_START = re.compile(r"accumulated Bloom started: (\d+) key\(s\), (\d+) batch\(es\), (\d+) array bytes per GPU on (\d+) GPU\(s\), (\d+)-byte transfer chunks")
R_BACC_PUB = re.compile(r"accumulated Bloom published through (HOST staging|peer copies): (\d+) filter\(s\) pushed to (\d+) target\(s\), ([\d.]+) ms after the final contribution")
R_QUEUED = re.compile(r"\[(sirius_dynamic_\w+filter)\] queued (\d+)-byte replica GPU (\d+) -> GPU (\d+)")
# tip
R_TKEY = re.compile(r"\[dynamic_filter_key\] (.*)$")
R_TSTART = re.compile(r"construction started: (\d+) key\(s\), (\d+) batch\(es\) on (\d+) GPU\(s\)")
R_TPUB = re.compile(r"constructed filters published( \(Bloom through HOST staging\))?: (\d+) filter\(s\) pushed to (\d+) target\(s\), ([\d.]+) ms after the final contribution")
R_FIN = re.compile(r"accumulated_bloom_builder::finish\] root GPU (\d+) reduced and replicated (\d+) key\(s\), (\d+) bytes in all, through (.+?): (\d+) contributing source\(s\), (\d+) target\(s\)")
DECL_PAT = re.compile(r"declined|no accumulated dynamic filter|constructs no dynamic filter|construction skipped|Bloom filters refused|Bloom refused|Bloom skipped|dynamic filter skipped|Skipping dynamic filter push|not pushed to|membership filters declined|passed through unfiltered")


def ts(l):
    m = R_TS.match(l)
    return datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f").timestamp() if m else None


def windows(lines):
    out, start = [], None
    for i, l in enumerate(lines):
        if "QueryBegin: transparent_execution" in l:
            start = i
        elif start is not None and "[window] end" in l:
            out.append((start, i, "outcome=ok" in l)); start = None
    return out


def parse(lines):
    joins = {}
    ops = defaultdict(lambda: dict(name="", tasks=0, rows_in=0, rows_out=0, ms=0.0, max_ms=0.0, sink_ms=0.0, prep_ms=0.0))
    filters = []          # dict(join, key, kind, size, rows, valid, zone, pushed, reason, path)
    pubs = []             # one-shot publications (base) / constructions (tip)
    acc = []              # base accumulated-Bloom events
    declines = defaultdict(int)
    kept = defaultdict(lambda: [0, 0, 0]); ckpt = [0, 0, 0]
    wired = []
    pend_keys = []        # base key lines waiting for their publication line
    pend_tip = []         # tip start/fin/pub waiting for key line
    queued = []           # (ts, bytes, kind)
    doms = []
    last_join = None
    for l in lines:
        m = R_OP.search(l)
        if m:
            gpu, pipe, name, oid, task, what, nb, rows, size = m.groups()
            o = ops[int(oid)]; o["name"] = name
            n = sum(int(x) for x in rows.split())
            if what == "executing on":
                o["tasks"] += 1; o["rows_in"] += n
            else:
                o["rows_out"] += n
                t = R_OPT.search(l)
                if t:
                    v = float(t.group(1)); o["ms"] += v; o["max_ms"] = max(o["max_ms"], v)
            continue
        m = R_SINK.search(l)
        if m:
            o = ops[int(m.group(3))]; o["name"] = m.group(2)
            o["sink_ms" if m.group(4) == "sink" else "prep_ms"] += float(m.group(5)); continue
        m = R_JOINKEPT.search(l)
        if m:
            k = f"{m.group(3)}/{m.group(4)}/{m.group(5)}"
            kept[k][0] += 1; kept[k][1] += int(m.group(1)); kept[k][2] += int(m.group(2)); continue
        m = R_CKPT.search(l)
        if m:
            ckpt[0] += 1; ckpt[1] += int(m.group(1)); ckpt[2] += int(m.group(2)); continue
        if "Wired hash join" in l:
            m = R_WIRED.search(l)
            if m: wired.append(m.group(2))
            continue
        m = R_STRAT.search(l)
        if m:
            jid = int(m.group(1))
            joins[jid] = dict(join=jid, partitions=int(m.group(2)), build_bytes=int(m.group(4)), build_rows=int(m.group(5)),
                              jtype=m.group(6), jmode=m.group(7), broadcast=bool(m.group(8)))
            last_join = jid; continue
        m = R_DOM.search(l)
        if m:
            doms.append(dict(t=ts(l), path=m.group(1), condition=int(m.group(2)), rows=int(m.group(3)), domain=int(m.group(4)),
                             provenance=m.group(5), decision=m.group(8))); continue
        m = R_QUEUED.search(l)
        if m:
            queued.append((ts(l), int(m.group(2)), m.group(1))); continue
        m = R_BKEY.search(l)
        if m:
            kind = m.group(8)
            size = {"bloom": int(m.group(6)), "in_list": int(m.group(5))}.get(kind, 0)
            pend_keys.append(dict(t=ts(l), key=int(m.group(1)), rows=int(m.group(2)), valid=int(m.group(3)),
                                  zone=m.group(4), kind=kind, size=size, in_list_set=int(m.group(5)), bloom=int(m.group(6))))
            continue
        m = R_BPUB.search(l)
        if m:
            t = ts(l); rows = int(m.group(4))
            mine = [k for k in pend_keys if k["rows"] == rows]
            pend_keys = [k for k in pend_keys if k["rows"] != rows]
            cand = [j for j in joins.values() if j["build_rows"] == rows]
            jid = cand[-1]["join"] if cand else None
            amb = len({j["join"] for j in cand}) > 1
            t0 = min((k["t"] for k in mine), default=None)
            dd = [d["t"] for d in doms if d["path"] == "ordinary" and d["rows"] == rows and not d.get("used") and d["t"] <= (t0 or t)]
            for d in doms:
                if d["path"] == "ordinary" and d["rows"] == rows: d["used"] = True
            if dd and t0: t0 = min(t0, min(dd))
            rq = [q for q in queued if t0 is not None and t0 <= q[0] <= t]
            pubs.append(dict(join=jid, ambiguous=amb, pushed=int(m.group(1)), targets=int(m.group(2)), wired=int(m.group(3)),
                             build_rows=rows, bound=int(m.group(5)), admitted=int(m.group(6)),
                             ms_key_to_pub=round((t - t0) * 1e3, 1) if t0 else None,
                             ms_key_to_first_replica=round((rq[0][0] - t0) * 1e3, 1) if rq else None,
                             replica_bytes=sum(q[1] for q in rq), replicas=len(rq), path="one-shot"))
            for k in mine:
                filters.append(dict(join=jid, key=k["key"], kind=k["kind"], size=k["size"], rows=k["rows"], valid=k["valid"],
                                    zone=k["zone"], pushed=1 if int(m.group(1)) > 0 else 0, reason="-", path="one-shot", amb=amb))
            continue
        m = R_BACC_START.search(l)
        if m:
            acc.append(dict(ev="start", t=ts(l), keys=int(m.group(1)), batches=int(m.group(2)), bytes_per_gpu=int(m.group(3)),
                            gpus=int(m.group(4)), chunk=int(m.group(5)), join_guess=last_join,
                            rows=next((d["rows"] for d in reversed(doms) if d["path"] == "accumulated"), None))); continue
        m = R_BACC_PUB.search(l)
        if m:
            acc.append(dict(ev="pub", t=ts(l), transport=m.group(1), filters=int(m.group(2)), targets=int(m.group(3)), ms=float(m.group(4)))); continue
        m = R_TSTART.search(l)
        if m:
            pend_tip.append(dict(ev="start", t=ts(l), keys=int(m.group(1)), batches=int(m.group(2)), gpus=int(m.group(3)))); continue
        m = R_FIN.search(l)
        if m:
            pend_tip.append(dict(ev="fin", t=ts(l), root=int(m.group(1)), keys=int(m.group(2)), bytes=int(m.group(3)),
                                 transport=m.group(4), sources=int(m.group(5)), targets=int(m.group(6)))); continue
        m = R_TPUB.search(l)
        if m:
            pend_tip.append(dict(ev="pub", t=ts(l), host=bool(m.group(1)), filters=int(m.group(2)), targets=int(m.group(3)), ms=float(m.group(4)))); continue
        m = R_TKEY.search(l)
        if m:
            kv = dict(x.split("=", 1) for x in m.group(1).split() if "=" in x)
            jid = int(kv.get("join", -1))
            filters.append(dict(join=jid, key=int(kv.get("condition", -1)), kind=kv.get("kind"), size=int(kv.get("size", 0)),
                                rows=int(kv.get("rows", 0)), valid=int(kv.get("valid", 0)), zone=kv.get("zone_map"),
                                pushed=int(kv.get("pushed", 0)), reason=kv.get("reason"), path="construction", sig=kv.get("sig")))
            if pend_tip:
                st = [e for e in pend_tip if e["ev"] == "start"]; pb = [e for e in pend_tip if e["ev"] == "pub"]
                fn = [e for e in pend_tip if e["ev"] == "fin"]
                pubs.append(dict(join=jid, path="construction", batches=st[0]["batches"] if st else None, gpus=st[0]["gpus"] if st else None,
                                 keys=st[0]["keys"] if st else None, pushed=pb[0]["filters"] if pb else 0,
                                 targets=pb[0]["targets"] if pb else 0, ms_after_final=pb[0]["ms"] if pb else None,
                                 host=pb[0]["host"] if pb else None,
                                 ms_start_to_pub=round((pb[0]["t"] - st[0]["t"]) * 1e3, 1) if st and pb else None,
                                 fin_bytes=sum(e["bytes"] for e in fn), fin_sources=fn[0]["sources"] if fn else None))
                pend_tip = []
            continue
        if DECL_PAT.search(l) and ("dynamic" in l or "Bloom" in l or "build_input_certifier" in l):
            s = re.sub(r"^\[[^]]*\] \[\w+\] \[[^]]*\] ", "", l.strip())
            s = re.sub(r"\(id=\d+\)", "(id=N)", s)
            declines[s[:260]] += 1
    # base keys that never saw a publication line
    for k in pend_keys:
        cand = [j for j in joins.values() if j["build_rows"] == k["rows"]]
        filters.append(dict(join=cand[-1]["join"] if cand else None, key=k["key"], kind=k["kind"], size=k["size"], rows=k["rows"],
                            valid=k["valid"], zone=k["zone"], pushed=0, reason="no-publication-line", path="one-shot"))
    # base accumulated: pair start/pub in order
    accs = []
    cur = None
    for e in acc:
        if e["ev"] == "start":
            cur = dict(e); accs.append(cur)
        elif cur is not None:
            cur.update(pub_ms_after_final=e["ms"], transport=e["transport"], pushed=e["filters"], targets=e["targets"],
                       ms_start_to_pub=round((e["t"] - cur["t"]) * 1e3, 1)); cur = None
    return dict(joins=list(joins.values()), filters=filters, pubs=pubs, acc=accs, declines=dict(declines), kept=dict(kept),
                ckpt=ckpt, wired=wired, ops={str(k): v for k, v in ops.items()}, doms=doms,
                tip_unattached=pend_tip)


def main():
    label_dir, out = sys.argv[1], sys.argv[2]
    res = {}
    for q in range(1, 23):
        fs = sorted(glob.glob(f"{label_dir}/tpch_*_q{q}/log_dir/sirius_*.log"))
        if not fs: continue
        raw = open(fs[-1], errors="replace").read().splitlines()
        wins = windows(raw)
        if not wins: res[q] = dict(error="no window"); continue
        ws, we, ok = wins[-1]
        i = ws
        while i > 0 and "QueryBegin: instance" not in raw[i]: i -= 1
        r = parse(raw[i:we + 1])
        r["ok"] = ok; r["windows"] = len(wins)
        r["window_ms"] = round((ts(raw[we]) - ts(raw[ws])) * 1e3, 1)
        r["fallback_lines"] = sum(1 for l in raw if re.search(r"fallback", l, re.I))
        r["error_lines"] = sum(1 for l in raw if re.search(r"Error in Sirius|bad_alloc|out of memory|illegal memory", l))
        r["oom_retries"] = sum(1 for l in raw if "reschedule (retry" in l)
        r["pinned_assigned"] = sum(1 for l in raw if "assigned pinned entry" in l)
        if len(wins) > 1:
            ws0, we0, _ = wins[0]
            r0 = parse(raw[max(0, ws0 - 60):we0 + 1])
            sig = lambda rr: sorted((f["join"] or -1, f["key"], f["kind"], f["size"], f["pushed"]) for f in rr["filters"])
            r["iter0_same_filters"] = sig(r0) == sig(r)
        res[q] = r
    json.dump(res, open(out, "w"), indent=1)
    for q, r in res.items():
        if "error" in r: print(q, r); continue
        kinds = defaultdict(int)
        for f in r["filters"]: kinds[f["kind"]] += 1
        print(f"q{q}: ok={r['ok']} win={r['window_ms']}ms joins={len(r['joins'])} wired={len(r['wired'])} filters={dict(kinds)} "
              f"pubs={len(r['pubs'])} acc={len(r['acc'])} declines={sum(r['declines'].values())} fb={r['fallback_lines']} err={r['error_lines']} "
              f"oom={r['oom_retries']} pin={r['pinned_assigned']} iter0same={r.get('iter0_same_filters')}")


if __name__ == "__main__":
    main()
