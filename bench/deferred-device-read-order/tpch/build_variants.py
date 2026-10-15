#!/usr/bin/env python3
"""Build isolated full SQL runners and checked diagnostic-only source variants."""
import argparse
import difflib
import hashlib
import importlib.util
import json
from pathlib import Path
import shlex
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError(
            f"Expected one source anchor: {old!r}, found {text.count(old)}"
        )
    return text.replace(old, new)


def diagnostic_batch(original):
    original = replace_once(
        original,
        '#include "io/deferred_device_batch.hpp"',
        '#include "io/deferred_device_batch.hpp"\n#include "diagnostics.hpp"',
    )
    entry = r"""
  order_diagnostic::await_scope benchmark_scope{this};
  order_diagnostic::snapshot benchmark_own;
  bool benchmark_open = false;
  {
    std::scoped_lock lock(_mutex);
    benchmark_open = _state == state::open;
    if (benchmark_open) benchmark_own = order_diagnostic::observe(_owner, _slices, _stream, _device);
  }
  if (benchmark_open) {
    auto registry = _registry.lock();
    auto const benchmark_peers = order_diagnostic::peers(registry.get(), _stream, this);
    if (benchmark_peers.first) {
      benchmark_scope.adopt_cohort(benchmark_peers.generation);
      std::size_t benchmark_open_siblings = 0;
      bool benchmark_sibling_pending = false;
      for (auto const& weak : benchmark_peers.entries) {
        if (auto sibling = weak.lock(); sibling && sibling.get() != this) {
          std::scoped_lock sibling_lock(sibling->_mutex);
          if (sibling->_state != state::open) continue;
          ++benchmark_open_siblings;
          auto const observed = order_diagnostic::observe(sibling->_owner, sibling->_slices,
                                                          sibling->_stream, sibling->_device);
          benchmark_sibling_pending |= observed.inflight && observed.loading_overlap;
          benchmark_scope.sibling(observed);
        }
      }
      benchmark_scope.entry(benchmark_own, benchmark_open_siblings, benchmark_sibling_pending);
    } else {
      benchmark_scope.unclaimed();
    }
  } else {
    benchmark_scope.already_issued();
  }
"""
    original = replace_once(
        original,
        "std::size_t deferred_device_batch::await(std::size_t bytes)\n{",
        "std::size_t deferred_device_batch::await(std::size_t bytes)\n{" + entry,
    )
    original = replace_once(
        original,
        "void deferred_device_batch::issue() noexcept\n{",
        "void deferred_device_batch::issue() noexcept\n{\n  order_diagnostic::issue(this, order_diagnostic::observe(_owner, _slices, _stream, _device));",
    )
    original = replace_once(
        original,
        "  open.push_back(batch);",
        "  open.push_back(batch);\n  order_diagnostic::registered(this, batch);",
    )
    original = replace_once(
        original,
        "  auto batches = std::move(it->second);",
        "  auto batches = std::move(it->second);\n  order_diagnostic::extracted(this, stream);",
    )
    return original


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--build", type=Path, default=Path("build/release"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--prepare-only", action="store_true")
    args = parser.parse_args()
    root, build, output = (
        args.root.resolve(),
        args.build.resolve(),
        args.output.resolve(),
    )
    output.mkdir(parents=True, exist_ok=True)
    authored = root / "bench/deferred-device-read-order/tpch"
    commands = []

    def run(command, cwd=build):
        commands.append({"cwd": str(cwd), "argv": command})
        (output / "build_commands.json").write_text(
            json.dumps(commands, indent=2) + "\n"
        )
        print(shlex.join(command), flush=True)
        subprocess.run(command, cwd=cwd, check=True)

    originals = {
        name: (root / path).read_text()
        for name, path in {
            "batch": "src/io/deferred_device_batch.cpp",
            "datasource": "src/io/sirius_datasource.cpp",
            "cache": "src/io/cache/prefetching_cache.cpp",
        }.items()
    }
    generated = {}

    def save(name, text, reference):
        path = output / (name + ".cpp")
        path.write_text(text)
        (output / (name + ".diff")).write_text(
            "".join(
                difflib.unified_diff(
                    reference.splitlines(True),
                    text.splitlines(True),
                    fromfile="production.cpp",
                    tofile=path.name,
                )
            )
        )
        generated[name] = path

    specification = importlib.util.spec_from_file_location(
        "deferred_order_prepare", authored.parent / "prepare.py"
    )
    preparer = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(preparer)
    production_policy, policies = preparer.named_policy_sources(originals["batch"])
    for name, policy in [("siblings", "siblings_first"), ("own", "own_first")]:
        save("batch_" + name, policies[policy], originals["batch"])
        save(
            "batch_" + name + "_diagnostic",
            diagnostic_batch(policies[policy]),
            policies[policy],
        )
    datasource = replace_once(
        originals["datasource"],
        '#include "io/sirius_datasource.hpp"',
        '#include "io/sirius_datasource.hpp"\n#include "diagnostics.hpp"',
    )
    datasource = replace_once(
        datasource,
        "  : _io_ctx(std::move(io_ctx)), _io_object(std::move(io_obj))\n{\n}",
        "  : _io_ctx(std::move(io_ctx)), _io_object(std::move(io_obj))\n{\n  order_diagnostic::track(this, &_cache_handle);\n}",
    )
    datasource = replace_once(
        datasource,
        "sirius_datasource::~sirius_datasource()\n{",
        "sirius_datasource::~sirius_datasource()\n{\n  order_diagnostic::untrack(this);",
    )
    save("datasource_diagnostic", datasource, originals["datasource"])
    cache = replace_once(
        originals["cache"],
        '#include "io/cache/prefetching_cache.hpp"',
        '#include "io/cache/prefetching_cache.hpp"\n#include "diagnostics.hpp"',
    )
    cache = replace_once(
        cache,
        "      std::ignore = handle->wait_until_ready();\n      return;",
        "      auto const benchmark_wait_begin = order_diagnostic::now_ns();\n"
        "      std::ignore = handle->wait_until_ready();\n"
        "      order_diagnostic::waited(benchmark_wait_begin, order_diagnostic::now_ns(), obj.object_path().c_str());\n"
        "      return;",
    )
    save("cache_diagnostic", cache, originals["cache"])
    compile_database = json.loads((build / "compile_commands.json").read_text())
    template = next(
        row
        for row in compile_database
        if row["file"].endswith("/test/telemetry/nvtx_startup.cpp")
    )
    compile_command = shlex.split(template["command"])

    def compile_source(path, name):
        obj = output / (name + ".o")
        command = compile_command.copy()
        command[command.index("-o") + 1] = str(obj)
        command[command.index("-c") + 1] = str(path)
        command.insert(1, "-I" + str(authored))
        # Isolated compilation must not overwrite the template target's depfile.
        for flag in ["-MF", "-MT"]:
            if flag in command:
                position = command.index(flag)
                del command[position : position + 2]
        command = [part for part in command if part not in ["-MD", "-MMD"]]
        if not args.prepare_only:
            run(command)
        return obj

    raw = subprocess.check_output(
        ["ninja", "-C", str(build), "-t", "commands", "test/cpp/sirius_nvtx_startup"],
        text=True,
    )
    line = next(
        line
        for line in reversed(raw.splitlines())
        if " -o test/cpp/sirius_nvtx_startup " in line
    )
    link = shlex.split(line)
    if link[:2] == [":", "&&"]:
        link = link[2:]
    if link[-2:] == ["&&", ":"]:
        link = link[:-2]
    if "&&" in link:
        raise RuntimeError("Unexpected link shell operations")
    link = [arg for arg in link if not arg.startswith("-Wl,--dependency-file=")]
    original_main = (
        "CMakeFiles/sirius_nvtx_startup.dir/test/telemetry/nvtx_startup.cpp.o"
    )
    objects = {"runner": compile_source(authored / "sql_runner.cpp", "runner")}
    for name in [
        "batch_siblings",
        "batch_own",
        "batch_siblings_diagnostic",
        "batch_own_diagnostic",
        "datasource_diagnostic",
        "cache_diagnostic",
    ]:
        objects[name] = compile_source(generated[name], name)
    objects["diagnostics"] = compile_source(authored / "diagnostics.cpp", "diagnostics")
    binaries = {}
    for name, additional in {
        "siblings_first": ["batch_siblings"],
        "own_first": ["batch_own"],
        "siblings_first_diagnostic": [
            "batch_siblings_diagnostic",
            "datasource_diagnostic",
            "cache_diagnostic",
            "diagnostics",
        ],
        "own_first_diagnostic": [
            "batch_own_diagnostic",
            "datasource_diagnostic",
            "cache_diagnostic",
            "diagnostics",
        ],
    }.items():
        command = link.copy()
        position = command.index(original_main)
        command[position : position + 1] = [str(objects["runner"])] + [
            str(objects[item]) for item in additional
        ]
        binary = output / ("tpch_" + name)
        command[command.index("-o") + 1] = str(binary)
        command.insert(1, "-Wl,-Map," + str(output / (name + ".map")))
        if not args.prepare_only:
            run(command)
        binaries[name] = str(binary)
    source_paths = [
        root / "src/io/deferred_device_batch.cpp",
        root / "src/io/sirius_datasource.cpp",
        root / "src/io/cache/prefetching_cache.cpp",
        *authored.glob("*.cpp"),
        *authored.glob("*.hpp"),
        authored / "build_variants.py",
        authored.parent / "prepare.py",
    ]
    manifest = {
        "prepare_only": args.prepare_only,
        "production_policy": production_policy,
        "production_sources": {str(p): digest(p) for p in source_paths},
        "generated_sources": {str(p): digest(p) for p in generated.values()},
        "core_archive": {
            "path": str(build / "libsirius_core.a"),
            "sha256": digest(build / "libsirius_core.a"),
        },
        "binaries": {
            name: {
                "path": path,
                "sha256": digest(Path(path)) if not args.prepare_only else None,
            }
            for name, path in binaries.items()
        },
    }
    (output / "build_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(
        (
            "Sources prepared; binaries not built:"
            if args.prepare_only
            else "Variants ready:"
        ),
        json.dumps(binaries),
        flush=True,
    )


if __name__ == "__main__":
    main()
