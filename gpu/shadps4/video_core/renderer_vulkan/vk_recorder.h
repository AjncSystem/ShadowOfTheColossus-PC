// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: taken from Pink-shadPS4 0.4.0 (luizgustavs), cp_record_thread; scheduler hooks in vk_scheduler.cpp.

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;
class Semaphore;
class PassSecondary;

/// sotc: the attachment formats and sample count of a dynamic rendering pass, for the inheritance
/// info of the secondary command buffer that records it (SOTC_PASS_SEC). The scheduler sets it right
/// before the BeginRendering it describes; that block takes it.
struct PassFormats {
    bool valid{};
    u32 num_colors{};
    std::array<VkFormat, 8> colors{};
    VkFormat depth{};
    VkFormat stencil{};
    VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};
};

/// sotc: identity of a state-setting block. A later block with the same identity supersedes the
/// earlier one in the replay table of a secondary command buffer (vk_recorder.cpp, PassSecondary):
/// tag is the Vulkan command, a..c the arguments that select which part of the state it sets.
struct StateKey {
    u32 tag{};
    u32 a{};
    u32 b{};
    u32 c{};

    bool operator==(const StateKey& other) const noexcept = default;
};

/// With cp_record_thread, record Vulkan commands and submit them on a separate thread from the
/// command processor
/// The scheduler gives the producer fake command buffer handles, and dispatcher handlers copy their
/// arguments into an ordered stream
/// The consumer replays that stream on real command buffers, while presenter, ImGui and ahead
/// copies using real handles call the driver directly
///
/// Only one producer owns the scheduler at a time and one consumer reads the stream of 256 KiB
/// chunks
/// Data blocks come before the commands using them, and a command may still point into the
/// preceding chunk
/// Recycle a chunk only after the consumer leaves the following chunk so those references remain
/// valid, and only once no pin (sotc: see Chunk) still keeps it
/// Each producer write is published before the consumer reads it, keeping the stream in the order
/// used by the command processor
/// Copied arrays stay inside these chunks until their commands have run, so the producer does not
/// have to keep the original argument memory alive
class CommandRecorder {
public:
    /// Kind of recorded work, set by each thunk; the SOTC_CMD_STATS counts use it
    enum class BlockCategory : u8 {
        Draw,
        Dispatch,
        BeginRendering,
        EndRendering,
        Barrier,
        Copy,
        BindPipeline,
        BindDescriptors,
        PushConstants,
        DynamicState,
        VertexIndex,
        Other,
        Label, ///< sotc: debug utils labels, allowed inside a secondary-recorded pass
        Count,
    };

    /// sotc: instance and work_semaphore are used by the secondary command buffer pool (SOTC_PASS_SEC)
    CommandRecorder(const Instance& instance, Semaphore* work_semaphore);
    ~CommandRecorder();

    CommandRecorder(const CommandRecorder&) = delete;
    CommandRecorder& operator=(const CommandRecorder&) = delete;

    /// Install command handlers once after the default dispatcher has loaded the device functions
    /// Copy ordinary arguments with the generic handler and pointer arguments with dedicated deep-
    /// copy handlers
    /// Stop and report the command name if a fake handle reaches a command whose pointer arguments
    /// are unsupported
    static void InstallThunks();

    [[nodiscard]] static bool IsFake(VkCommandBuffer cmdbuf) noexcept;

    /// Give the producer a fake handle for a command buffer that the consumer will allocate later
    /// The queued closure calls Bind to connect it with the real buffer before recording its
    /// commands
    [[nodiscard]] vk::CommandBuffer NewFakeHandle();

    /// Queue func for the recording thread after every operation already in the stream so the
    /// producer can preserve command order
    /// category says which kind of work func is, for the SOTC_CMD_STATS counts
    template <typename F>
    void Run(BlockCategory category, F&& func) {
        using T = std::decay_t<F>;
        static_assert(alignof(T) <= BlockAlign, "Closure over-aligned for the stream");
        void* payload = Allocate(sizeof(T), BlockKind::Command, category, &Invoke<T>);
        new (payload) T(std::forward<F>(func));
        Publish();
    }

    /// sotc: Run for a state-setting command of cmd (a fake handle). The block carries cmd and key so
    /// the consumer can keep the latest one per key and replay it inside a secondary command buffer.
    /// The closure runs in place again on a replay, so it must be trivially destructible (captures
    /// of handles, integers and pointers into the stream).
    template <typename F>
    void RunState(BlockCategory category, VkCommandBuffer cmd, StateKey key, F&& func) {
        using T = std::decay_t<F>;
        static_assert(alignof(T) <= BlockAlign, "Closure over-aligned for the stream");
        static_assert(std::is_trivially_destructible_v<T>,
                      "State closures are replayed in place and must not need destruction");
        // sotc: without SOTC_PASS_SEC no metadata is recorded, the block is an ordinary command
        void* payload = state_meta
                            ? AllocateState(sizeof(T), category, &Invoke<T>, cmd, key, 0)
                            : Allocate(sizeof(T), BlockKind::Command, category, &Invoke<T>);
        new (payload) T(std::forward<F>(func));
        Publish();
    }

