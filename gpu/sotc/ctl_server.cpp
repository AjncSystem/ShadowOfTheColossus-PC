// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SotC Lab control pipe (ctl_server.h). One thread owns a single named-pipe instance and
// serves one client at a time; each line is handed to SotcLab::Execute and answered with one line.

#include "ctl_server.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "lab_commands.h"

#ifdef _WIN32
#include <atomic>
#include <mutex>
#include <thread>

#include <windows.h>

#include "common/thread.h"
#endif

namespace SotcCtl {

#ifdef _WIN32
namespace {

constexpr const char* kDefaultName = "sotc-ctl";
constexpr size_t kMaxLineBytes = 64 * 1024;

std::mutex g_mutex; // guards start and stop
bool g_running = false;
std::string g_name;
std::thread g_thread;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_done{true};
std::atomic<HANDLE> g_pipe{nullptr};

// SOTC_CTL=<name> picks the pipe name; "1", "0" and empty mean the default name.
std::string PipeNameFromEnv() {
    const char* v = std::getenv("SOTC_CTL");
    if (v && v[0] != '\0' && std::strcmp(v, "0") != 0 && std::strcmp(v, "1") != 0 &&
        std::strchr(v, '\\') == nullptr) {
        return v;
    }
    return kDefaultName;
}

bool Wanted() {
    const char* v = std::getenv("SOTC_CTL");
    if (v && v[0] != '\0' && std::strcmp(v, "0") != 0) {
        return true;
    }
    return SotcLab::Enabled();
}

std::string PathOf(const std::string& name) {
    return "\\\\.\\pipe\\" + name;
}

std::string Trim(const std::string& s) {
    const char* ws = " \t\r\n";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) {
        return {};
    }
    const size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

// Serves one connected client until it disconnects, sends "quit", or the server stops.
void Serve(HANDLE pipe) {
    std::string buf;
    char chunk[4096];
    while (!g_stop.load()) {
        DWORD got = 0;
        if (!ReadFile(pipe, chunk, sizeof chunk, &got, nullptr) || got == 0) {
            return;
        }
        buf.append(chunk, got);
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            const std::string line = Trim(buf.substr(0, nl));
            buf.erase(0, nl + 1);
            if (line.empty()) {
                continue;
            }
            const bool quit = line == "quit";
            const std::string reply = SotcLab::Execute(line) + "\n";
            DWORD wrote = 0;
            if (!WriteFile(pipe, reply.data(), static_cast<DWORD>(reply.size()), &wrote, nullptr)) {
                return;
            }
            // Wait until the client has read the reply: DisconnectNamedPipe would drop it otherwise.
            FlushFileBuffers(pipe);
            if (quit) {
                return;
            }
        }
        if (buf.size() > kMaxLineBytes) {
            return; // no newline in sight: drop the client
        }
    }
}

void ServerMain(std::string name) {
    Common::SetCurrentThreadName("sotc:ctl");
    const std::string path = PathOf(name);
    HANDLE pipe = CreateNamedPipeA(path.c_str(), PIPE_ACCESS_DUPLEX,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 65536,
                                   65536, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "SOTC CTL cannot create %s (error %lu)\n", path.c_str(),
                     static_cast<unsigned long>(GetLastError()));
        g_done.store(true);
        return;
    }
    g_pipe.store(pipe);
    std::fprintf(stderr, "SOTC CTL listening on %s\n", path.c_str());

    while (!g_stop.load()) {
        const BOOL connected = ConnectNamedPipe(pipe, nullptr)
                                   ? TRUE
                                   : (GetLastError() == ERROR_PIPE_CONNECTED ? TRUE : FALSE);
        if (g_stop.load()) {
            break;
        }
        if (connected) {
            Serve(pipe);
        } else {
            Sleep(50);
        }
        DisconnectNamedPipe(pipe);
    }
    g_pipe.store(nullptr);
    CloseHandle(pipe);
    g_done.store(true);
}

} // namespace

void StartIfEnabled() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_running || !Wanted()) {
        return;
    }
    g_name = PipeNameFromEnv();
    g_stop.store(false);
    g_done.store(false);
    g_running = true;
    g_thread = std::thread(ServerMain, g_name);
}

void Stop() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_running) {
        return;
    }
    g_stop.store(true);
    const std::string path = PathOf(g_name);
    // Each round wakes the server wherever it blocks: DisconnectNamedPipe ends a ReadFile on a
    // connected client, and a throw-away client connection ends ConnectNamedPipe. The server
    // checks g_stop after both, so it exits on the next round at the latest.
    for (int round = 0; round < 200 && !g_done.load(); ++round) {
        const HANDLE pipe = g_pipe.load();
        if (pipe) {
            DisconnectNamedPipe(pipe);
        }
        const HANDLE client = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                          OPEN_EXISTING, 0, nullptr);
        if (client != INVALID_HANDLE_VALUE) {
            CloseHandle(client);
        }
        Sleep(10);
    }
    if (g_done.load()) {
        g_thread.join();
    } else {
        g_thread.detach(); // never got out of a blocking call; process exit ends it
    }
    g_running = false;
}

const char* PipeName() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_running ? g_name.c_str() : nullptr;
}

#else // !_WIN32

void StartIfEnabled() {}
void Stop() {}
const char* PipeName() {
    return nullptr;
}

#endif

} // namespace SotcCtl
