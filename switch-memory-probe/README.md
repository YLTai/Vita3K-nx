# Switch Memory Probe (standalone NRO)

This is a tiny, self-contained Nintendo Switch homebrew for checking the memory
capabilities that matter to a Vita3K-nx dirty-page tracker. It **does not**
launch Vita3K or require its graphics dependencies.

## Build

With devkitPro's devkitA64 + libnx installed:

```sh
export DEVKITPRO=/opt/devkitpro
export DEVKITA64=$DEVKITPRO/devkitA64
make -C switch-memory-probe -j2
```

Output: `switch-memory-probe/switch-memory-probe.nro`. A dedicated GitHub Actions
workflow builds only this probe (no emulator builds).

## Run

Copy the NRO to `sdmc:/switch/switch-memory-probe.nro` and launch via hbmenu,
preferably with the same application-title-takeover environment as Vita3K-nx.

The safe checks run immediately:

1. `svcGetInfo` for HOS, region locations, system-resource size, debugger and
   available user-exception-context address.
2. `svcQueryMemory` on stack, static data and dedicated, page-aligned heap.
   Print permission, attributes, memory type, permission-change flag, map flag.
3. On a dedicated heap page, attempt `svcSetMemoryPermission(R)` and restore to
   `Rw` **without** writing while read-only.
4. Attempt a real `svcMapMemory` of a disposable heap page into free Stack
   address space (`virtmemFindStack`), query both source and destination,
   attempt permission changes on the destination, restore and unmap. The source
   is **never touched while borrowed**.
5. `jitCreate`/`jitTransitionToExecutable`: execute two AArch64 instructions
   (`str w1, [x0]` + `ret`) to prove a direct JIT-generated store does not go
   through an unrelated user-supplied write callback. This is an analogy,
   **not a Dynarmic implementation test**.

The small text report is saved once when the safe checks finish, to
`sdmc:/switch/vita3k-memory-probe-<timestamp>-<run>.txt`. The console supports
Up/Down to scroll, Y to repeat the safe suite, and + to quit.

## Dangerous exception experiment (opt-in ONLY)

Press **X**, then hold **L+R+ZL+ZR** and press **A**. The tool attempts to make
one separate heap scratch page read-only and deliberately writes to it. It
writes a `pre-fault` report marker beforehand. **A normal retail Horizon/libnx
process may crash immediately and require a relaunch**. No automatic resume
is promised; a crash demonstrates this default setup did not recover, but
not that all custom exception handlers are impossible.

Do NOT use that experiment when important unsaved data is present. All other
tests avoid intentionally faulting.

## What this probe cannot prove

- It cannot conclusively verify that a custom data-abort handler can resume
  arbitrary memory writes: doing that requires a separate runtime/exception
  experiment, with kernel and firmware restrictions.
- It cannot run Vita3K's real Dynarmic guest page-table path, instrument all
  HLE pointer writes, or observe Vulkan/NVK GPU shader writes. These require
  integration tests inside Vita3K-nx and a comprehensive mapping/write-barrier
  review.
- `MemState_PermChangeAllowed` is a required capability signal, **not** proof
  that the requested permission change, data-abort delivery, or recovery works.
- Results reflect the *current* loader mode, HOS firmware, libnx version and
  memory type, not a universal property of all Switch consoles or APIs.

To discuss next steps, share the generated `.txt` report and whether the
separate dangerous fault test was attempted. The safest preliminary data are
sufficient to rule out the incorrect claim that 'Switch Vulkan can't protect
memory at all'.