    /// Reserve a command block of size bytes for invoke(payload) to run on the recording thread
    /// Write the arguments and any referenced arrays into that block, then call Commit before
    /// making another recorder call
    /// Keeping the arguments with the command lets the caller reuse its original memory after
    /// publication
    /// sotc: cmd and key make it a state block (see RunState) when SOTC_PASS_SEC is on; aux marks
    /// special blocks, see kBeginPassAux
    [[nodiscard]] void* AllocateCommand(size_t size, BlockCategory category,
                                        void (*invoke)(void* payload), VkCommandBuffer cmd = nullptr,
                                        StateKey key = {}, u16 aux = 0) {
        if (state_meta && cmd != nullptr) {
            return AllocateState(size, category, invoke, cmd, key, aux);
        }
        return Allocate(size, BlockKind::Command, category, invoke, aux);
    }

    /// Publish the block reserved by AllocateCommand so the consumer can run it in the same ordered
    /// stream used by Run
    void Commit() {
        Publish();
    }

    /// sotc: aux value of the BeginRendering blocks, whose payload is a BeginPayload (vk_recorder.cpp)
    static constexpr u16 kBeginPassAux = 1;

    /// sotc: the producer describes the dynamic rendering pass its next BeginRendering starts
    void SetPassFormats(const PassFormats& formats) noexcept {
        pending_formats = formats;
        pending_formats.valid = true;
    }

    /// sotc: the consumer submitted the fake command buffers fakes with the GPU tick signal_tick. Drops
    /// their replay state and frees the pass secondaries executed in them once that tick completes
    /// (consumer thread, vk_recorder.cpp)
    void PassSubmitted(std::span<const vk::CommandBuffer> fakes, u64 signal_tick);

    /// sotc: taken by the BeginRendering thunk; invalid unless SetPassFormats came right before it
    [[nodiscard]] PassFormats TakePassFormats() noexcept {
        PassFormats formats = pending_formats;
        pending_formats.valid = false;
        return formats;
    }

    /// Copy count elements into the stream for the next queued command so the producer can release
    /// the original array, preserving null pointers
    template <typename T>
    [[nodiscard]] T* Copy(const T* src, size_t count = 1) {
        if (src == nullptr || count == 0) {
            return nullptr;
        }
        static_assert(std::is_trivially_copyable_v<T>);
        void* dst = Allocate(sizeof(T) * count, BlockKind::Data, BlockCategory::Other, nullptr);
        std::memcpy(dst, src, sizeof(T) * count);
        return static_cast<T*>(dst);
    }

    /// Reserve enough room in the current chunk for the next data and command blocks so their bytes
    /// stay together until the consumer finishes
    void Reserve(size_t bytes);

    /// Publish all queued work and wake the recording thread if it is sleeping at this submit point
    /// Between kicks, publish smaller batches for the consumer to pick up while spinning or after
    /// its short timed wait
    /// A Kick makes every command already queued visible before the submit or drain can continue
    void Kick();

    /// Count this deferred Vulkan call for performance statistics and report the accumulated calls
    /// at the next Kick
    void CountCommand() noexcept {
        ++commands_counted;
    }

    /// Wait until the recording thread has run every operation queued so far before the producer
    /// continues with work needing those results
    void Sync();

    /// Make the calling thread the producer when shutdown moves recording to another thread
    /// The previous producer must already have stopped, and any other unexpected producer change
    /// stops the recorder
    void ClaimProducer();

    /// Connect a fake command buffer handle to the real buffer allocated by the consumer before
    /// queued commands use it
    void Bind(VkCommandBuffer fake, VkCommandBuffer real) noexcept;

    /// Look up the real command buffer belonging to this fake handle when the consumer replays its
    /// queued commands
    /// sotc: on the secondary worker thread, the secondary command buffer being recorded for its fake
    /// handle takes the place of the real one (thread-local override, vk_recorder.cpp)
    [[nodiscard]] VkCommandBuffer Real(VkCommandBuffer fake) const noexcept;

    /// Release count fake handle sessions after the consumer has submitted them so the producer can
    /// reuse those slots later
    void Retire(u32 count) noexcept {
        handles_retired.fetch_add(count, std::memory_order_release);
    }

    [[nodiscard]] bool IsRecordingThread() const noexcept {
        return std::this_thread::get_id() == thread.get_id();
    }

