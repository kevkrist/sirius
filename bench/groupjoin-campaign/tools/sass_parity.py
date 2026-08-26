#!/usr/bin/env python3
"""SASS parity checker for the dense-count-join -> group-join refactor.

Parses two `cuobjdump --dump-sass` dumps, selects every function originating in the
dense_count_join_impl.cu / group_join_impl.cu translation unit (direct kernels plus CUB/thrust
kernels parameterized on its anonymous-namespace functors), maps old -> new by demangled "role"
(normalizing the anon-namespace hash and the count_bundle wrapper), and diffs instruction
streams per (arch, role).

Flags:
  --allow-new-roles   roles present only in the candidate are reported as new kernels instead of
                      failing (used when the candidate adds whitelisted instantiations, e.g. the
                      PR-2 value bundles; baseline roles must still be instruction-identical)
  --self-test         mutation control folded into the harness: verifies the checker (a) passes a
                      baseline-vs-baseline comparison and (b) fails when one instruction of one
                      kernel is mutated, proving the diff actually looks at instruction streams
"""

import copy
import re
import subprocess
import sys
from collections import defaultdict

OLD_MARK = "dense_count_join_impl"
NEW_MARK = "group_join_impl"

ARCH_RE = re.compile(r"^arch = (\S+)")
FUNC_RE = re.compile(r"^\s*Function : (\S+)")
INSTR_RE = re.compile(r"^\s*/\*[0-9a-fA-F]+\*/\s*(.*?)\s*(?:/\* 0x[0-9a-fA-F]+ \*/)?\s*$")
CONT_RE = re.compile(r"^\s*/\* 0x[0-9a-fA-F]+ \*/\s*$")


def parse(path, marker):
    """Return {(arch, mangled): [instruction, ...]} for functions containing marker."""
    funcs = {}
    arch = None
    cur = None
    with open(path) as f:
        for line in f:
            m = ARCH_RE.match(line)
            if m:
                arch = m.group(1)
                cur = None
                continue
            m = FUNC_RE.match(line)
            if m:
                name = m.group(1)
                if marker in name:
                    cur = []
                    key = (arch, name)
                    if key in funcs:
                        raise SystemExit(f"duplicate function {key}")
                    funcs[key] = cur
                else:
                    cur = None
                continue
            if cur is None:
                continue
            if CONT_RE.match(line):
                continue
            m = INSTR_RE.match(line)
            if m and m.group(1):
                text = re.sub(r"\s+", " ", m.group(1)).strip()
                if text:
                    cur.append(text)
    return funcs


def demangle_many(names):
    strip = {}
    for n in names:
        s = n
        m = re.match(r"^__nv_static_\d+__.*?(_ZN.*)$", s)
        if m:
            s = m.group(1)
        strip[n] = s
    proc = subprocess.run(
        ["c++filt"], input="\n".join(strip[n] for n in names), capture_output=True, text=True
    )
    out = proc.stdout.splitlines()
    result = dict(zip(names, out))
    for n, d in result.items():
        if d.startswith("_Z") or d.startswith("__nv_static"):
            raise SystemExit(f"demangle failed for {n}")
    return result


def role_of(demangled):
    r = re.sub(r"_GLOBAL__N__\w+", "_ANON_", demangled)
    # Fold the count-bundle tag back to its count type so old CountT-parameterized names and new
    # Bundle-parameterized names produce the same role. ::count_type first, then the bare tag.
    bundle = r"sirius::op::groupjoin::count_bundle<unsigned (int|long)\s*>"
    r = re.sub(bundle + r"::count_type", r"unsigned \1", r)
    r = re.sub(bundle, r"unsigned \1", r)
    r = re.sub(r"\s+", " ", r)
    r = re.sub(r"\s+>", ">", r)
    return r


def build_roles(funcs, demangled, label):
    roles = defaultdict(dict)  # arch -> role -> (name, instrs)
    for (arch, name), ins in funcs.items():
        role = role_of(demangled[name])
        if role in roles[arch]:
            raise SystemExit(f"{label} role collision {arch}: {role}")
        roles[arch][role] = (name, ins)
    return roles


