#!/usr/bin/env python3
"""Controlled native Windows timings and separate gprof runs.

All timed servers use the same uninstrumented baseline client. Rotating run
order, CPU affinity, raw samples, CPU times, and file hashes make build-mode
comparisons independent of encrypted traffic and profiling overhead.
"""
import argparse
import csv
import ctypes
from ctypes import wintypes
import hashlib
import itertools
import json
import os
from pathlib import Path
import platform
import re
import statistics
import subprocess
import sys
import time


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save_json(path, data):
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


class NativeProcess:
    """Keep the process handle alive for identity, CPU accounting and cleanup."""
    kernel = ctypes.WinDLL("kernel32", use_last_error=True) if os.name == "nt" else None
    if kernel:
        kernel.SetProcessAffinityMask.argtypes = [wintypes.HANDLE, ctypes.c_size_t]
        kernel.SetProcessAffinityMask.restype = wintypes.BOOL
        kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
        kernel.GetProcessTimes.restype = wintypes.BOOL

    def __init__(self, command, directory, prefix, affinity):
        self.stdout = (directory / (prefix + ".stdout")).open("wb")
        self.stderr = (directory / (prefix + ".stderr")).open("wb")
        save_json(directory / (prefix + ".command.json"), command)
        self.process = subprocess.Popen(command, cwd=directory, stdin=subprocess.DEVNULL,
                                        stdout=self.stdout, stderr=self.stderr,
                                        creationflags=subprocess.CREATE_NO_WINDOW)
        self.started = time.perf_counter()
        if not self.kernel.SetProcessAffinityMask(int(self.process._handle), affinity):
            error = ctypes.get_last_error()
            self.close()
            raise ctypes.WinError(error)

    def cpu(self):
        creation, exit_time, kernel, user = (wintypes.FILETIME() for _ in range(4))
        if not self.kernel.GetProcessTimes(int(self.process._handle), ctypes.byref(creation),
                                          ctypes.byref(exit_time), ctypes.byref(kernel), ctypes.byref(user)):
            raise ctypes.WinError(ctypes.get_last_error())
        def milliseconds(value):
            return ((value.dwHighDateTime << 32) | value.dwLowDateTime) / 10000
        return {"user_ms": milliseconds(user), "kernel_ms": milliseconds(kernel)}

    def wait(self, timeout=300):
        code = self.process.wait(timeout=timeout)
        self.stdout.flush()
        self.stderr.flush()
        if code:
            raise RuntimeError(f"Process exited with {code}: {self.process.args[0]}")

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=30)
        self.stdout.close()
        self.stderr.close()


def cli_run(cli, port, *args):
    return subprocess.check_output([str(cli), "--raw", "-h", "127.0.0.1", "-p", str(port), *args],
                                   timeout=20, text=True, creationflags=subprocess.CREATE_NO_WINDOW).strip()


