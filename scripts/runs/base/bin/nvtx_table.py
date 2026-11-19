#!/usr/bin/env python3
"""Base vs tip DF construction cost per hot iteration from nsys NVTX (nvtx_df.json of each label)."""
import json
R = "/opt/dlami/nvme/dfperf/runs/base"
b = json.load(open(f"{R}/nsys/nvtx_df.json")); t = json.load(open(f"{R}/nsys_tip/nvtx_df.json"))
mc = lambda d, kinds: sum(d.get("mc", {}).get(k, [0, 0, 0])[1] for k in kinds)
g = lambda D, k, f: D.get(k, {}).get(f, 0)
print("| q | BASE one-shot push_build_side: n, wall ms (iter1, iter2) | of which replicate_devices wall | build kernels GPU ms | BASE accum@PARTITION: contribute GPU ms / finish wall ms | BASE total wall | BASE explicit host MB / PtoP-API MB | TIP begin wall ms (memset MB) | TIP contribute: n, GPU ms (bloom+stage) | TIP finish wall (accum::finish + stage::gather + bounds + replicate) | TIP total wall | TIP explicit host MB / PtoP-API MB | TIP - BASE wall ms |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
tb = tt = 0
for q in sorted(b, key=int):
    B = b[q]; T = t.get(q, {})
    p = B.get("push_build_side", {}); wi = p.get("wall_iters", [0, 0, 0])
    bg = sum(g(B, k, "gpu") for k in ("build_bloom", "build_in_list", "build_small_in_list", "build_zone_map"))
    bacc = g(B, "accum::begin", "wall") + g(B, "accum::contribute", "wall") + g(B, "accum::finish", "wall")
    bw = p.get("wall", 0) + bacc
    bh = mc(p, ("HtoD", "DtoH")) + mc(B.get("accum::finish", {}), ("HtoD", "DtoH")); bp = mc(p, ("PtoP",)) + mc(B.get("accum::finish", {}), ("PtoP",))
    fin_keys = ("accum::finish", "stage::gather", "bounds::finish", "replicate_devices")
    tfin = sum(g(T, k, "wall") for k in fin_keys)
    tcg = g(T, "accum::contribute", "gpu") + g(T, "stage::contribute", "gpu") + g(T, "bounds::contribute", "gpu")
    tcw = g(T, "accum::contribute", "wall") + g(T, "stage::contribute", "wall") + g(T, "bounds::contribute", "wall")
    tw = g(T, "construction::begin", "wall") + tcw + tfin
    th = sum(mc(T.get(k, {}), ("HtoD", "DtoH")) for k in fin_keys); tp = sum(mc(T.get(k, {}), ("PtoP",)) for k in fin_keys)
    tb += bw; tt += tw
    print(f"| {q} | {p.get('n', 0)}, {p.get('wall', 0):.1f} ({wi[1]:.1f}, {wi[2]:.1f}) | {g(B, 'replicate_devices', 'wall'):.1f} | {bg:.2f} | {g(B, 'accum::contribute', 'gpu'):.1f} / {g(B, 'accum::finish', 'wall'):.1f} | {bw:.1f} | {bh:.0f} / {bp:.0f} | "
          f"{g(T, 'construction::begin', 'wall'):.2f} ({g(T, 'construction::begin', 'memset_mb'):.0f}) | {g(T, 'accum::contribute', 'n') + g(T, 'stage::contribute', 'n'):.0f}, {tcg:.2f} | {tfin:.1f} | {tw:.1f} | {th:.0f} / {tp:.0f} | {tw - bw:+.1f} |")
print(f"\nSum over the 14 captured queries, per hot iteration: base {tb:.1f} ms, tip {tt:.1f} ms (same-thread wall inside DF construction ranges).")
