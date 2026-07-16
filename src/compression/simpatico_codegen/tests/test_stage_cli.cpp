// SPDX-License-Identifier: Apache-2.0
//
// End-to-end smoke test for the `simpatico stage` CLI subcommand. Runs the built
// `simpatico` binary against a generated input and exercises the wiring the
// library-level staged tests cannot reach: argument parsing, the
// consume-before-stage ordering, candidate sizing, the --min-ratio accept/reject
// gate, the on-accept .hpln write, and verify-by-reload.
//
//   * --min-ratio 0        forces ACCEPT regardless of ratio -> writes + round-trips.
//   * --min-ratio 1e9       forces REJECT                    -> original preserved.
//
// Both decisions must exit 0 (each is a valid outcome). The CLI binary path is
// injected by CMake as SIMPATICO_CLI.

#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#ifndef SIMPATICO_CLI
#error "SIMPATICO_CLI (path to the built simpatico binary) must be defined by CMake"
#endif

namespace fs = std::filesystem;

namespace {

// The per-run temp dir, removed by an atexit handler so a fail()/std::exit(1) does not leak
// fixtures (std::exit does not unwind, so a scope guard would not fire — but atexit does).
fs::path g_temp_dir;
void cleanup_temp_dir()
{
  if (g_temp_dir.empty()) return;
  std::error_code ec;
  fs::remove_all(g_temp_dir, ec);
}

[[noreturn]] void fail(std::string const& message)
{
  std::fprintf(stderr, "test_stage_cli: FAIL: %s\n", message.c_str());
  std::exit(1);
}

// Single-quote a path for /bin/sh so a space in the build or temp directory cannot
// mis-tokenize the command. (Build/temp paths never contain single quotes.)
std::string sh_quote(std::string const& s) { return "'" + s + "'"; }

// Run a shell command and return the child's exit code (or -1 if it did not exit normally).
int run(std::string const& command)
{
  int const status = std::system(command.c_str());
  if (status == -1) fail("failed to launch: " + command);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

}  // namespace

int main()
{
  auto const dir =
    fs::temp_directory_path() / ("simpatico-stage-cli-" + std::to_string(::getpid()));
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  g_temp_dir = dir;
  std::atexit(cleanup_temp_dir);

  // A small, deterministic single-column int32 input as a flat binary file.
  auto const input = dir / "input.i32";
  {
    std::vector<std::int32_t> data(4096);
    for (std::size_t i = 0; i < data.size(); ++i)
      data[i] = static_cast<std::int32_t>((i * 17 + 7) % 1000);
    std::ofstream out(input, std::ios::binary);
    out.write(reinterpret_cast<char const*>(data.data()),
              static_cast<std::streamsize>(data.size() * sizeof(std::int32_t)));
    if (!out) fail("could not write the input fixture");
  }

  // Single-column identity plan — always valid for a one-column table.
  auto const plan = dir / "plan.dsl";
  {
    std::ofstream out(plan);
    out << "input -> identity\n";
    if (!out) fail("could not write the plan fixture");
  }

  auto const hpln       = dir / "out.hpln";
  std::string const cli = SIMPATICO_CLI;
  std::string const base = sh_quote(cli) + " stage --input " + sh_quote(input.string()) +
                           " --format binary --dtype i32 --plan " + sh_quote(plan.string());

  // ACCEPT path: keep the candidate regardless of ratio, write it, and verify the round-trip.
  if (int const rc = run(base + " --min-ratio 0 --out " + sh_quote(hpln.string()) + " --verify");
      rc != 0)
    fail("accept path exited with " + std::to_string(rc));
  if (!fs::exists(hpln)) fail("accept path did not write the .hpln");

  // REJECT path: an unreachable ratio forces reject; the original must be preserved and exit 0.
  if (int const rc = run(base + " --min-ratio 1000000 --verify"); rc != 0)
    fail("reject path exited with " + std::to_string(rc));

  std::printf("test_stage_cli: PASS\n");
  return 0;
}
