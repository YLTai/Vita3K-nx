// The call sites audit native waiting ONLY. Never use around callbacks, output
// writes or queue insertion. Outside a tracked import this is a no-op.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#if defined(__SWITCH__) && defined(VITA3K_SWITCH_TRACELAB)
#include <util/jit_epoch_gate.h>
#define V3K_NATIVE_WAIT_SCOPE() const switch_validation::epoch::NativeWaitLease v3k_native_wait_scope
#else
#define V3K_NATIVE_WAIT_SCOPE() ((void)0)
#endif
