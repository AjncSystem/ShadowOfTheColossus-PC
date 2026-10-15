// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: DrawScan (see sotc_draw_scan.h).

#include <cstdio>
#include <cstdlib>
#include "common/thread.h"
#include "video_core/amdgpu/sotc_draw_scan.h"
#include "video_core/sotc_stats.h"

namespace AmdGpu {

namespace {
u32 EnvMode(const char* name) {
    const char* v = std::getenv(name);
    return v ? static_cast<u32>(std::strtoul(v, nullptr, 10)) : 0u;
}
} // namespace

u32 DrawScan::PrepMode() {
    // On by default since the attract demo A/B (three pairs: command processor work -8%, average
    // fps +3.6%) and the save-load route verify run; SOTC_DRAW_PREP=0 turns it off.
    static const u32 mode = [] {
        const char* v = std::getenv("SOTC_DRAW_PREP");
        return v ? static_cast<u32>(std::strtoul(v, nullptr, 10)) : 1u;
    }();
    return mode;
}

bool DrawScan::Enabled() {
    static const bool enabled = EnvMode("SOTC_DRAW_SCAN") != 0 || PrepMode() != 0;
    return enabled;
}

u32 DrawScan::VerifyMode() {
    static const u32 mode = EnvMode("SOTC_DRAW_SCAN");
    return mode;
}

DrawScan::Submission::~Submission() = default;

DrawScan::DrawScan(const Regs& initial_regs) : scan_regs{std::make_unique<Regs>(initial_regs)} {
    thread = std::jthread([this](std::stop_token stop) { ScanLoop(stop); });
}

DrawScan::~DrawScan() {
    Stop();
}

void DrawScan::Stop() {
    if (!thread.joinable()) {
        return;
    }
    thread.request_stop();
    cv.notify_all();
    thread.join();
    std::scoped_lock lk{mutex};
    preparer = nullptr;
}

std::shared_ptr<DrawScan::Submission> DrawScan::Build(std::span<const u32> dcb) {
    auto submission = std::make_shared<Submission>();
    submission->commands.assign(dcb.begin(), dcb.end());
    return submission;
}

u64 DrawScan::Enqueue(std::shared_ptr<Submission> submission) {
    std::scoped_lock lk{mutex};
    submission->seq = next_seq++;
    pending.push_back(submission);
    recent.push_back(std::move(submission));
    // Bounded: a command processor far behind (or a scanner far behind) only loses statistics.
    while (recent.size() > 256) {
        recent.pop_front();
    }
    cv.notify_one();
    return next_seq - 1;
}

void DrawScan::ScanLoop(std::stop_token stop) {
    Common::SetCurrentThreadName("sotc:DrawScan");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
    while (!stop.stop_requested()) {
        std::shared_ptr<Submission> submission;
        std::vector<std::shared_ptr<Submission>> done;
        {
            std::unique_lock lk{mutex};
            cv.wait(lk, stop, [this] { return !pending.empty(); });
            if (stop.stop_requested()) {
                return;
            }
            submission = std::move(pending.front());
            pending.pop_front();
            done.swap(retired);
        }
        done.clear(); // command buffer copies and prepared records, freed off the CP
        // Every submission is walked in order, even when the command processor is already past
        // it: the history hash and the register file must see every packet.
        Scan(*submission);
        submission->scanned.store(true, std::memory_order_release);
    }
}

void DrawScan::Scan(Submission& submission) {
    const auto& commands = submission.commands;
    // First pass: the packet layout and the number of draws (the records are allocated once,
    // so the command processor can read published ones while later ones are written).
    size_t end = 0;
    u32 num_draws = 0;
    while (end < commands.size()) {
        const auto* header = reinterpret_cast<const PM4Header*>(&commands[end]);
        if (header->type == 2) {
            ++end;
            continue;
        }
        if (header->type != 3) {
            break; // garbage: the rest of this buffer is not scanned
        }
        const u32 words = header->type3.NumWords() + 1;
        if (end + words > commands.size()) {
            break;
        }
        num_draws += IsDrawOpcode(header->type3.opcode);
        end += words;
    }
    submission.num_draws = num_draws;
    submission.draw_hashes = std::make_unique<u64[]>(num_draws);
    const bool verify = VerifyMode() >= 2;
    if (verify) {
        submission.verify_regs = std::make_unique<std::unique_ptr<Regs>[]>(
            (num_draws + Submission::VerifyEvery - 1) / Submission::VerifyEvery);
    }
    void* block = nullptr;
    if (preparer && !scan_broken) {
        submission.prepared = preparer->NewBlock(num_draws);
        block = submission.prepared.get();
    }

    Regs& regs = *scan_regs;
    u64 hash = scan_hash;
    u32 draw = 0;
    for (size_t i = 0; i < end;) {
        const auto* header = reinterpret_cast<const PM4Header*>(&commands[i]);
        if (header->type == 2) {
            ++i;
            continue;
        }
        const PM4ItOpcode opcode = header->type3.opcode;
        HashStatePacket(header, hash);
        if (IsDrawOpcode(opcode)) {
            // The command processor compares before applying the draw packet.
            submission.draw_hashes[draw] = hash;
            if (verify && draw % Submission::VerifyEvery == 0 && !scan_broken) {
                submission.verify_regs[draw / Submission::VerifyEvery] =
                    std::make_unique<Regs>(regs);
            }
        }
        if (IsStatePacket(opcode) && !scan_broken && !ApplyStatePacket(regs, header)) {
            scan_broken = true;
            std::fprintf(stderr, "SOTC drawscan: register write past the register file, "
                                 "scanned registers no longer used\n");
        }
        if (IsDrawOpcode(opcode)) {
            // Selection runs after the draw packet's own register writes.
            if (block && !scan_broken) {
                preparer->Prepare(block, draw, regs, DrawParamsOf(header));
            }
            submission.num_ready.store(draw + 1, std::memory_order_release);
            ++draw;
        }
        i += header->type3.NumWords() + 1;
    }
    scan_hash = hash;
}

void DrawScan::Begin(u64 seq) {
    std::scoped_lock lk{mutex};
    current.reset();
    current_draw = 0;
    for (const auto& submission : recent) {
        if (submission->seq == seq) {
            current = submission;
            break;
        }
    }
}

void DrawScan::End() {
    std::scoped_lock lk{mutex};
    if (current) {
        std::erase(recent, current);
        if (retired.size() < 1024) {
            retired.push_back(std::move(current));
        }
    }
    current.reset();
}

const void* DrawScan::OnDraw(u64 hash, const Regs& regs) {
    const u32 ordinal = current_draw++;
    if (!current) {
        SotcStats::scan_lost.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    const Submission& s = *current;
    if (ordinal >= s.num_ready.load(std::memory_order_acquire)) {
        (s.scanned.load(std::memory_order_acquire) ? SotcStats::scan_mismatch
                                                   : SotcStats::scan_late)
            .fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    if (s.draw_hashes[ordinal] != hash) {
        SotcStats::scan_mismatch.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    SotcStats::Bump(SotcStats::scan_agree);
    if (s.verify_regs && ordinal % Submission::VerifyEvery == 0) {
        if (const auto& snap = s.verify_regs[ordinal / Submission::VerifyEvery]) {
            SotcStats::scan_regs_checked.fetch_add(1, std::memory_order_relaxed);
            for (u32 w = 0; w < Regs::NumRegs; ++w) {
                if (snap->reg_array[w] != regs.reg_array[w]) {
                    SotcStats::scan_regs_differ.fetch_add(1, std::memory_order_relaxed);
                    static std::atomic<u32> shown{0};
                    if (shown.fetch_add(1, std::memory_order_relaxed) < 32) {
                        std::fprintf(stderr,
                                     "SOTC drawscan verify: register word %#x differs (scanner "
                                     "%#x, command processor %#x), buffer %llu draw %u\n",
                                     w, snap->reg_array[w], regs.reg_array[w],
                                     static_cast<unsigned long long>(s.seq), ordinal);
                    }
                    break;
                }
            }
        }
    }
    if (!s.prepared || !preparer) {
        return nullptr;
    }
    return preparer->Record(s.prepared.get(), ordinal);
}

} // namespace AmdGpu