    static constexpr size_t BlockAlign = 16;
    static constexpr u32 NumFakeHandles = 1U << 16;

    /// sotc: state-setting categories, the blocks a secondary command buffer replays
    static constexpr bool IsStateCategory(BlockCategory category) noexcept {
        return category == BlockCategory::BindPipeline ||
               category == BlockCategory::BindDescriptors ||
               category == BlockCategory::PushConstants ||
               category == BlockCategory::DynamicState || category == BlockCategory::VertexIndex;
    }

private:
    /// Meta: StateMeta of the Command block right after it (SOTC_PASS_SEC only, see AllocateState)
    enum class BlockKind : u8 { Data, Command, End, Meta };

    struct BlockHeader {
        u32 size; ///< Total block size including its header, rounded to a multiple of BlockAlign for the next block
        BlockKind kind;
        BlockCategory category;
        u16 aux;
        void (*invoke)(void* payload);
    };
    static_assert(sizeof(BlockHeader) == BlockAlign);

    /// sotc: the identity of a state block, written as a Meta block before it with SOTC_PASS_SEC on
    struct StateMeta {
        VkCommandBuffer cmd;
        StateKey key;
    };
    static_assert(sizeof(StateMeta) == 24);

    struct Chunk {
        static constexpr size_t Size = 256_KB;
        std::atomic<u32> committed{0};
        std::atomic<Chunk*> next{nullptr};
        /// sotc: pins held by a secondary command buffer job that still reads this chunk
        std::atomic<u32> job_pins{0};
        /// sotc: pins held by replay table entries; only the consumer thread changes it
        u32 table_pins{0};
        alignas(64) u8 data[Size];
    };

    template <typename T>
    static void Invoke(void* payload) {
        T& func = *static_cast<T*>(payload);
        func();
        func.~T();
    }

    void* Allocate(size_t size, BlockKind kind, BlockCategory category, void (*invoke)(void*),
                   u16 aux = 0);
    /// sotc: a Meta block with cmd and key, then the Command block, in one chunk
    void* AllocateState(size_t size, BlockCategory category, void (*invoke)(void*),
                        VkCommandBuffer cmd, StateKey key, u16 aux);
    void Publish();
    void PublishAll();
    void NextChunk();
    Chunk* AcquireChunk();
    void RecycleChunk(Chunk* chunk);
    /// sotc: recycle a chunk the consumer left, or keep it in deferred_chunks while a pin holds it
    void ReleaseChunk(Chunk* chunk);
    void SweepDeferred();
    void WakeConsumer();
    void WakeIfSleeping();
    void CheckProducer();
    void ConsumerLoop(std::stop_token stoken);

    friend class PassSecondary;

    static constexpr size_t MaxChunksInFlight = 256; ///< Allow up to 64 MiB of queued commands before the producer waits for the recording thread to
///< catch up
    static constexpr u32 PublishBatch = 32;

    // State written by the producer while it adds commands and publishes batches to the recording
    // thread
    Chunk* write_chunk{};
    u32 write_pos{};
    u64 handles_issued{};
    u64 sync_requested{};
    u64 commands_counted{};
    u32 unpublished{};
    u32 producer_tid{};
    u32 producer_switches{};
    PassFormats pending_formats{};
    /// sotc: true when SOTC_PASS_SEC is on; state blocks then carry a Meta block (set in the ctor)
    bool state_meta{};

    // State written by the consumer while it runs queued commands and advances through the stream
    // chunks
    std::array<VkCommandBuffer, NumFakeHandles> real_handles{};
    /// sotc: chunks the consumer left while a pin still held them (consumer thread only)
    std::vector<Chunk*> deferred_chunks;
    /// sotc: SOTC_PASS_SEC=1 only: runs the dynamic rendering passes on secondary command buffers
    std::unique_ptr<PassSecondary> pass;

    // State shared by both threads to track queued work, sleeping and the recorder lifetime
    std::atomic<u64> handles_retired{0};
    std::atomic<u64> sync_done{0};
    std::atomic<u32> chunks_in_flight{0};
    std::atomic<bool> consumer_sleeping{false};
    std::mutex wake_mutex;
    std::condition_variable wake_cv;
    bool wake_flag{};
    /// sotc: SOTC_WAIT_NOSPIN doorbell the consumer blocks on (std::atomic::wait), bumped by
    /// WakeConsumer. A stop request has to change a waited-on value, so it cannot be the
    /// committed write position alone.
    std::atomic<u32> wake_seq{0};
    std::mutex free_mutex;
    std::vector<Chunk*> free_chunks;
    Chunk* first_chunk{};
    std::jthread thread;
};

} // namespace Vulkan
