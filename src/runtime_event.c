/* sotc: session event log (runtime_event.h). */
#include "runtime_event.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

static pthread_mutex_t event_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *event_file;
static int event_file_tried;
static struct timespec event_start;

/* Times count from the start of the process. */
__attribute__((constructor)) static void event_clock_start(void) {
    clock_gettime(CLOCK_MONOTONIC, &event_start);
}

int sotc_event_pt(void) {
    static int pt = -1;
    if (pt < 0) {
        const char *v = getenv("SOTC_UI_LANG");
        pt = v && !strncmp(v, "pt", 2);
    }
    return pt;
}

void sotc_event(const char *category, const char *format, ...) {
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    pthread_mutex_lock(&event_lock);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long ms = (long long)(now.tv_sec - event_start.tv_sec) * 1000 +
                   (now.tv_nsec - event_start.tv_nsec) / 1000000;
    if (ms < 0) ms = 0;
    char line[1200];
    snprintf(line, sizeof(line), "[%02lld:%02lld:%02lld.%03lld] [%-8s] %s\n", ms / 3600000,
             ms / 60000 % 60, ms / 1000 % 60, ms % 1000, category, message);
    fputs(line, stderr);
    fflush(stderr);
    if (!event_file_tried) {
        event_file_tried = 1;
        const char *path = getenv("SOTC_EVENT_LOG");
        if (path && *path) event_file = fopen(path, "a"); /* the launcher wrote the header */
    }
    if (event_file) {
        fputs(line, event_file);
        fflush(event_file);
    }
    pthread_mutex_unlock(&event_lock);
}
