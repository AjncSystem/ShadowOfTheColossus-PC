// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: overlay (Dear ImGui) drawn by the presenter into the swapchain image after the game
// frame, at display resolution: the game's dialogs (save list, text entry) and the FPS counter.
// While a dialog is shown the game gets no pad/keyboard input.

#pragma once

#include <string>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

union SDL_Event;
struct SDL_Window;

namespace Vulkan {
class Instance;
}

namespace BbOverlay {

/// Present thread, once: the ImGui context and its Vulkan backend.
void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count);

/// Window thread, for every SDL event: true when the save list consumed it.
bool HandleEvent(const SDL_Event& event);

/// Whether anything is drawn this frame (a game dialog or the FPS counter).
bool Visible();

/// Present thread: draws into `view` (layout ColorAttachmentOptimal).
void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent);

/// A text entry or the save list is shown: the game's input is held neutral.
bool CapturesInput();

/// Window thread: the game's text dialog (sceImeDialog). Shown as a box on screen until
/// `active` is false; typing goes to the window (sdl_window), the box only displays it.
void SetTextEntry(bool active, const std::string& prompt, const std::string& text);

/// The game's save/load slot list (sceSaveDataDialog LIST): shown until chosen or cancelled.
/// Any thread. `focus` is the item selected first; items from `first_deletable` on can be
/// deleted (Del / Triangle, after a confirmation; -1: none). False when the overlay is not running.
bool OpenSaveList(const std::string& title, const std::vector<std::string>& items, int focus,
                  int first_deletable = -1);
/// -1 while the list is open (or none), -2 cancelled, -100 - i: delete item i, else the chosen
/// item. Closes the list when it returns anything but -1.
int PollSaveList();
void CloseSaveList();

} // namespace BbOverlay
