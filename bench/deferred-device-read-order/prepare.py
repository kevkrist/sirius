#!/usr/bin/env python3
"""Prepare auditable benchmark-only link symbols and issuance-order variants."""
import argparse
import difflib
import hashlib
import json
from pathlib import Path
import subprocess


def named_policy_sources(original):
    """Return checked policy sources, preserving the production variant verbatim."""
    signature = "std::size_t deferred_device_batch::await(std::size_t bytes)"
    terminator = "\nvoid deferred_device_batch::issue_if_open()"
    if original.count(signature) != 1 or original.count(terminator) != 1:
        raise RuntimeError("Unexpected await boundaries; review transformation")
    begin = original.index(signature)
    end = original.index(terminator, begin)
    body = original[begin:end]
    siblings = """  if (auto registry = _registry.lock()) {
    for (auto const& weak : registry->take_batches(_stream)) {
      if (auto sibling = weak.lock(); sibling && sibling.get() != this) {
        sibling->issue_if_open();
      }
    }
  }
"""
    own = "  issue_if_open();\n"
    completion = "  std::shared_future<std::size_t> completion;"
    wait = "  std::ignore = completion.get();"
    for anchor in [
        siblings,
        own,
        completion,
        wait,
        "    completion = _completion;\n  }",
    ]:
        if body.count(anchor) != 1:
            raise RuntimeError(f"Unexpected await anchor: {anchor!r}")
    if "if (_state == state::open)" in body:
        raise RuntimeError("Own issuance must precede terminal-state checking")
    start = body.index(own)
    sibling_start = body.index(siblings)
    production_policy = "own_first" if start < sibling_start else "siblings_first"
    first_stop = (
        start + len(own)
        if production_policy == "own_first"
        else sibling_start + len(siblings)
    )
    second_start = sibling_start if production_policy == "own_first" else start
    second_stop = (
        sibling_start + len(siblings)
        if production_policy == "own_first"
        else start + len(own)
    )
    if (
        body[first_stop:second_start].strip()
        or body[second_stop : body.index(completion)].strip()
    ):
        raise RuntimeError(
            "Unexpected statements between issuance and terminal-state checking"
        )
    if (
        not second_stop
        <= body.index(completion)
        < body.index("    completion = _completion;")
        < body.index(wait)
    ):
        raise RuntimeError("Unexpected issuance/completion order")
    alternate = body[:start] + body[start + len(own) :]
    anchor = siblings if production_policy == "siblings_first" else completion
    alternate = alternate.replace(anchor, own + anchor)
    alternative_policy = (
        "own_first" if production_policy == "siblings_first" else "siblings_first"
    )
    return production_policy, {
        production_policy: original,
        alternative_policy: original[:begin] + alternate + original[end:],
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cudf", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    path = args.root / "src/io/deferred_device_batch.cpp"
    original = path.read_text()
    production_policy, variants = named_policy_sources(original)
    # Keep this snapshot/key for the runner's existing stale-source check.
    (args.output / "deferred_device_batch_baseline.cpp").write_text(original)
    for policy, source in variants.items():
        (args.output / f"deferred_device_batch_{policy}.cpp").write_text(source)
        (args.output / (policy.replace("_", "-") + ".diff")).write_text(
            "".join(
                difflib.unified_diff(
                    original.splitlines(True),
                    source.splitlines(True),
                    fromfile=str(path),
                    tofile=f"{policy}.cpp",
                )
            )
        )
    symbols = subprocess.check_output(["nm", "-D", str(args.cudf)], text=True)
    matches = [
        line.split()[-1]
        for line in symbols.splitlines()
        if "memcpy_batch_asyncE" in line and " T " in line
    ]
    if len(matches) != 1:
        raise RuntimeError(
            f"Expected exactly one cudf memcpy batch symbol, got {matches}"
        )
    symbol = matches[0]
    (args.output / "copy_symbols.hpp").write_text(
        '#pragma once\n#define SIRIUS_BENCH_REAL_COPY "__real_' + symbol + '"\n'
        '#define SIRIUS_BENCH_WRAPPED_COPY "__wrap_' + symbol + '"\n'
    )
    (args.output / "wrap.rsp").write_text("--wrap=" + symbol + "\n")
    (args.output / "manifest.json").write_text(
        json.dumps(
            {
                "baseline_source": str(path),
                "baseline_sha256": hashlib.sha256(original.encode()).hexdigest(),
                "production_policy": production_policy,
                "policy_sha256": {
                    policy: hashlib.sha256(source.encode()).hexdigest()
                    for policy, source in variants.items()
                },
                "cudf_library": str(args.cudf),
                "wrapped_symbol": symbol,
            },
            indent=2,
        )
        + "\n"
    )


if __name__ == "__main__":
    main()
