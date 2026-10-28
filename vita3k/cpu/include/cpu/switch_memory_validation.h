// Isolated Switch Dynarmic memory validation. No real Guest mappings are used.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Call once before Guest workers start, after the validation report is ready.
// The implementation is a no-op outside opt-in Switch TraceLab builds.
void run_jit_memory_validation();