def run_variant(variant, client, output, round_number, requests, profile, port, client_affinity):
    name = variant["name"]
    directory = output / f"r{round_number:02d}-{name}"
    directory.mkdir()
    server = Path(variant["directory"]) / "redis-server.exe"
    cli = client / "redis-cli.exe"
    benchmark = client / "redis-benchmark.exe"
    config = directory / "server.conf"
    config.write_text(f'bind 127.0.0.1\nport {port}\nprotected-mode yes\nsave ""\n'
                      'appendonly no\nlogfile ""\nloglevel warning\n'
                      'io-threads 1\nrepl-compression 0\n', encoding="utf-8")
    service = NativeProcess([str(server), str(config)], directory, "server", 1)
    samples = []
    try:
        for _ in range(200):
            if service.process.poll() is not None:
                raise RuntimeError(f"{name} exited during startup")
            try:
                if cli_run(cli, port, "PING") == "PONG":
                    break
            except (subprocess.SubprocessError, OSError):
                pass
            time.sleep(0.05)
        else:
            raise RuntimeError(f"{name} did not become ready")
        info = cli_run(cli, port, "INFO", "server")
        assert f"process_id:{service.process.pid}" in info
        assert f"redis_git_sha1:{variant['source_commit'][:8]}" in info
        (directory / "info-server.txt").write_text(info, encoding="utf-8")
        common = [str(benchmark), "-h", "127.0.0.1", "-p", str(port), "--seed", "7410",
                  "-k", "1", "-c", "50", "-P", "1", "--csv"]
        warmup = NativeProcess(common + ["-n", "20000", "-t", "set,get", "-r", "100000", "-d", "512"],
                               directory, "warmup", client_affinity)
        try:
            warmup.wait()
        finally:
            warmup.close()
        for workload, arguments in [("ping", ["-t", "ping"]),
                                    ("set-get", ["-t", "set,get", "-r", "100000", "-d", "512"])]:
            assert cli_run(cli, port, "FLUSHALL", "SYNC") == "OK"
            assert cli_run(cli, port, "CONFIG", "RESETSTAT") == "OK"
            before = service.cpu()
            load = NativeProcess(common + ["-n", str(requests)] + arguments, directory, workload, client_affinity)
            try:
                load.wait(timeout=600)
                after = service.cpu()
                client_cpu = load.cpu()
                elapsed = time.perf_counter() - load.started
            finally:
                load.close()
            rows = list(csv.DictReader((directory / (workload + ".stdout")).read_text().splitlines()))
            expected = ["PING_INLINE", "PING_MBULK"] if workload == "ping" else ["SET", "GET"]
            assert [row["test"] for row in rows] == expected, rows
            for row in rows:
                assert float(row["rps"]) > 0
                samples.append({"variant": name, "round": round_number, "profile": profile,
                                "requests_per_test": requests, "test": row["test"],
                                **{key: float(value) for key, value in row.items() if key != "test"},
                                "workload_elapsed_seconds": elapsed,
                                "workload_server_user_ms": after["user_ms"] - before["user_ms"],
                                "workload_server_kernel_ms": after["kernel_ms"] - before["kernel_ms"],
                                "workload_client_user_ms": client_cpu["user_ms"],
                                "workload_client_kernel_ms": client_cpu["kernel_ms"]})
            (directory / (workload + ".stats.txt")).write_text(cli_run(cli, port, "INFO", "stats"), encoding="utf-8")
            commandstats = cli_run(cli, port, "INFO", "commandstats")
            (directory / (workload + ".commandstats.txt")).write_text(commandstats, encoding="utf-8")
            commands = {"ping": requests * 2} if workload == "ping" else {"set": requests, "get": requests}
            for command, count in commands.items():
                match = re.search(rf"^cmdstat_{command}:calls=(\d+),", commandstats, re.MULTILINE)
                assert match and int(match[1]) == count, (command, count, commandstats)
            save_json(directory / "samples.json", samples)
        cli_run(cli, port, "SHUTDOWN", "NOSAVE")
        service.wait(timeout=30)
    finally:
        service.close()
    if profile:
        gmon = directory / "gmon.out"
        assert gmon.is_file() and gmon.stat().st_size > 0, f"Missing gprof output for {name}"
        # MinGW gprof's PE symbol reader can attribute static functions to the
        # preceding global function. An explicit nm table preserves their
        # names and call counts (including the event loop and clock helpers).
        symbols = directory / "symbols.txt"
        symbols.write_bytes(subprocess.check_output(["nm", "-n", "--defined-only", str(server)],
                                                     timeout=120))
        report = subprocess.check_output(["gprof", "-b", "-S", str(symbols), str(server), str(gmon)],
                                         text=True, errors="replace", timeout=120)
        assert "Flat profile:" in report and "Call graph" in report
        assert "WSIOCP_QueueNextRead" in report, "Profile is missing the Windows I/O path"
        assert "aeApiPoll" in report and "getMonotonicUs_win32" in report, "Profile lost static function symbols"
        (directory / "gprof.txt").write_text(report, encoding="utf-8")
        save_json(directory / "profile-identity.json", {"server_sha256": digest(server),
                                                       "gmon_sha256": digest(gmon),
                                                       "symbols_sha256": digest(symbols)})
    print(f"Completed {name} round {round_number}", flush=True)
    return samples


