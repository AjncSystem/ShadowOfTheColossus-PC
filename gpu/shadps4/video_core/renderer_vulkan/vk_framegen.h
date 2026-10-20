// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the frame generation mode: off, NVIDIA DLSS Frame Generation (vk_dlssg.cpp, RTX 40/50)
// or AMD FSR 3 frame generation (vk_fsrfg.cpp, any Vulkan 1.3 GPU). One generated frame between
// two game frames: a game locked at 30 FPS is shown at 60.
//
// SOTC_FRAMEGEN=off|auto|dlss|fsr chooses the mode at start (the launcher's setting; the old
// SOTC_DLSSG=1 means dlss). auto: DLSS-G when the GPU has it, else FSR 3. F8 in game switches
// off -> DLSS-G -> FSR 3 -> off, skipping what the GPU cannot do. Streamline (DLSS-G) is loaded
// only with an NVIDIA GPU and a mode other than off, so with off F8 offers FSR 3 alone.

#pragma once

namespace Vulkan::FrameGen {

enum class Mode : int { Off = 0, Dlss = 1, Fsr = 2 };

/// Before the Vulkan instance is created: whether to load Streamline.
bool WantStreamline();

/// After the device is created: what this GPU can do. Resolves auto.
void SetAvailable(bool dlss, bool fsr);

/// The current mode (any thread).
Mode Current();

/// A mode was asked at start (the presenter keeps more frames in flight).
bool AskedAtStart();

bool Available(Mode mode);

/// F8 (window thread): the next available mode, which it returns.
Mode Cycle();

/// sotc: switches to this mode when available (the in-game menu); false otherwise.
bool Set(Mode mode);

/// For the overlay: "off" (in the launcher's language), "DLSS-G", "FSR 3".
const char* Name(Mode mode);

/// Present thread, at each game frame: SOTC_FRAMEGEN_TEST_CYCLE=<seconds> switches the mode
/// like F8 that often (scripted tests of the switches).
void TestTick();

} // namespace Vulkan::FrameGen
