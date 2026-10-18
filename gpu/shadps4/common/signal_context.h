// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Common {

void* GetXmmPointer(void* ctx, u8 index);

void* GetRip(void* ctx);

void IncrementRip(void* ctx, u64 length);

bool IsWriteError(void* ctx);

/// sotc: emulates a load that faulted as one that read zero: sets general purpose register
/// gpr_index (x86 encoding: 0 = rax ... 7 = rdi, 8-15 = r8-r15) to 0 and skips the instruction.
void SkipLoadAsZero(void* ctx, u8 gpr_index, u64 length);

} // namespace Common