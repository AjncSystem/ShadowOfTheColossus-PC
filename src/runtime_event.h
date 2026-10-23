/* sotc: session event log. One line per event a player or a developer wants to find again
 * (settings at start, in-game menu changes, keys, controllers, loads, texture levels, errors):
 *     [00:12:34.567] [MENU    ] Texture quality: High -> Maximum
 * to stderr (the session log) and to SOTC_EVENT_LOG (user/events.log, set by the launcher, which
 * writes the configuration header first). Messages follow the launcher's language (SOTC_UI_LANG
 * pt or en); categories stay in English so the file can be searched the same way everywhere. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void sotc_event(const char *category, const char *format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
/* 1 when the launcher's language is Portuguese (SOTC_UI_LANG=pt...). */
int sotc_event_pt(void);

#ifdef __cplusplus
}
#endif

#define SOTC_T(pt, en) (sotc_event_pt() ? (pt) : (en))
