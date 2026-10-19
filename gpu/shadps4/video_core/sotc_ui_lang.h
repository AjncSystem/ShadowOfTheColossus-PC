// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: language of the port's own on-screen texts (overlay, F7/F8 messages): the launcher's,
// passed as SOTC_UI_LANG (pt or en; anything else is English).
#pragma once

#include <cstdlib>
#include <cstring>

namespace SotcUi {

inline bool Pt() {
    static const bool pt = [] {
        const char* v = std::getenv("SOTC_UI_LANG");
        return v && std::strncmp(v, "pt", 2) == 0;
    }();
    return pt;
}

inline const char* T(const char* pt, const char* en) {
    return Pt() ? pt : en;
}

} // namespace SotcUi
