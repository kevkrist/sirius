#!/usr/bin/env python3
"""sites_compact.py <esites json> [min_window_ms=5]: per site and arm: CONCAT Preparing median, build lands (CONCAT end - pub),
scan ends (scan end - pub), build tail = max(0, lands - scan end), scan chain, site span = max(end) - pub, gain vs tip."""
import json, sys
r = json.load(open(sys.argv[1])); mw = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
print("| q | site | arm | CONCAT prep med | build lands | scan ends | tail | scan chain | site span | span gain |"); print("|" + "---|" * 10)
tails = {}; gains = {}
for q, d in r.items():
    sites = d["_sites"]; arms = [a for a in d if a != "_sites"]
    for si, site in enumerate(sites):
        t = d[arms[0]]["sites"].get(str(si))
        if not t or (t["c_end"] - t["pub"]) < mw: continue
        for a in arms:
            m = d[a]["sites"].get(str(si))
            if not m: continue
            lands = m["c_end"] - m["pub"]; send = m["s_end"] - m["pub"]; tail = max(0.0, lands - send)
            gain = t["site_span"] - m["site_span"]
            tails[a] = tails.get(a, 0) + tail; gains[a] = gains.get(a, 0) + gain
            lab = f"{site['c'].split(' ')[0]} / {site['s'].split(' ')[0]}{'' if site['df'] else ' (no DF)'}"
            print(f"| {q} | {lab} | {a} | {m['c_prep_med']:.0f} | {lands:.0f} | {send:.0f} | {tail:.0f} | {m['s_chain']:.0f} | {m['site_span']:.0f} | {gain:+.0f} |")
print("\nsum of build tails per arm: " + ", ".join(f"{a} {v:.0f}" for a, v in tails.items()))
print("sum of site-span gains per arm: " + ", ".join(f"{a} {v:.0f}" for a, v in gains.items()))