def summarize(samples):
    result = []
    baseline = {}
    for (variant, test), grouped in itertools.groupby(sorted(samples, key=lambda s: (s["variant"], s["test"])),
                                                      key=lambda s: (s["variant"], s["test"])):
        rows = list(grouped)
        rates = [row["rps"] for row in rows]
        result.append({"variant": variant, "test": test, "samples": len(rows),
                       "median_rps": statistics.median(rates),
                       "rps_cv": statistics.stdev(rates) / statistics.mean(rates) if len(rates) > 1 else 0,
                       "median_p95_ms": statistics.median(row["p95_latency_ms"] for row in rows),
                       "median_server_user_ms": statistics.median(row["workload_server_user_ms"] for row in rows),
                       "median_server_kernel_ms": statistics.median(row["workload_server_kernel_ms"] for row in rows)})
        if variant == "baseline":
            baseline[test] = result[-1]
    for row in result:
        if row["test"] in baseline:
            row["rps_ratio_to_baseline"] = row["median_rps"] / baseline[row["test"]]["median_rps"]
            row["p95_ratio_to_baseline"] = row["median_p95_ms"] / baseline[row["test"]]["median_p95_ms"]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--stage", choices=["benchmark", "profile"], required=True)
    parser.add_argument("--rounds", type=int, default=0, help="Defaults to two complete rotations of the variants")
    parser.add_argument("--requests", type=int, default=250000)
    parser.add_argument("--profile-requests", type=int, default=100000)
    args = parser.parse_args()
    assert os.name == "nt" and (os.cpu_count() or 0) >= 2, "Native Windows with at least two CPUs is required"
    assert args.rounds >= 0 and args.requests >= 1000 and args.profile_requests >= 1000
    # Prefer nonadjacent logical CPUs on larger runners.
    client_affinity = 4 if os.cpu_count() >= 4 else 2
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    variants = manifest["variants"]
    assert len({v["name"] for v in variants}) == len(variants)
    profiling = args.stage == "profile"
    selected = [v for v in variants if v["profile"] == profiling]
    assert selected
    rounds = 1 if profiling else (args.rounds or 2 * len(selected))
    client = Path(next(v["directory"] for v in variants if v["name"] == "baseline" and not v["profile"]))
    output = Path(manifest["output"]) / args.stage
    output.mkdir(parents=True)
    identities = {v["name"]: {**v, "server_sha256": digest(Path(v["directory"]) / "redis-server.exe")}
                  for v in variants}
    save_json(output / "identity.json", {"harness_commit": manifest["harness_commit"], "variants": identities,
              "benchmark_sha256": digest(client / "redis-benchmark.exe"),
              "cli_sha256": digest(client / "redis-cli.exe"), "platform": platform.platform(),
              "python": sys.version, "logical_cpus": os.cpu_count(),
              "server_affinity_mask": 1, "client_affinity_mask": client_affinity,
              "transport": "plaintext", "requests": args.requests,
              "profile_requests": args.profile_requests, "rounds": rounds})
    samples = []
    for round_number in range(1, rounds + 1):
        offset = (round_number - 1) % len(selected)
        order = selected[offset:] + selected[:offset]
        if ((round_number - 1) // len(selected)) % 2:
            order = list(reversed(order))
        for variant in order:
            samples.extend(run_variant(variant, client, output, round_number,
                           args.profile_requests if profiling else args.requests, profiling, 6397, client_affinity))
            save_json(output / "samples.json", samples)
    summary = summarize(samples)
    save_json(output / "summary.json", summary)
    for row in summary:
        print(json.dumps(row), flush=True)
    for variant in variants:
        assert digest(Path(variant["directory"]) / "redis-server.exe") == identities[variant["name"]]["server_sha256"]


if __name__ == "__main__":
    main()