def compare(old, new, old_dem, new_dem, allow_new_roles):
    """Return (verdicts, diffs, new_roles, archs, old_roles, new_roles_map)."""
    old_roles = build_roles(old, old_dem, "old")
    new_roles = build_roles(new, new_dem, "new")
    archs = sorted(set(old_roles) | set(new_roles))

    baseline_roles = sorted({r for a in archs for r in old_roles[a]})
    candidate_only = sorted(
        {r for a in archs for r in new_roles[a]} - set(baseline_roles)
    )

    verdicts = {}
    diffs = []
    for role in baseline_roles:
        per_arch = []
        for arch in archs:
            o = old_roles[arch].get(role)
            n = new_roles[arch].get(role)
            if o is None or n is None:
                per_arch.append((arch, "MISSING-" + ("OLD" if o is None else "NEW")))
                continue
            if o[1] == n[1]:
                per_arch.append((arch, "IDENTICAL"))
            else:
                per_arch.append((arch, f"DIFF ({len(o[1])} vs {len(n[1])} instrs)"))
                import difflib

                d = list(difflib.unified_diff(o[1], n[1], lineterm="", n=1))
                diffs.append((role, arch, d[:400]))
        states = {s for (_, s) in per_arch}
        verdicts[role] = (
            "IDENTICAL (all archs)"
            if states == {"IDENTICAL"}
            else "; ".join(f"{a}: {s}" for (a, s) in per_arch if s != "IDENTICAL") or "IDENTICAL"
        )

    if not allow_new_roles:
        for role in candidate_only:
            verdicts[role] = "MISSING-OLD (new kernel; rerun with --allow-new-roles if intended)"
    return verdicts, diffs, candidate_only, archs, old_roles, new_roles


def self_test(old_path, old_mark):
    """Mutation control: baseline-vs-baseline must pass; a single-instruction mutation must fail."""
    base = parse(old_path, old_mark)
    if not base:
        print("self-test: no baseline kernels parsed")
        return 1
    dem = demangle_many(sorted({n for (_, n) in base}))

    verdicts, diffs, new_only, _, _, _ = compare(base, base, dem, dem, allow_new_roles=False)
    clean_ok = all(v.startswith("IDENTICAL") for v in verdicts.values()) and not new_only
    print(f"self-test [control A] baseline-vs-baseline: {'PASS' if clean_ok else 'FAIL'}")

    mutated = copy.deepcopy(base)
    victim = None
    for key, instrs in sorted(mutated.items()):
        if instrs:
            victim = key
            instrs[len(instrs) // 2] = "SELF_TEST_MUTATED_INSTRUCTION ;"
            break
    if victim is None:
        print("self-test: no kernel with instructions to mutate")
        return 1
    verdicts, diffs, _, _, _, _ = compare(base, mutated, dem, dem, allow_new_roles=False)
    detected = any(not v.startswith("IDENTICAL") for v in verdicts.values())
    print(
        f"self-test [control B] mutated {victim[1][:60]}... on {victim[0]}: "
        f"{'DETECTED (PASS)' if detected else 'MISSED (FAIL)'}"
    )
    return 0 if (clean_ok and detected) else 1


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = {a for a in sys.argv[1:] if a.startswith("--")}
    allow_new_roles = "--allow-new-roles" in flags

    if "--self-test" in flags:
        old_path = args[0]
        old_mark = args[1] if len(args) > 1 else OLD_MARK
        return self_test(old_path, old_mark)

    old_path, new_path, report_path = args[0:3]
    old_mark = args[3] if len(args) > 3 else OLD_MARK
    new_mark = args[4] if len(args) > 4 else NEW_MARK
    old = parse(old_path, old_mark)
    new = parse(new_path, new_mark)

    old_dem = demangle_many(sorted({n for (_, n) in old}))
    new_dem = demangle_many(sorted({n for (_, n) in new}))

    verdicts, diffs, new_only, archs, old_roles, new_roles = compare(
        old, new, old_dem, new_dem, allow_new_roles
    )

    lines = []
    lines.append("# SASS parity report: dense_count_join_impl.cu -> group_join_impl.cu\n")
    lines.append(f"- baseline: `{old_path}`")
    lines.append(f"- candidate: `{new_path}`")
    lines.append(f"- architectures: {', '.join(archs)}")
    lines.append(f"- allow-new-roles: {allow_new_roles}\n")
    lines.append(
        f"Kernels per arch: baseline {len(old_roles[archs[0]])}, candidate "
        f"{len(new_roles[archs[0]])}, baseline roles compared {len(verdicts)}\n"
    )

    lines.append("## Verdict per baseline kernel role\n")
    lines.append("| # | kernel role (demangled, normalized) | verdict |")
    lines.append("|---|---|---|")
    for i, role in enumerate(sorted(verdicts), 1):
        lines.append(f"| {i} | `{role}` | {verdicts[role]} |")

    if new_only:
        lines.append("\n## New kernels (candidate only)\n")
        for i, role in enumerate(sorted(new_only), 1):
            lines.append(f"{i}. `{role}`")

    if diffs:
        lines.append("\n## Instruction diffs\n")
        for role, arch, d in diffs:
            lines.append(f"### {role} [{arch}]\n")
            lines.append("```diff")
            lines.extend(d)
            lines.append("```")

    ok = all(v.startswith("IDENTICAL") for v in verdicts.values()) and len(verdicts) > 0
    lines.append("\n## Overall\n")
    lines.append(
        "PASS: every baseline kernel role is instruction-identical."
        if ok
        else "FAIL: differences found (see above)."
    )

    with open(report_path, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(
        f"baseline_roles={len(verdicts)} new_roles={len(new_only)} archs={len(archs)} -> "
        f"{'PASS' if ok else 'FAIL'}"
    )
    print(f"report: {report_path}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
