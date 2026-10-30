// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SotC Lab control pipe. With SOTC_CTL=1 (or SOTC_CTL=<name>, or SOTC_LAB=1) one thread
// serves \\.\pipe\sotc-ctl (or \\.\pipe\<name>): newline-terminated commands in, one reply line
// out per command (SotcLab::Execute, lab_commands.h). One client at a time; a disconnect waits for
// the next client. Windows only; a no-op elsewhere.
#pragma once

namespace SotcCtl {

/// Starts the server thread when the switch is set. Idempotent.
void StartIfEnabled();
/// Stops the server thread (process exit).
void Stop();
/// The pipe name in use, or nullptr when the server is off.
const char* PipeName();

} // namespace SotcCtl
