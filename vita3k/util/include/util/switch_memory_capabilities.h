// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

#pragma once

namespace switch_validation {

// Isolated, synchronous capability checks for the running emulator process.
// Call once during the opt-in validation session, before starting guest work.
// Results go to the shared validation recorder. No intentional access faults,
// guest memory, renderer resources, process-wide handlers, or file I/O are used.
void run_memory_capability_validation();

// A failed cleanup may leave a protected/borrowed allocation owned by this
// process. In that case exit the process normally instead of chainloading an
// NRO into its address space. True when the suite did not retain resources.
bool memory_capabilities_allow_chainload();

} // namespace switch_validation
