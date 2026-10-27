#!/usr/bin/env python3
"""Compile the production Switch checkpoint with a fake clock/kernel API.

The function body and its profile entry/class are taken from the repository.
Only libnx calls and registration are replaced, so the lock contention test
exercises the production control flow without a Switch or the full toolchain.
It does not validate Horizon's thread-tick SVC or real SD-card performance.
"""

import argparse
import pathlib
import re
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parents[2])
    parser.add_argument("--source", type=pathlib.Path)
    parser.add_argument("--cxx", default="c++")
    args = parser.parse_args()
    source_path = args.source or args.repo / "vita3k/util/src/switch_thread.cpp"
    source = source_path.read_text()
    function = re.search(r"(?ms)^void SwitchCpuProfileThread::checkpoint\(\) \{.*?^\}\n", source)
    entry = re.search(r"(?ms)^struct CpuProfileEntry \{.*?^\};\n", source)
    if not function or not entry:
        raise SystemExit("Could not find the production checkpoint/entry")

    prelude = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <mutex>
#include <vector>
#define __SWITCH__
#define private public
#include <util/switch_thread.h>
#undef private
using u64 = uint64_t;
using u32 = uint32_t;
using s32 = int32_t;
using Result = uint32_t;
using Handle = uint32_t;
constexpr Handle INVALID_HANDLE = 0, CUR_THREAD_HANDLE = 1;
constexpr int InfoType_ThreadTickCount = 0, TickCountInfo_Total = 0;
bool R_SUCCEEDED(Result rc) { return rc == 0; }
std::atomic<u64> fake_tick{1000}, fake_cpu{100};
std::atomic<unsigned> query_count{0};
u64 armGetSystemTick() { return fake_tick.load(); }
u64 armGetSystemTickFreq() { return 1000; }
Result svcGetInfo(u64 *ticks, int, Handle, int) {
    ++query_count;
    *ticks = fake_cpu.load();
    return 0;
}
u32 svcGetCurrentProcessorNumber() { return 3; }
'''
    globals_ = r'''
std::mutex cpu_profile_mutex;
std::vector<CpuProfileEntry> cpu_profile_threads(1);
std::atomic<bool> cpu_profile_enabled{true};
SwitchCpuProfileThread::SwitchCpuProfileThread(const char *) { token_ = 1; }
SwitchCpuProfileThread::~SwitchCpuProfileThread() = default;
'''
    tests = r'''
int main() {
    using namespace std::chrono_literals;
    SwitchCpuProfileThread thread("test");
    auto &entry = cpu_profile_threads[0];
    entry.token = 1;

    // Sample a real baseline, then simulate the frontend writing to SD.
    thread.checkpoint();
    assert(query_count == 1);
    assert(entry.self_wall == 1000 && entry.self_total == 100);
    assert(entry.previous_self_wall == 1000 && entry.previous_self_total == 100);
    assert(thread.next_checkpoint_tick_ == 2000);
    fake_tick = 2100;
    fake_cpu = 430;
    std::unique_lock<std::mutex> writer(cpu_profile_mutex);
    auto pending = std::async(std::launch::async, [&] { thread.checkpoint(); });
    const bool returned_without_writer = pending.wait_for(250ms) == std::future_status::ready;
    if (!returned_without_writer) {
        writer.unlock();
        pending.get();
        std::cerr << "checkpoint blocked behind the frontend writer\n";
        return 1;
    }
    pending.get();
    assert(query_count == 1); // No kernel query while the sample cannot be published.
    assert(thread.next_checkpoint_tick_ == 2000); // Retry remains due.
    assert(entry.self_wall == 1000 && entry.self_total == 100);
    writer.unlock();

    // No extra one-second wait: the next frame can sample immediately.
    fake_tick = 2150;
    fake_cpu = 445;
    thread.checkpoint();
    assert(query_count == 2);
    assert(entry.self_wall == 2150 && entry.self_total == 445);
    assert(thread.next_checkpoint_tick_ == 3150);
    // The frontend's existing self_pct uses these actual tick differences.
    assert(entry.self_wall - entry.previous_self_wall == 1150);
    assert(entry.self_total - entry.previous_self_total == 345);
    const double pct = 100.0 * (entry.self_total - entry.previous_self_total)
        / (entry.self_wall - entry.previous_self_wall);
    assert(pct == 30.0);

    fake_tick = 2200;
    thread.checkpoint();
    assert(query_count == 2); // The successful sample remains rate limited.
    fake_tick = 4000;
    cpu_profile_enabled = false;
    thread.checkpoint();
    assert(query_count == 2 && thread.next_checkpoint_tick_ == 3150);
    cpu_profile_enabled = true;
    entry.token = 9; // No use of a removed registration.
    thread.checkpoint();
    assert(query_count == 2 && thread.next_checkpoint_tick_ == 3150);
    std::cout << "PASS: contention, immediate retry, actual-span accounting, cadence, disabled and removed registration\n";
}
'''
    with tempfile.TemporaryDirectory(prefix="switch-checkpoint-") as work:
        work = pathlib.Path(work)
        test_cpp = work / "checkpoint.cpp"
        test_cpp.write_text(prelude + entry.group() + globals_ + function.group() + tests)
        executable = work / "checkpoint"
        command = shlex.split(args.cxx) + ["-std=c++17", "-Wall", "-Wextra", "-Werror",
                                          "-pthread", "-I" + str(args.repo / "vita3k/util/include"),
                                          str(test_cpp), "-o", str(executable)]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True, timeout=10)


if __name__ == "__main__":
    main()
