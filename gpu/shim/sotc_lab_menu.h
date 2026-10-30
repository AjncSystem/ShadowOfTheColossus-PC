// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SotC Lab ImGui menu (F2), drawn inside the overlay's ImGui frame (bbport_overlay.cpp).
// Tabs: Câmera, Rotas, Colossos, SOTC_*, Testes. Actions go through SotcLab::Execute and the
// module APIs (freecam.h, autopilot.h, engine_cmd.h, sotc_live.h). Switch list: out/tuning.json
// (scripts/tuning_export.py), path from SOTC_TUNING_JSON.
#pragma once

namespace SotcLabMenu {

/// True when the SotC Lab is enabled (SotcLab::Enabled()).
bool Available();
bool Visible();
void Toggle();
/// Draws the window when visible. Call between ImGui::NewFrame and ImGui::Render.
void Draw();

} // namespace SotcLabMenu
