#!/usr/bin/env python3
"""Compile the actual Dynarmic/Oaknut memory emitters and check their A64 bytes.

Run after apply-switch-submodule-patches.sh. Needs a host C++20 compiler, git,
and the pinned Dynarmic/Oaknut checkout; no Switch SDK or Python packages.
The source before the write-table patch is reconstructed in a temporary
directory. No working-tree or real-index changes are made. Generated A64
instructions are inspected, never executed on the host.

This gate covers the page-table lookup and memory instruction emission. It
does not substitute for the on-device Run/Step/callback/concurrency tests.
"""
import argparse
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DYNARMIC = ROOT / "external/dynarmic"
PATCH = ROOT / "patches/submodules/dynarmic-switch-write-table.patch"
EMITTER = Path("src/dynarmic/backend/arm64/emit_arm64_memory.cpp")


def extract_emitters(source):
    start = "constexpr size_t page_bits = "
    end = "template<size_t bitsize>\nvoid InlinePageTableEmitReadMemory"
    assert source.count(start) == 1 and source.count(end) == 1, "Memory emitter layout changed; update the extraction boundary"
    return source[source.index(start):source.index(end)]


def reconstruct_previous(tmp):
    previous = tmp / "previous"
    previous.mkdir()
    paths = re.findall(r"^\+\+\+ b/(.+)$", PATCH.read_text(), re.M)
    assert paths and EMITTER.as_posix() in paths, "Write-table patch has no memory emitter"
    for name in paths:
        path = Path(name)
        assert not path.is_absolute() and ".." not in path.parts, "Unsafe patch path"
        destination = previous / path
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(DYNARMIC / path, destination)
    result = subprocess.run(
        ["git", "apply", "--reverse", str(PATCH)], cwd=previous,
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    assert result.returncode == 0, (
        "Cannot reconstruct the pre-write-table source. Apply the complete "
        "Switch patch stack first, and keep the additive patch in sync.\n" + result.stderr
    )
    return (previous / EMITTER).read_text()


HARNESS_HEAD = r'''
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <oaknut/oaknut.hpp>
#include "dynarmic/backend/arm64/abi.h"
using namespace Dynarmic::Backend::Arm64;
using namespace oaknut::util;
using u64 = std::uint64_t;
using CodePtr = const std::uint32_t*;
using SharedLabel = std::shared_ptr<oaknut::Label>;
#define UNREACHABLE() std::abort()
#define ASSERT_FALSE(...) std::abort()

// Supply only the surrounding compiler context. Every instruction-emission
// function below comes verbatim from the actual old/new memory source.
struct Config {
    std::size_t page_table_address_space_bits = 32;
    bool silently_mirror_page_table = true;
    int page_table_pointer_mask_bits = 0;
    bool absolute_offset_page_table = true;
    std::uint8_t detect_misaligned_access_via_page_table = 0;
    bool only_detect_misalignment_via_page_table_on_page_boundary = true;
    std::uint64_t write_page_table_pointer = 0;
};
struct EmitContext { Config conf; };

static_assert(Xpagetable.index() == 24 && Xfastmem.index() == 25);
static_assert((ABI_CALLEE_SAVE & (std::uint64_t{1} << 25)) != 0);
static_assert([] {
    for (int reg : GPR_ORDER)
        if (reg == 25)
            return false;
    return true;
}(), "X25 must remain excluded from allocation");
'''


HARNESS_MAIN = r'''
template<std::size_t bits>
void emit(const char* name, bool old, bool write, bool enabled, bool boundary) {
    std::array<std::uint32_t, 64> words{};
    oaknut::CodeGenerator code(words.data());
    EmitContext ctx;
    ctx.conf.write_page_table_pointer = enabled ? 1 : 0;
    ctx.conf.detect_misaligned_access_via_page_table = boundary ? bits : 0;
    auto fallback = std::make_shared<oaknut::Label>();
    if (old) {
        const auto [base, offset] = previous::InlinePageTableEmitVAddrLookup<bits>(code, ctx, X0, fallback);
        if (write)
            previous::EmitMemoryStr<bits>(code, 1, base, offset, false);
        else
            previous::EmitMemoryLdr<bits>(code, 1, base, offset, false);
    } else {
        const auto [base, offset] = write
            ? current::InlinePageTableEmitVAddrLookup<bits, true>(code, ctx, X0, fallback)
            : current::InlinePageTableEmitVAddrLookup<bits, false>(code, ctx, X0, fallback);
        if (write)
            current::EmitMemoryStr<bits>(code, 1, base, offset, false);
        else
            current::EmitMemoryLdr<bits>(code, 1, base, offset, false);
    }
    code.l(*fallback);
    code.RET();
    std::printf("%s_%zu", name, bits);
    for (auto p = words.data(); p != code.wptr<std::uint32_t*>(); ++p)
        std::printf(" %08x", *p);
    std::puts("");
}

template<std::size_t bits>
void check_width() {
    emit<bits>("before_store", true, true, false, false);
    emit<bits>("default_store", false, true, false, false);
    emit<bits>("split_store", false, true, true, false);
    emit<bits>("before_read", true, false, false, false);
    emit<bits>("default_read", false, false, false, false);
    emit<bits>("split_read", false, false, true, false);
    emit<bits>("cross_store", false, true, true, true);
}

int main() {
    check_width<8>();
    check_width<16>();
    check_width<32>();
    check_width<64>();
}
'''


def check_words(rows, bits):
    def words(name):
        return rows[f"{name}_{bits}"]

    original_store = words("before_store")
    default_store = words("default_store")
    split_store = words("split_store")
    assert original_store == default_store, f"Default {bits}-bit store changed its A64 instructions"
    assert words("before_read") == words("default_read") == words("split_read"), f"Read path changed for {bits} bits"
    assert len(default_store) == len(split_store) == 5, f"Extra fast-path instructions for {bits} bits"

    # A64 LDR X16, [Xn, X16, LSL #3]: Rn occupies bits 9:5. Assert the
    # actual opcode and all other instruction bits, without a disassembler.
    rn_mask = 31 << 5
    assert default_store[1] == 0xF8707B10, "Expected original X24 PTE load"
    assert split_store[1] == 0xF8707B30, "Expected write-table X25 PTE load"
    assert (default_store[1] & ~rn_mask) == (split_store[1] & ~rn_mask)
    changed = [i for i, pair in enumerate(zip(default_store, split_store)) if pair[0] != pair[1]]
    assert changed == [1], f"Write table altered more than the PTE base register: {bits} bits"
    assert default_store[0] == 0xD34C7C10, "Expected guest page-index UBFX"
    assert default_store[2] == 0xB4000050, "Expected existing zero-PTE CBZ"
    assert default_store[-1] == 0xD65F03C0, "Expected harness return instruction"

    cross = words("cross_store")
    if bits == 8:
        assert cross == split_store, "A byte store cannot cross a page"
    else:
        assert len(cross) == len(split_store) + 3 and cross[3:] == split_store
        assert cross[0] == 0x92402C10, "Expected AND X16, X0, #0xfff"
        # CMP X16, #(4096 - access_bytes), then B.HI to the fallback RET.
        assert cross[1] == (0xF100021F | ((4096 - bits // 8) << 10)), f"Wrong {bits}-bit boundary threshold"
        assert cross[2] == 0x540000A8, "Expected B.HI fallback, five instructions ahead"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="c++", help="Host C++20 compiler (default: c++)")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vita3k-write-table-codegen-") as directory:
        tmp = Path(directory)
        previous = reconstruct_previous(tmp)
        current = (DYNARMIC / EMITTER).read_text()
        harness = tmp / "memory-emission.cpp"
        harness.write_text(
            HARNESS_HEAD
            + "\nnamespace previous {\n" + extract_emitters(previous) + "\n}\n"
            + "\nnamespace current {\n" + extract_emitters(current) + "\n}\n"
            + HARNESS_MAIN
        )
        executable = tmp / "memory-emission"
        subprocess.run([
            args.cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
            "-D__SWITCH__", "-I", str(DYNARMIC / "src"),
            "-I", str(DYNARMIC / "externals/mcl/include"),
            "-I", str(DYNARMIC / "externals/oaknut/include"),
            str(harness), "-o", str(executable),
        ], check=True)
        result = subprocess.run([str(executable)], check=True, text=True, stdout=subprocess.PIPE)
        rows = {}
        for line in result.stdout.splitlines():
            name, *encoded = line.split()
            assert name not in rows, "Duplicate emission case"
            rows[name] = [int(word, 16) for word in encoded]
        assert len(rows) == 28, "Not all requested widths and modes emitted"
        for bits in (8, 16, 32, 64):
            check_words(rows, bits)
    print("Write-table A64 emission OK: 8/16/32/64-bit default bytes unchanged; "
          "stores change only X24 to X25; reads unchanged; real boundary checks and X25 ABI verified")


if __name__ == "__main__":
    main()
