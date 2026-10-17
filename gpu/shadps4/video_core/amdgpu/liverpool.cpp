// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <utility>
#include <cstdlib>
#include <boost/preprocessor/stringize.hpp>

#include "common/assert.h"
#include "common/debug.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/process.h"
#include "core/libraries/videoout/driver.h"
#include "core/memory.h"
#include "core/platform.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/renderdoc.h"
#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/sotc_lod_stats.h"
#include "video_core/sotc_stats.h"

// sotc: writes guest memory through the backing view (runtime_memory.c).
extern "C" int runtime_memory_write_backing(unsigned long long address, const void* data,
                                            unsigned long long size);

// Counted for the SOTC_PROBE diagnostic log (emulator.cpp).
std::atomic<u64> g_sotc_zpass_dumps{0};

namespace AmdGpu {

static const char* dcb_task_name{"DCB_TASK"};
static const char* ccb_task_name{"CCB_TASK"};

#define MAX_NAMES 56
static_assert(Liverpool::NumComputeRings <= MAX_NAMES);

#define NAME_NUM(z, n, name) BOOST_PP_STRINGIZE(name) BOOST_PP_STRINGIZE(n),
#define NAME_ARRAY(name, num) {BOOST_PP_REPEAT(num, NAME_NUM, name)}

static const char* acb_task_name[] = NAME_ARRAY(ACB_TASK, MAX_NAMES);

#define YIELD(name)                                                                                \
    FIBER_EXIT;                                                                                    \
    co_yield {};                                                                                   \
    FIBER_ENTER(name);

#define YIELD_CE() YIELD(ccb_task_name)
#define YIELD_GFX() YIELD(dcb_task_name)
#define YIELD_ASC(id) YIELD(acb_task_name[id])

#define RESUME(task, name)                                                                         \
    FIBER_EXIT;                                                                                    \
    task.handle.resume();                                                                          \
    FIBER_ENTER(name);

#define RESUME_CE(task) RESUME(task, ccb_task_name)
#define RESUME_GFX(task) RESUME(task, dcb_task_name)
#define RESUME_ASC(task, id) RESUME(task, acb_task_name[id])

std::array<u8, 48_KB> Liverpool::ConstantEngine::constants_heap;

static std::span<const u32> NextPacket(std::span<const u32> span, size_t offset) {
    if (offset > span.size()) {
        LOG_ERROR(
            Lib_GnmDriver,
            ": packet length exceeds remaining submission size. Packet dword count={}, remaining "
            "submission dwords={}",
            offset, span.size());
        // Return empty subspan so check for next packet bails out
        return {};
    }

    return span.subspan(offset);
}

Liverpool::Liverpool() : guest_markers_enabled{EmulatorSettings.IsVkGuestMarkersEnabled()} {
    num_counter_pairs = Libraries::Kernel::sceKernelIsNeoMode() ? 16 : 8;
    // sotc: SOTC_CP_THREADS=0 binds and records the draws on the command processor thread.
    const char* threads = std::getenv("SOTC_CP_THREADS");
    back_threaded = !threads || threads[0] != '0';
    const char* strict = std::getenv("SOTC_CP_STRICT");
    back_strict = strict && strict[0] == '1';
    if (back_threaded) {
        back_ring.resize(BackRingSize);
        dirty_list.reserve(NumRegChunks);
        back_thread = std::jthread{std::bind_front(&Liverpool::BackLoop, this)};
        back_id = back_thread.get_id();
    }
    process_thread = std::jthread{std::bind_front(&Liverpool::Process, this)};
}

Liverpool::~Liverpool() {
    process_thread.request_stop();
    process_thread.join();
    if (back_thread.joinable()) {
        back_thread.request_stop();
        back_thread.join();
    }
}

static inline void CpuRelax() {
#if defined(__x86_64__) || defined(_M_X64)
    __builtin_ia32_pause();
#endif
}

void Liverpool::BackLoop(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuDraw");
    Shader::g_info_slot = 1;
    u64 tail = back_tail.load(std::memory_order_relaxed);
    while (!stoken.stop_requested()) {
        if (num_commands.load(std::memory_order_acquire)) {
            ProcessCommands();
        }
        if (back_head.load(std::memory_order_acquire) == tail) {
            const u64 idle_start = SotcStats::NowNs();
            struct IdleTime {
                u64 start;
                ~IdleTime() {
                    SotcStats::draw_idle_ns.fetch_add(SotcStats::NowNs() - start,
                                                      std::memory_order_relaxed);
                }
            } idle_time{idle_start};
            bool ready = false;
            for (u32 spin = 0; spin < 4096; ++spin) {
                if (back_head.load(std::memory_order_acquire) != tail ||
                    num_commands.load(std::memory_order_acquire)) {
                    ready = true;
                    break;
                }
                CpuRelax();
            }
            if (!ready) {
                std::unique_lock lk{back_mutex};
                back_sleeping.store(true, std::memory_order_seq_cst);
                back_cv.wait_for(lk, std::chrono::milliseconds(1), [&] {
                    return back_head.load(std::memory_order_seq_cst) != tail ||
                           num_commands.load(std::memory_order_seq_cst) ||
                           stoken.stop_requested();
                });
                back_sleeping.store(false, std::memory_order_relaxed);
            }
            continue;
        }
        BackOp& op = back_ring[tail % BackRingSize];
        ExecuteBackOp(op);
        op.func = {};
        ++tail;
        back_tail.store(tail, std::memory_order_seq_cst);
        if (drain_waiting.load(std::memory_order_seq_cst)) {
            std::scoped_lock lk{back_mutex};
            drain_cv.notify_all();
        }
    }
}

void Liverpool::DrainBack() {
    if (!back_threaded || std::this_thread::get_id() == back_id) {
        return;
    }
    WaitBackTail(back_head.load(std::memory_order_acquire));
}

void Liverpool::WaitBackTail(u64 head) {
    SotcStats::cp_waits.fetch_add(1, std::memory_order_relaxed);
    for (u32 spin = 0; back_tail.load(std::memory_order_acquire) < head; ++spin) {
        if (spin < 4096) {
            CpuRelax();
            continue;
        }
        std::unique_lock lk{back_mutex};
        drain_waiting.store(true, std::memory_order_seq_cst);
        drain_cv.wait_for(lk, std::chrono::milliseconds(1), [&] {
            return back_tail.load(std::memory_order_seq_cst) >= head;
        });
        drain_waiting.store(false, std::memory_order_relaxed);
    }
}

Liverpool::BackOp& Liverpool::BackAlloc() {
    const u64 head = back_head.load(std::memory_order_relaxed);
    for (u32 spin = 0; head - back_tail.load(std::memory_order_acquire) >= BackRingSize; ++spin) {
        if (spin < 4096) {
            CpuRelax();
            continue;
        }
        std::unique_lock lk{back_mutex};
        drain_waiting.store(true, std::memory_order_seq_cst);
        drain_cv.wait_for(lk, std::chrono::milliseconds(1), [&] {
            return head - back_tail.load(std::memory_order_seq_cst) < BackRingSize;
        });
        drain_waiting.store(false, std::memory_order_relaxed);
    }
    BackOp& op = back_ring[head % BackRingSize];
    op.pipeline = nullptr;
    op.num_stages = 0;
    // The registers as of this point of the command stream: the chunks changed since the last op.
    op.reg_chunks.clear();
    for (const u16 chunk : dirty_list) {
        const u32* src = &regs.reg_array[u32{chunk} * RegChunkDwords];
        op.reg_chunks.push_back(chunk);
        op.reg_chunks.insert(op.reg_chunks.end(), src, src + RegChunkDwords);
        dirty_chunks[chunk / 64] &= ~(u64{1} << (chunk % 64));
    }
    dirty_list.clear();
    op.cb_extent = last_cb_extent;
    op.db_extent = last_db_extent;
    return op;
}

void Liverpool::BackPublish(bool writes_memory) {
    const u64 head = back_head.load(std::memory_order_relaxed) + 1;
    back_head.store(head, std::memory_order_seq_cst);
    if (writes_memory) {
        back_write_seq = head;
    }
    if (back_sleeping.load(std::memory_order_seq_cst)) {
        std::scoped_lock lk{back_mutex};
        back_cv.notify_one();
    }
}

void Liverpool::QueueBackFunc(Common::UniqueFunction<void>&& func, bool writes_memory) {
    BackOp& op = BackAlloc();
    op.kind = BackOp::Kind::Func;
    op.func = std::move(func);
    BackPublish(writes_memory);
}

void Liverpool::CaptureBackOp(BackOp& op, const Vulkan::Pipeline* pipeline) {
    op.pipeline = pipeline;
    op.num_stages = 0;
    if (!pipeline) {
        return;
    }
    for (const Shader::Info* info : pipeline->GetStages()) {
        if (!info) {
            continue;
        }
        BackStage& stage = op.stages[op.num_stages++];
        const Shader::InfoRuntime& rt = info->runtime[0];
        stage.info = const_cast<Shader::Info*>(info);
        if (rt.user_data.data() == nullptr) {
            stage.num_user_data = ~0u;
        } else {
            stage.num_user_data = static_cast<u32>(std::min<size_t>(rt.user_data.size(), 16));
            std::memcpy(stage.user_data.data(), rt.user_data.data(),
                        stage.num_user_data * sizeof(u32));
        }
        stage.pgm_base = rt.pgm_base;
        stage.flat.assign(rt.flattened_ud_buf.begin(), rt.flattened_ud_buf.end());
    }
}

/// A draw or dispatch with this pipeline may write guest memory (storage buffers or images).
static bool PipelineWrites(const Vulkan::Pipeline* pipeline) {
    if (!pipeline) {
        return false;
    }
    if (pipeline->IsCompute()) {
        return true;
    }
    for (const Shader::Info* info : pipeline->GetStages()) {
        if (!info) {
            continue;
        }
        if (info->uses_dma || info->has_storage_images) {
            return true;
        }
        for (const auto& buffer : info->buffers) {
            if (buffer.is_written) {
                return true;
            }
        }
        for (const auto& image : info->images) {
            if (image.is_written) {
                return true;
            }
        }
    }
    return false;
}

void Liverpool::ExecuteBackOp(BackOp& op) {
    for (size_t i = 0; i < op.reg_chunks.size(); i += RegChunkDwords + 1) {
        std::memcpy(&back_regs.reg_array[op.reg_chunks[i] * RegChunkDwords], &op.reg_chunks[i + 1],
                    RegChunkDwords * sizeof(u32));
    }
    back_cb_extent = op.cb_extent;
    back_db_extent = op.db_extent;
    for (u32 i = 0; i < op.num_stages; ++i) {
        BackStage& stage = op.stages[i];
        Shader::InfoRuntime& rt = stage.info->runtime[1];
        rt.user_data = stage.num_user_data == ~0u
                           ? std::span<const u32>{}
                           : std::span<const u32>{stage.user_data.data(), stage.num_user_data};
        rt.flattened_ud_buf.assign(stage.flat.begin(), stage.flat.end());
        rt.pgm_base = stage.pgm_base;
    }
    switch (op.kind) {
    case BackOp::Kind::Func:
        op.func();
        break;
    case BackOp::Kind::Draw:
        rasterizer->Draw(op.is_indexed, op.index_offset, op.pipeline, true);
        break;
    case BackOp::Kind::DrawIndirect:
        rasterizer->DrawIndirect(op.is_indexed, op.address, op.offset, op.stride, op.max_count,
                                 op.count_address, op.vertex_sgpr_offset, op.instance_sgpr_offset,
                                 op.pipeline, true);
        break;
    case BackOp::Kind::Dispatch:
        back_cs = op.cs;
        rasterizer->DispatchDirect(op.pipeline, true);
        break;
    case BackOp::Kind::DispatchIndirect:
        back_cs = op.cs;
        rasterizer->DispatchIndirect(op.address, op.offset, op.size, op.pipeline, true);
        break;
    }
}

void Liverpool::SubmitDraw(bool is_indexed, u32 index_offset) {
    if (!back_threaded) {
        rasterizer->Draw(is_indexed, index_offset);
        return;
    }
    WaitBackWrites();
    const auto* pipeline = rasterizer->LookupGraphicsPipeline(0, 0);
    BackOp& op = BackAlloc();
    op.kind = BackOp::Kind::Draw;
    op.is_indexed = is_indexed;
    op.index_offset = index_offset;
    CaptureBackOp(op, pipeline);
    BackPublish(back_strict && PipelineWrites(pipeline));
}

void Liverpool::SubmitDrawIndirect(bool is_indexed, VAddr address, u32 offset, u32 stride,
                                   u32 max_count, VAddr count_address, u16 vertex_sgpr_offset,
                                   u16 instance_sgpr_offset) {
    if (!back_threaded) {
        rasterizer->DrawIndirect(is_indexed, address, offset, stride, max_count, count_address,
                                 vertex_sgpr_offset, instance_sgpr_offset);
        return;
    }
    WaitBackWrites();
    const auto* pipeline =
        rasterizer->LookupGraphicsPipeline(vertex_sgpr_offset, instance_sgpr_offset);
    BackOp& op = BackAlloc();
    op.kind = BackOp::Kind::DrawIndirect;
    op.is_indexed = is_indexed;
    op.address = address;
    op.offset = offset;
    op.stride = stride;
    op.max_count = max_count;
    op.count_address = count_address;
    op.vertex_sgpr_offset = vertex_sgpr_offset;
    op.instance_sgpr_offset = instance_sgpr_offset;
    CaptureBackOp(op, pipeline);
    BackPublish(back_strict && PipelineWrites(pipeline));
}

void Liverpool::SubmitDispatch(bool indirect, VAddr address, u32 offset, u32 size) {
    if (!back_threaded) {
        if (indirect) {
            rasterizer->DispatchIndirect(address, offset, size);
        } else {
            rasterizer->DispatchDirect();
        }
        return;
    }
    WaitBackWrites();
    const auto* pipeline = rasterizer->LookupComputePipeline();
    BackOp& op = BackAlloc();
    op.kind = indirect ? BackOp::Kind::DispatchIndirect : BackOp::Kind::Dispatch;
    op.address = address;
    op.offset = offset;
    op.size = size;
    op.cs = GetCsRegs();
    CaptureBackOp(op, pipeline);
    BackPublish(back_strict);
}

void Liverpool::WriteFenceValue(void* address, u64 data, u32 num_bytes) {
    // The value is written to the backing memory directly (the GPU may target pages the guest
    // mapped read-only), which the memory tracker cannot see. Invalidate first so a copy of the
    // page already uploaded to the GPU is not reused with the old value, and so GPU-written data
    // sharing the page is read back before the new value lands on top of it.
    if (rasterizer) {
        rasterizer->InvalidateMemory(std::bit_cast<VAddr>(address), num_bytes, true);
    }
    auto* memory = Core::Memory::Instance();
    ASSERT(memory->TryWriteBacking(address, &data, num_bytes, 2));
}

void Liverpool::ProcessCommands() {
    // sotc: with the draw thread, the commands run there between two queued operations (guest
    // threads waiting for a readback do not wait for the queue), and the ordered ones are queued
    // here, at this point of the command stream.
    if (back_threaded) {
        if (std::this_thread::get_id() == back_id) {
            while (num_commands) {
                Common::UniqueFunction<void> callback{};
                {
                    std::scoped_lock lk{submit_mutex};
                    callback = std::move(command_queue.front());
                    command_queue.pop();
                    --num_commands;
                }
                callback();
            }
            return;
        }
        while (num_ordered) {
            Common::UniqueFunction<void> callback{};
            {
                std::scoped_lock lk{submit_mutex};
                callback = std::move(ordered_queue.front());
                ordered_queue.pop();
                --num_ordered;
            }
            QueueBackFunc(std::move(callback), back_strict);
        }
        return;
    }
    // Process incoming commands with high priority
    while (num_commands) {
        Common::UniqueFunction<void> callback{};
        {
            std::scoped_lock lk{submit_mutex};
            callback = std::move(command_queue.front());
            command_queue.pop();
            --num_commands;
        }
        callback();
    }
}

void Liverpool::Process(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuCommandProcessor");
    gpu_id = std::this_thread::get_id();
#ifdef __linux__
    gpu_tid = gettid();
#endif

    while (!stoken.stop_requested()) {
        {
            const SotcStats::FrameTimer idle_timer{SotcStats::FrameIdle};
            std::unique_lock lk{submit_mutex};
            Common::CondvarWait(submit_cv, lk, stoken,
                                [this] {
                                    return (num_commands && !back_threaded) || num_ordered ||
                                           num_submits || submit_done;
                                });
        }
        if (stoken.stop_requested()) {
            break;
        }

        VideoCore::StartCapture();

        curr_qid = -1;

        while (num_submits || (num_commands && !back_threaded) || num_ordered) {
            ProcessCommands();

            curr_qid = (curr_qid + 1) % num_mapped_queues;

            auto& queue = mapped_queues[curr_qid];

            Task::Handle task{};
            {
                std::scoped_lock lock{queue.m_access};
                if (queue.submits.empty()) {
                    continue;
                }
                task = queue.submits.front();
            }
            const u64 parse_start = SotcStats::NowNs();
            task.resume();
            SotcStats::cp_parse_ns.fetch_add(SotcStats::NowNs() - parse_start,
                                             std::memory_order_relaxed);

            if (task.done()) {
                task.destroy();

                std::scoped_lock lock{queue.m_access};
                queue.submits.pop();

                --num_submits;
                std::scoped_lock lock2{submit_mutex};
                submit_cv.notify_all();
            }
        }

        if (submit_done) {
            VideoCore::EndCapture();
            if (rasterizer) {
                BackOrRun(
                    [this] {
                        rasterizer->OnSubmit();
                        rasterizer->Flush();
                    },
                    false);
            }
            submit_done = false;
        }

        BackOrRun([] { Platform::IrqC::Instance()->Signal(Platform::InterruptId::GpuIdle); },
                  false);
    }
}

Liverpool::Task Liverpool::ProcessCeUpdate(std::span<const u32> ccb) {
    FIBER_ENTER(ccb_task_name);

    while (!ccb.empty()) {
        ProcessCommands();

        const auto* header = reinterpret_cast<const PM4Header*>(ccb.data());
        const u32 type = header->type;
        if (type != 3) {
            // No other types of packets were spotted so far
            UNREACHABLE_MSG("Invalid PM4 type {}", type);
        }

        const PM4ItOpcode opcode = header->type3.opcode;
        const auto* it_body = reinterpret_cast<const u32*>(header) + 1;
        switch (opcode) {
        case PM4ItOpcode::Nop: {
            // const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
            break;
        }
        case PM4ItOpcode::WriteConstRam: {
            const auto* write_const = reinterpret_cast<const PM4WriteConstRam*>(header);
            memcpy(cblock.constants_heap.data() + write_const->Offset(), &write_const->data,
                   write_const->Size());
            break;
        }
        case PM4ItOpcode::DumpConstRam: {
            const auto* dump_const = reinterpret_cast<const PM4DumpConstRam*>(header);
            DrainBack(); // sotc: queued draws may still read the old constants
            memcpy(dump_const->Address<void*>(),
                   cblock.constants_heap.data() + dump_const->Offset(), dump_const->Size());
            Core::MemoryManager::NoteHostWrite(dump_const->Address<VAddr>(), dump_const->Size(), 5);
            break;
        }
        case PM4ItOpcode::IncrementCeCounter: {
            ++cblock.ce_count;
            break;
        }
        case PM4ItOpcode::WaitOnDeCounterDiff: {
            const auto diff = it_body[0];
            while ((cblock.de_count - cblock.ce_count) >= diff) {
                YIELD_CE();
            }
            break;
        }
        case PM4ItOpcode::IndirectBufferConst: {
            const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
            auto task =
                ProcessCeUpdate({indirect_buffer->Address<const u32>(), indirect_buffer->ib_size});
            RESUME_CE(task);

            while (!task.handle.done()) {
                YIELD_CE();
                RESUME_CE(task);
            }
            break;
        }
        default:
            const u32 count = header->type3.NumWords();
            UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                            static_cast<u32>(opcode), count);
        }
        ccb = NextPacket(ccb, header->type3.NumWords() + 1);
    }

    FIBER_EXIT;
}

Liverpool::Task Liverpool::ProcessGraphics(std::span<const u32> dcb, std::span<const u32> ccb) {
    FIBER_ENTER(dcb_task_name);

    cblock.Reset();

    // TODO: potentially, ASCs also can depend on CE and in this case the
    // CE task should be moved into more global scope
    Task ce_task{};

    if (!ccb.empty()) {
        // In case of CCB provided kick off CE asap to have the constant heap ready to use
        ce_task = ProcessCeUpdate(ccb);
        RESUME_GFX(ce_task);
    }

    const auto base_addr = reinterpret_cast<uintptr_t>(dcb.data());
    while (!dcb.empty()) {
        ProcessCommands();

        const auto* header = reinterpret_cast<const PM4Header*>(dcb.data());
        const u32 type = header->type;

        switch (type) {
        default:
            UNREACHABLE_MSG("Wrong PM4 type {}", type);
            break;
        case 0:
            UNREACHABLE_MSG("Unimplemented PM4 type 0, base reg: {}, size: {}",
                            header->type0.base.Value(), header->type0.NumWords());
            break;
        case 2:
            // Type-2 packet are used for padding purposes
            dcb = NextPacket(dcb, 1);
            continue;
        case 3:
            const u32 count = header->type3.NumWords();
            const PM4ItOpcode opcode = header->type3.opcode;
            switch (opcode) {
            case PM4ItOpcode::Nop: {
                const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
                if (nop->header.count.Value() == 0) {
                    break;
                }

                switch (nop->data_block[0]) {
                case PM4CmdNop::PayloadType::PatchedFlip: {
                    // There is no evidence that GPU CP drives flip events by parsing
                    // special NOP packets. For convenience lets assume that it does.
                    BackOrRun(
                        [] { Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxFlip); },
                        false);
                    break;
                }
                case PM4CmdNop::PayloadType::DebugMarkerPush: {
                    if (guest_markers_enabled && !back_threaded) {
                        const auto marker_sz = nop->header.count.Value() * 2;
                        const std::string_view label{
                            reinterpret_cast<const char*>(&nop->data_block[1]), marker_sz};
                        rasterizer->ScopeMarkerBegin(label, true);
                    }
                    break;
                }
                case PM4CmdNop::PayloadType::DebugColorMarkerPush: {
                    if (guest_markers_enabled && !back_threaded) {
                        const auto marker_sz = nop->header.count.Value() * 2;
                        const std::string_view label{
                            reinterpret_cast<const char*>(&nop->data_block[1]), marker_sz};
                        const u32 color = *reinterpret_cast<const u32*>(
                            reinterpret_cast<const u8*>(&nop->data_block[1]) + marker_sz);
                        rasterizer->ScopedMarkerInsertColor(label, color, true);
                    }
                    break;
                }
                case PM4CmdNop::PayloadType::DebugMarkerPop: {
                    if (guest_markers_enabled && !back_threaded) {
                        rasterizer->ScopeMarkerEnd(true);
                    }
                    break;
                }
                default:
                    break;
                }
                break;
            }
            case PM4ItOpcode::ContextControl: {
                break;
            }
            case PM4ItOpcode::ClearState: {
                regs.SetDefaults();
                MarkRegs(0, Regs::NumRegs);
                break;
            }
            case PM4ItOpcode::SetConfigReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                const auto reg_addr = Regs::ConfigRegWordOffset + set_data->reg_offset;
                const auto* payload = reinterpret_cast<const u32*>(header + 2);
                std::memcpy(&regs.reg_array[reg_addr], payload, (count - 1) * sizeof(u32));
                MarkRegs(reg_addr, count - 1);
                break;
            }
            case PM4ItOpcode::SetContextReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                const auto reg_addr = Regs::ContextRegWordOffset + set_data->reg_offset;
                const auto* payload = reinterpret_cast<const u32*>(header + 2);

                std::memcpy(&regs.reg_array[reg_addr], payload, (count - 1) * sizeof(u32));
                MarkRegs(reg_addr, count - 1);

                // In the case of HW, render target memory has alignment as color block operates on
                // tiles. There is no information of actual resource extents stored in CB context
                // regs, so any deduction of it from slices/pitch will lead to a larger surface
                // created. The same applies to the depth targets. Fortunately, the guest always
                // sends a trailing NOP packet right after the context regs setup, so we can use the
                // heuristic below and extract the hint to determine actual resource dims.

                switch (reg_addr) {
                case ContextRegs::CbColor0Base:
                case ContextRegs::CbColor1Base:
                case ContextRegs::CbColor2Base:
                case ContextRegs::CbColor3Base:
                case ContextRegs::CbColor4Base:
                case ContextRegs::CbColor5Base:
                case ContextRegs::CbColor6Base:
                case ContextRegs::CbColor7Base: {
                    const auto col_buf_id = (reg_addr - ContextRegs::CbColor0Base) /
                                            (ContextRegs::CbColor1Base - ContextRegs::CbColor0Base);
                    ASSERT(col_buf_id < NUM_COLOR_BUFFERS);

                    const auto nop_offset = header->type3.count;
                    if (nop_offset == 0x0e || nop_offset == 0x0d || nop_offset == 0x0b) {
                        ASSERT_MSG(payload[nop_offset] == 0xc0001000,
                                   "NOP hint is missing in CB setup sequence");
                        last_cb_extent[col_buf_id].raw = payload[nop_offset + 1];
                    } else {
                        last_cb_extent[col_buf_id].raw = 0;
                    }
                    break;
                }
                case ContextRegs::CbColor0Cmask:
                case ContextRegs::CbColor1Cmask:
                case ContextRegs::CbColor2Cmask:
                case ContextRegs::CbColor3Cmask:
                case ContextRegs::CbColor4Cmask:
                case ContextRegs::CbColor5Cmask:
                case ContextRegs::CbColor6Cmask:
                case ContextRegs::CbColor7Cmask: {
                    const auto col_buf_id =
                        (reg_addr - ContextRegs::CbColor0Cmask) /
                        (ContextRegs::CbColor1Cmask - ContextRegs::CbColor0Cmask);
                    ASSERT(col_buf_id < NUM_COLOR_BUFFERS);

                    const auto nop_offset = header->type3.count;
                    if (nop_offset == 0x04) {
                        ASSERT_MSG(payload[nop_offset] == 0xc0001000,
                                   "NOP hint is missing in CB setup sequence");
                        last_cb_extent[col_buf_id].raw = payload[nop_offset + 1];
                    }
                    break;
                }
                case ContextRegs::DbZInfo: {
                    if (header->type3.count == 8) {
                        ASSERT_MSG(payload[20] == 0xc0001000,
                                   "NOP hint is missing in DB setup sequence");
                        last_db_extent.raw = payload[21];
                    } else {
                        last_db_extent.raw = 0;
                    }
                    break;
                }
                default:
                    break;
                }
                break;
            }
            case PM4ItOpcode::SetShReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                const auto set_size = (count - 1) * sizeof(u32);

                if (set_data->reg_offset >= 0x200 &&
                    set_data->reg_offset <= (0x200 + sizeof(ComputeProgram) / 4)) {
                    ASSERT(set_size <= sizeof(ComputeProgram));
                    auto* addr = reinterpret_cast<u32*>(&mapped_queues[GfxQueueId].cs_state) +
                                 (set_data->reg_offset - 0x200);
                    std::memcpy(addr, header + 2, set_size);
                } else {
                    std::memcpy(&regs.reg_array[Regs::ShRegWordOffset + set_data->reg_offset],
                                header + 2, set_size);
                    MarkRegs(Regs::ShRegWordOffset + set_data->reg_offset,
                             static_cast<u32>(set_size / 4));
                }
                break;
            }
            case PM4ItOpcode::SetUconfigReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                std::memcpy(&regs.reg_array[Regs::UconfigRegWordOffset + set_data->reg_offset],
                            header + 2, (count - 1) * sizeof(u32));
                MarkRegs(Regs::UconfigRegWordOffset + set_data->reg_offset, count - 1);
                break;
            }
            case PM4ItOpcode::SetPredication: {
                // sotc: once; the game sends it every frame.
                static bool reported = false;
                if (!std::exchange(reported, true)) {
                    LOG_WARNING(Render, "Unimplemented IT_SET_PREDICATION (reported once)");
                }
                break;
            }
            case PM4ItOpcode::IndexType: {
                const auto* index_type = reinterpret_cast<const PM4CmdDrawIndexType*>(header);
                regs.index_buffer_type.raw = index_type->raw;
                MarkRegs(&regs.index_buffer_type, sizeof(regs.index_buffer_type));
                break;
            }
            case PM4ItOpcode::DrawIndex2: {
                const auto* draw_index = reinterpret_cast<const PM4CmdDrawIndex2*>(header);
                regs.max_index_size = draw_index->max_size;
                regs.index_base_address.base_addr_lo = draw_index->index_base_lo;
                regs.index_base_address.base_addr_hi = draw_index->index_base_hi;
                regs.num_indices = draw_index->index_count;
                regs.draw_initiator = draw_index->draw_initiator;
                MarkRegs(&regs.max_index_size, sizeof(regs.max_index_size));
                MarkRegs(&regs.index_base_address, sizeof(regs.index_base_address));
                MarkRegs(&regs.num_indices, sizeof(regs.num_indices));
                MarkRegs(&regs.draw_initiator, sizeof(regs.draw_initiator));
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker("gfx:{}:DrawIndex2", fmt::make_format_args(cmd_address),
                                        [&] { SubmitDraw(true, 0); });
                break;
            }
            case PM4ItOpcode::DrawIndexOffset2: {
                const auto* draw_index_off =
                    reinterpret_cast<const PM4CmdDrawIndexOffset2*>(header);
                regs.max_index_size = draw_index_off->max_size;
                regs.num_indices = draw_index_off->index_count;
                regs.draw_initiator = draw_index_off->draw_initiator;
                MarkRegs(&regs.max_index_size, sizeof(regs.max_index_size));
                MarkRegs(&regs.num_indices, sizeof(regs.num_indices));
                MarkRegs(&regs.draw_initiator, sizeof(regs.draw_initiator));
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndexOffset2", fmt::make_format_args(cmd_address),
                    [&] { SubmitDraw(true, draw_index_off->index_offset); });
                break;
            }
            case PM4ItOpcode::DrawIndexAuto: {
                const auto* draw_index = reinterpret_cast<const PM4CmdDrawIndexAuto*>(header);
                regs.num_indices = draw_index->index_count;
                regs.draw_initiator = draw_index->draw_initiator;
                MarkRegs(&regs.num_indices, sizeof(regs.num_indices));
                MarkRegs(&regs.draw_initiator, sizeof(regs.draw_initiator));
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker("gfx:{}:DrawIndexAuto", fmt::make_format_args(cmd_address),
                                        [&] { SubmitDraw(false, 0); });
                break;
            }
            case PM4ItOpcode::DrawIndirect: {
                const auto* draw_indirect = reinterpret_cast<const PM4CmdDrawIndirect*>(header);
                const auto offset = draw_indirect->data_offset;
                const auto stride = sizeof(DrawIndirectArgs);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndirect", fmt::make_format_args(cmd_address), [&] {
                        SubmitDrawIndirect(false, indirect_args_addr, offset, stride, 1, 0,
                                           draw_indirect->base_vtx_loc,
                                           draw_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DrawIndirectMulti: {
                const auto* draw_indirect =
                    reinterpret_cast<const PM4CmdDrawIndirectMulti*>(header);
                const auto offset = draw_indirect->data_offset;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndirectMulti", fmt::make_format_args(cmd_address), [&] {
                        SubmitDrawIndirect(false, indirect_args_addr, offset,
                                           draw_indirect->stride, draw_indirect->count, 0,
                                           draw_indirect->base_vtx_loc,
                                           draw_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DrawIndexIndirect: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirect*>(header);
                const auto offset = draw_index_indirect->data_offset;
                const auto stride = sizeof(DrawIndexedIndirectArgs);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndexIndirect", fmt::make_format_args(cmd_address), [&] {
                        SubmitDrawIndirect(true, indirect_args_addr, offset, stride, 1, 0,
                                           draw_index_indirect->base_vtx_loc,
                                           draw_index_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DrawIndexIndirectMulti: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirectMulti*>(header);
                const auto offset = draw_index_indirect->data_offset;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndexIndirectMulti", fmt::make_format_args(cmd_address), [&] {
                        SubmitDrawIndirect(
                            true, indirect_args_addr, offset, draw_index_indirect->stride,
                            draw_index_indirect->count, 0, draw_index_indirect->base_vtx_loc,
                            draw_index_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DrawIndexIndirectCountMulti: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirectCountMulti*>(header);
                const auto offset = draw_index_indirect->data_offset;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndexIndirectCountMulti", fmt::make_format_args(cmd_address), [&] {
                        SubmitDrawIndirect(
                            true, indirect_args_addr, offset, draw_index_indirect->stride,
                            draw_index_indirect->count,
                            draw_index_indirect->count_indirect_enable.Value()
                                ? draw_index_indirect->count_addr
                                : 0,
                            draw_index_indirect->base_vtx_loc, draw_index_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DispatchDirect: {
                const auto* dispatch_direct = reinterpret_cast<const PM4CmdDispatchDirect*>(header);
                auto& cs_program = GetCsRegs();
                cs_program.dim_x = dispatch_direct->dim_x;
                cs_program.dim_y = dispatch_direct->dim_y;
                cs_program.dim_z = dispatch_direct->dim_z;
                cs_program.dispatch_initiator = dispatch_direct->dispatch_initiator;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                                   cs_program);
                }
                if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker("gfx:{}:DispatchDirect", fmt::make_format_args(cmd_address),
                                        [&] { SubmitDispatch(false, 0, 0, 0); });
                break;
            }
            case PM4ItOpcode::DispatchIndirect: {
                const auto* dispatch_indirect =
                    reinterpret_cast<const PM4CmdDispatchIndirect*>(header);
                auto& cs_program = GetCsRegs();
                const auto offset = dispatch_indirect->data_offset;
                const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                                   cs_program);
                }
                if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DispatchIndirect", fmt::make_format_args(cmd_address),
                    [&] { SubmitDispatch(true, indirect_args_addr, offset, size); });
                break;
            }
            case PM4ItOpcode::NumInstances: {
                const auto* num_instances = reinterpret_cast<const PM4CmdDrawNumInstances*>(header);
                regs.num_instances.num_instances = num_instances->num_instances;
                MarkRegs(&regs.num_instances, sizeof(regs.num_instances));
                break;
            }
            case PM4ItOpcode::IndexBase: {
                const auto* index_base = reinterpret_cast<const PM4CmdDrawIndexBase*>(header);
                regs.index_base_address.base_addr_lo = index_base->addr_lo;
                regs.index_base_address.base_addr_hi = index_base->addr_hi;
                MarkRegs(&regs.index_base_address, sizeof(regs.index_base_address));
                break;
            }
            case PM4ItOpcode::IndexBufferSize: {
                const auto* index_size = reinterpret_cast<const PM4CmdDrawIndexBufferSize*>(header);
                regs.num_indices = index_size->num_indices;
                MarkRegs(&regs.num_indices, sizeof(regs.num_indices));
                break;
            }
            case PM4ItOpcode::SetBase: {
                const auto* set_base = reinterpret_cast<const PM4CmdSetBase*>(header);
                ASSERT(set_base->base_index == PM4CmdSetBase::BaseIndex::DrawIndexIndirPatchTable);
                indirect_args_addr = set_base->Address<u64>();
                break;
            }
            case PM4ItOpcode::EventWrite: {
                const auto* event = reinterpret_cast<const PM4CmdEventWrite*>(header);
                LOG_DEBUG(Render, "Encountered EventWrite: event_type = {}, event_index = {}",
                          magic_enum::enum_name(event->event_type.Value()),
                          magic_enum::enum_name(event->event_index.Value()));
                if (event->event_type.Value() == EventType::SoVgtStreamoutFlush) {
                    // TODO: handle proper synchronization, for now signal that update is done
                    // immediately
                    regs.cp_strmout_cntl.offset_update_done = 1;
                    MarkRegs(&regs.cp_strmout_cntl, sizeof(regs.cp_strmout_cntl));
                } else if (event->event_index.Value() == EventIndex::ZpassDone) {
                    if (event->event_type.Value() == EventType::PixelPipeStatDump) {
                        static constexpr u64 OcclusionCounterValidMask = 0x8000000000000000ULL;
                        // SOTC_OCCLUSION_STEP overrides the fake per-dump increment (A/B tests:
                        // 0 = nothing passes the depth test, i.e. every query reads "occluded").
                        static const u64 OcclusionCounterStep = [] {
                            const char* v = std::getenv("SOTC_OCCLUSION_STEP");
                            return v ? std::strtoull(v, nullptr, 0) : 0x2FFFFFFULL;
                        }();
                        u64* results = event->Address<u64*>();
                        g_sotc_zpass_dumps.fetch_add(1, std::memory_order_relaxed);
                        for (s32 i = 0; i < num_counter_pairs; ++i, results += 2) {
                            *results = pixel_counter | OcclusionCounterValidMask;
                        }
                        pixel_counter += OcclusionCounterStep;
                    }
                }
                break;
            }
            case PM4ItOpcode::EventWriteEos: {
                const PM4CmdEventWriteEos eos = *reinterpret_cast<const PM4CmdEventWriteEos*>(header);
                BackOrRun([this, eos] {
                    if (rasterizer) {
                        rasterizer->OnFence();
                    }
                    eos.SignalFence([this](void* address, u64 data, u32 num_bytes) {
                        WriteFenceValue(address, data, num_bytes);
                    });
                    if (eos.command == PM4CmdEventWriteEos::Command::GdsStore) {
                        ASSERT(eos.size == 1);
                        if (rasterizer) {
                            rasterizer->Finish();
                            const u32 value = rasterizer->ReadDataFromGds(eos.gds_index);
                            *eos.Address() = value;
                        }
                    }
                }, back_strict);
                break;
            }
            case PM4ItOpcode::EventWriteEop: {
                const PM4CmdEventWriteEop eop_packet =
                    *reinterpret_cast<const PM4CmdEventWriteEop*>(header);
                BackOrRun([this, eop_packet] {
                const auto* event_eop = &eop_packet;
                if (rasterizer) {
                    rasterizer->OnFence();
                }
                // sotc: an end-of-pipe label means "the GPU finished everything before it". It
                // was written as soon as the command processor read it, while the work was still
                // queued for the host GPU: the game then reused or freed memory the GPU was still
                // reading or about to write (heap and descriptor corruption once the texture
                // streaming moves memory around). The label and its interrupt now wait for the
                // host GPU to finish the work recorded so far (the scheduler's priority thread).
                // SOTC_EOP_DEFER=1 only: it halves the frame rate (the game waits for the GPU at
                // every label) and did not stop the streaming corruption (2026-10-12 tests).
                static const bool defer_eop = [] {
                    const char* v = std::getenv("SOTC_EOP_DEFER");
                    return v && v[0] == '1';
                }();
                if (defer_eop && rasterizer) {
                    const PM4CmdEventWriteEop eop = *event_eop;
                    const VAddr label = reinterpret_cast<VAddr>(eop.Address<void>());
                    // A GPU copy of the label's page must not be reused with the old value.
                    if (eop.data_sel.Value() != DataSelect::None) {
                        rasterizer->InvalidateMemory(label, 8, true);
                    }
                    rasterizer->GetScheduler().DeferPriorityOperation([eop] {
                        eop.SignalFence(
                            [](void* address, u64 data, u32 num_bytes) {
                                ASSERT(Core::Memory::Instance()->TryWriteBacking(address, &data,
                                                                                 num_bytes, 2));
                            },
                            [] {
                                Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxEop);
                            });
                    });
                    // Submit that work now: the game may wait for the label before it sends
                    // anything else, and nothing else would submit it.
                    rasterizer->Flush();
                    return;
                }
                event_eop->SignalFence(
                    [this](void* address, u64 data, u32 num_bytes) {
                        WriteFenceValue(address, data, num_bytes);
                    },
                    [] { Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxEop); });
                }, back_strict);
                break;
            }
            case PM4ItOpcode::DmaData: {
                const auto* dma_data = reinterpret_cast<const PM4DmaData*>(header);
                if (dma_data->dst_addr_lo == 0x3022C || !rasterizer) {
                    break;
                }
                ASSERT(dma_data->command.das == 0);
                const PM4DmaData dma = *dma_data;
                if (dma.src_sel == DmaDataSrc::Data && dma.dst_sel == DmaDataDst::Gds) {
                    BackOrRun([this, dma] {
                        rasterizer->FillBuffer(dma.dst_addr_lo, dma.NumBytes(), dma.data, true);
                    });
                } else if ((dma.src_sel == DmaDataSrc::Memory ||
                            dma.src_sel == DmaDataSrc::MemoryUsingL2) &&
                           dma.dst_sel == DmaDataDst::Gds) {
                    BackOrRun([this, dma] {
                        rasterizer->CopyBuffer(dma.dst_addr_lo, dma.SrcAddress<VAddr>(),
                                               dma.NumBytes(), true, false);
                    });
                } else if (dma.src_sel == DmaDataSrc::Data &&
                           (dma.dst_sel == DmaDataDst::Memory ||
                            dma.dst_sel == DmaDataDst::MemoryUsingL2)) {
                    BackOrRun([this, dma] {
                        rasterizer->FillBuffer(dma.DstAddress<VAddr>(), dma.NumBytes(), dma.data,
                                               false);
                    });
                } else if (dma.src_sel == DmaDataSrc::Gds &&
                           (dma.dst_sel == DmaDataDst::Memory ||
                            dma.dst_sel == DmaDataDst::MemoryUsingL2)) {
                    BackOrRun([this, dma] {
                        rasterizer->CopyBuffer(dma.DstAddress<VAddr>(), dma.src_addr_lo,
                                               dma.NumBytes(), false, true);
                    });
                } else if ((dma.src_sel == DmaDataSrc::Memory ||
                            dma.src_sel == DmaDataSrc::MemoryUsingL2) &&
                           (dma.dst_sel == DmaDataDst::Memory ||
                            dma.dst_sel == DmaDataDst::MemoryUsingL2)) {
                    BackOrRun([this, dma] {
                        rasterizer->CopyBuffer(dma.DstAddress<VAddr>(), dma.SrcAddress<VAddr>(),
                                               dma.NumBytes(), false, false);
                    });
                } else {
                    UNREACHABLE_MSG("WriteData src_sel = {}, dst_sel = {}", u32(dma_data->src_sel),
                                    u32(dma_data->dst_sel));
                }
                break;
            }
            case PM4ItOpcode::WriteData: {
                const auto* write_data = reinterpret_cast<const PM4CmdWriteData*>(header);
                ASSERT(write_data->dst_sel.Value() == 2 || write_data->dst_sel.Value() == 5);
                const u32 data_size = (header->type3.count.Value() - 2) * 4;
                u64* address = write_data->Address<u64*>();
                if (!write_data->wr_one_addr.Value()) {
                    std::vector<u32> data(write_data->data, write_data->data + data_size / 4);
                    BackOrRun([this, address, data = std::move(data), data_size] {
                        if (rasterizer) {
                            rasterizer->OnFence();
                        }
                        std::memcpy(address, data.data(), data_size);
                        Core::MemoryManager::NoteHostWrite(std::bit_cast<VAddr>(address),
                                                           data_size, 4);
                    });
                } else {
                    UNREACHABLE();
                }
                break;
            }
            case PM4ItOpcode::CopyData: {
                const auto* copy_data = reinterpret_cast<const PM4CmdCopyData*>(header);
                LOG_WARNING(Render,
                            "unhandled IT_COPY_DATA src_sel = {}, dst_sel = {}, "
                            "count_sel = {}, wr_confirm = {}, engine_sel = {}",
                            u32(copy_data->src_sel.Value()), u32(copy_data->dst_sel.Value()),
                            copy_data->count_sel.Value(), copy_data->wr_confirm.Value(),
                            u32(copy_data->engine_sel.Value()));
                break;
            }
            case PM4ItOpcode::MemSemaphore: {
                const auto* mem_semaphore = reinterpret_cast<const PM4CmdMemSemaphore*>(header);
                if (mem_semaphore->IsSignaling()) {
                    mem_semaphore->Signal();
                } else {
                    while (!mem_semaphore->Signaled()) {
                        YIELD_GFX();
                    }
                    mem_semaphore->Decrement();
                }
                break;
            }
            case PM4ItOpcode::AcquireMem: {
                // const auto* acquire_mem = reinterpret_cast<PM4CmdAcquireMem*>(header);
                break;
            }
            case PM4ItOpcode::Rewind: {
                if (!rasterizer) {
                    break;
                }
                const PM4CmdRewind* rewind = reinterpret_cast<const PM4CmdRewind*>(header);
                while (!rewind->Valid()) {
                    YIELD_GFX();
                }
                break;
            }
            case PM4ItOpcode::WaitRegMem: {
                const auto* wait_reg_mem = reinterpret_cast<const PM4CmdWaitRegMem*>(header);
                // ASSERT(wait_reg_mem->engine.Value() == PM4CmdWaitRegMem::Engine::Me);
                // Optimization: VO label waits are special because the emulator
                // will write to the label when presentation is finished. So if
                // there are no other submits to yield to we can sleep the thread
                // instead and allow other tasks to run.
                const u64* wait_addr = wait_reg_mem->Address<u64*>();
                if (vo_port->IsVoLabel(wait_addr) &&
                    num_submits == mapped_queues[GfxQueueId].submits.size()) {
                    vo_port->WaitVoLabel([&] { return wait_reg_mem->Test(regs.reg_array); });
                    break;
                }
                while (!wait_reg_mem->Test(regs.reg_array)) {
                    YIELD_GFX();
                }
                break;
            }
            case PM4ItOpcode::IndirectBuffer: {
                const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
                auto task = ProcessGraphics(
                    {indirect_buffer->Address<const u32>(), indirect_buffer->ib_size}, {});
                RESUME_GFX(task);

                while (!task.handle.done()) {
                    YIELD_GFX();
                    RESUME_GFX(task);
                }
                break;
            }
            case PM4ItOpcode::IncrementDeCounter: {
                ++cblock.de_count;
                break;
            }
            case PM4ItOpcode::WaitOnCeCounter: {
                while (cblock.ce_count <= cblock.de_count && !ce_task.handle.done()) {
                    RESUME_GFX(ce_task);
                }
                break;
            }
            case PM4ItOpcode::PfpSyncMe: {
                break;
            }
            case PM4ItOpcode::StrmoutBufferUpdate: {
                const auto* strmout = reinterpret_cast<const PM4CmdStrmoutBufferUpdate*>(header);
                LOG_WARNING(Render_Vulkan,
                            "Unimplemented IT_STRMOUT_BUFFER_UPDATE, update_memory = {}, "
                            "source_select = {}, buffer_select = {}",
                            strmout->update_memory.Value(),
                            magic_enum::enum_name(strmout->source_select.Value()),
                            strmout->buffer_select.Value());
                break;
            }
            case PM4ItOpcode::GetLodStats: {
                // sotc: the texture LOD statistics Shadow of the Colossus streams its fine mips
                // from (sotc_lod_stats.h). Packet (from the game's Gnm getLodStats, 0x86a880):
                // size - 32, address >> 6, address bits 38-39 | 0xbfc00. The game reads 256 u64
                // bank entries at +0x40 and again at +0x840 (it alternates between the two).
                // On again (2026-10-13): the guest memory corruption with streaming came from the
                // GPU readback on unmap (Rasterizer::UnmapMemory), not from these statistics.
                // SOTC_LOD_STATS=0 leaves the buffer untouched (SOTC_LOD_LEVEL picks the mip).
                static const bool lod_stats = [] {
                    const char* v = std::getenv("SOTC_LOD_STATS");
                    return !v || v[0] != '0';
                }();
                const u32* d = reinterpret_cast<const u32*>(header) + 1;
                if (lod_stats && count >= 3) {
                    const VAddr base = (VAddr{d[1]} << 6) | (VAddr{d[2] & 3u} << 38);
                    const u32 size = d[0] + 0x20;
                    BackOrRun([base, size] {
                    u64 entries[256];
                    SotcLod::Collect(entries);
                    for (u32 half = 0; 0x40 + (half + 1) * sizeof(entries) <= size; ++half) {
                        runtime_memory_write_backing(base + 0x40 + half * sizeof(entries), entries,
                                                     sizeof(entries));
                    }
                    static const bool lod_trace = std::getenv("SOTC_LOD_TRACE") != nullptr;
                    static u64 packets = 0;
                    if (lod_trace && packets++ % 600 == 0) {
                        u32 used = 0;
                        for (const u64 e : entries) {
                            used += (e >> 56) != 0xf;
                        }
                        LOG_WARNING(Render_Vulkan, "sotc GET_LOD_STATS {:#x}+{:#x}: {} banks sampled",
                                    base, size, used);
                    }
                    }, false);
                }
                break;
            }
            case PM4ItOpcode::CondExec: {
                const auto* cond_exec = reinterpret_cast<const PM4CmdCondExec*>(header);
                if (cond_exec->command.Value() != 0) {
                    LOG_WARNING(Render, "IT_COND_EXEC used a reserved command");
                }
                WaitBackWrites();
                const auto skip = *cond_exec->Address() == false;
                if (skip) {
                    SotcStats::Drop(4, reinterpret_cast<uintptr_t>(cond_exec->Address()),
                                    cond_exec->exec_count.Value());
                    dcb = NextPacket(dcb,
                                     header->type3.NumWords() + 1 + cond_exec->exec_count.Value());
                    continue;
                }
                break;
            }
            default:
                UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                                static_cast<u32>(opcode), count);
            }
            dcb = NextPacket(dcb, header->type3.NumWords() + 1);
            break;
        }
    }

    if (ce_task.handle) {
        while (!ce_task.handle.done()) {
            RESUME_GFX(ce_task);
        }
        ce_task.handle.destroy();
    }

    FIBER_EXIT;
}

template <bool is_indirect>
Liverpool::Task Liverpool::ProcessCompute(std::span<const u32> acb, u32 vqid) {
    FIBER_ENTER(acb_task_name[vqid]);
    auto& queue = asc_queues[{vqid}];

    struct IndirectPatch {
        const PM4Header* header;
        VAddr indirect_addr;
    };
    boost::container::small_vector<IndirectPatch, 4> indirect_patches;

    auto base_addr = reinterpret_cast<VAddr>(acb.data());
    size_t acb_size = acb.size_bytes();
    while (!acb.empty()) {
        ProcessCommands();

        auto* header = reinterpret_cast<const PM4Header*>(acb.data());
        // Type-2 padding is a single dword; its count bits are meaningless. Sizing it as type-3
        // makes padding at the end of a submission look split, which corrupts the next packet.
        u32 next_dw_off = header->type == 2 ? 1 : header->type3.NumWords() + 1;

        // If we have a buffered packet, use it.
        if (queue.tmp_dwords > 0) [[unlikely]] {
            header = reinterpret_cast<const PM4Header*>(queue.tmp_packet.data());
            next_dw_off = header->type3.NumWords() + 1 - queue.tmp_dwords;
            std::memcpy(queue.tmp_packet.data() + queue.tmp_dwords, acb.data(),
                        next_dw_off * sizeof(u32));
            queue.tmp_dwords = 0;
        }

        // If the packet is split across ring boundary, buffer until next submission
        if (next_dw_off > acb.size()) [[unlikely]] {
            std::memcpy(queue.tmp_packet.data(), acb.data(), acb.size_bytes());
            queue.tmp_dwords = acb.size();
            if constexpr (!is_indirect) {
                *queue.read_addr += acb.size();
                *queue.read_addr %= queue.ring_size_dw;
            }
            break;
        }

        if (header->type == 2) {
            // Type-2 packet are used for padding purposes
            next_dw_off = 1;
            acb = NextPacket(acb, next_dw_off);
            if constexpr (!is_indirect) {
                *queue.read_addr += next_dw_off;
                *queue.read_addr %= queue.ring_size_dw;
            }
            continue;
        }

        if (header->type != 3) {
            // No other types of packets were spotted so far
            UNREACHABLE_MSG("Invalid PM4 type {}", header->type.Value());
        }

        const PM4ItOpcode opcode = header->type3.opcode;

        const auto* it_body = reinterpret_cast<const u32*>(header) + 1;
        switch (opcode) {
        case PM4ItOpcode::Nop: {
            const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
            break;
        }
        case PM4ItOpcode::IndirectBuffer: {
            const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
            auto task = ProcessCompute<true>(
                {indirect_buffer->Address<const u32>(), indirect_buffer->ib_size}, vqid);
            RESUME_ASC(task, vqid);

            while (!task.handle.done()) {
                YIELD_ASC(vqid);
                RESUME_ASC(task, vqid);
            }
            break;
        }
        case PM4ItOpcode::DmaData: {
            const auto* dma_data = reinterpret_cast<const PM4DmaData*>(header);
            if (dma_data->dst_addr_lo == 0x3022C || !rasterizer) {
                break;
            }
            ASSERT(dma_data->command.das == 0);
            const PM4DmaData dma = *dma_data;
            if (dma.src_sel == DmaDataSrc::Data && dma.dst_sel == DmaDataDst::Gds) {
                BackOrRun([this, dma] {
                    rasterizer->FillBuffer(dma.dst_addr_lo, dma.NumBytes(), dma.data, true);
                });
            } else if ((dma.src_sel == DmaDataSrc::Memory ||
                        dma.src_sel == DmaDataSrc::MemoryUsingL2) &&
                       dma.dst_sel == DmaDataDst::Gds) {
                BackOrRun([this, dma] {
                    rasterizer->CopyBuffer(dma.dst_addr_lo, dma.SrcAddress<VAddr>(),
                                           dma.NumBytes(), true, false);
                });
            } else if (dma.src_sel == DmaDataSrc::Data &&
                       (dma.dst_sel == DmaDataDst::Memory ||
                        dma.dst_sel == DmaDataDst::MemoryUsingL2)) {
                BackOrRun([this, dma] {
                    rasterizer->FillBuffer(dma.DstAddress<VAddr>(), dma.NumBytes(), dma.data,
                                           false);
                });
            } else if (dma.src_sel == DmaDataSrc::Gds &&
                       (dma.dst_sel == DmaDataDst::Memory ||
                        dma.dst_sel == DmaDataDst::MemoryUsingL2)) {
                BackOrRun([this, dma] {
                    rasterizer->CopyBuffer(dma.DstAddress<VAddr>(), dma.src_addr_lo,
                                           dma.NumBytes(), false, true);
                });
            } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                        dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
                       (dma_data->dst_sel == DmaDataDst::Memory ||
                        dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                const u32 num_bytes = dma_data->NumBytes();
                const VAddr src_addr = dma_data->SrcAddress<VAddr>();
                const VAddr dst_addr = dma_data->DstAddress<VAddr>();
                const PM4Header* header =
                    reinterpret_cast<const PM4Header*>(dst_addr - sizeof(PM4Header));
                if (dst_addr >= base_addr && dst_addr < base_addr + acb_size &&
                    num_bytes == sizeof(PM4CmdDispatchIndirect::GroupDimensions) &&
                    header->type == 3 && header->type3.opcode == PM4ItOpcode::DispatchDirect) {
                    indirect_patches.emplace_back(header, src_addr);
                } else {
                    BackOrRun([this, dst_addr, src_addr, num_bytes] {
                        rasterizer->CopyBuffer(dst_addr, src_addr, num_bytes, false, false);
                    });
                }
            } else {
                UNREACHABLE_MSG("WriteData src_sel = {}, dst_sel = {}", u32(dma_data->src_sel),
                                u32(dma_data->dst_sel));
            }
            break;
        }
        case PM4ItOpcode::AcquireMem: {
            break;
        }
        case PM4ItOpcode::Rewind: {
            if (!rasterizer) {
                break;
            }
            const PM4CmdRewind* rewind = reinterpret_cast<const PM4CmdRewind*>(header);
            while (!rewind->Valid()) {
                YIELD_ASC(vqid);
            }
            break;
        }
        case PM4ItOpcode::SetShReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
            const auto set_size = (header->type3.NumWords() - 1) * sizeof(u32);

            if (set_data->reg_offset >= 0x200 &&
                set_data->reg_offset <= (0x200 + sizeof(ComputeProgram) / 4)) {
                ASSERT(set_size <= sizeof(ComputeProgram));
                auto* addr = reinterpret_cast<u32*>(&mapped_queues[vqid + 1].cs_state) +
                             (set_data->reg_offset - 0x200);
                std::memcpy(addr, header + 2, set_size);
            } else {
                std::memcpy(&regs.reg_array[Regs::ShRegWordOffset + set_data->reg_offset],
                            header + 2, set_size);
                MarkRegs(Regs::ShRegWordOffset + set_data->reg_offset,
                         static_cast<u32>(set_size / 4));
            }
            break;
        }
        case PM4ItOpcode::SetQueueReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetQueueReg*>(header);
            LOG_WARNING(Render, "Encountered compute SetQueueReg: vqid = {}, reg_offset = {:#x}",
                        set_data->vqid.Value(), set_data->reg_offset.Value());
            break;
        }
        case PM4ItOpcode::DispatchDirect: {
            const auto* dispatch_direct = reinterpret_cast<const PM4CmdDispatchDirect*>(header);
            if (auto it = std::ranges::find(indirect_patches, header, &IndirectPatch::header);
                it != indirect_patches.end()) {
                const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
                SubmitDispatch(true, it->indirect_addr, 0, size);
                break;
            }
            auto& cs_program = GetCsRegs();
            cs_program.dim_x = dispatch_direct->dim_x;
            cs_program.dim_y = dispatch_direct->dim_y;
            cs_program.dim_z = dispatch_direct->dim_z;
            cs_program.dispatch_initiator = dispatch_direct->dispatch_initiator;
            if (DebugState.DumpingCurrentReg()) {
                DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                               cs_program);
            }
            if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                break;
            }
            const auto cmd_address = reinterpret_cast<const void*>(header);
            rasterizer->ScopeMarker("asc[{}]:{}:DispatchDirect",
                                    fmt::make_format_args(vqid, cmd_address),
                                    [&] { SubmitDispatch(false, 0, 0, 0); });
            break;
        }
        case PM4ItOpcode::DispatchIndirect: {
            const auto* dispatch_indirect =
                reinterpret_cast<const PM4CmdDispatchIndirectMec*>(header);
            auto& cs_program = GetCsRegs();
            const auto ib_address = dispatch_indirect->Address<VAddr>();
            const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
            if (DebugState.DumpingCurrentReg()) {
                DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                               cs_program);
            }
            if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                break;
            }
            const auto cmd_address = reinterpret_cast<const void*>(header);
            rasterizer->ScopeMarker("asc[{}]:{}:DispatchIndirect",
                                    fmt::make_format_args(vqid, cmd_address),
                                    [&] { SubmitDispatch(true, ib_address, 0, size); });
            break;
        }
        case PM4ItOpcode::WriteData: {
            const auto* write_data = reinterpret_cast<const PM4CmdWriteData*>(header);
            ASSERT(write_data->dst_sel.Value() == 2 || write_data->dst_sel.Value() == 5);
            const u32 data_size = (header->type3.count.Value() - 2) * 4;
            if (!write_data->wr_one_addr.Value()) {
                void* address = write_data->Address<void*>();
                std::vector<u32> data(write_data->data, write_data->data + data_size / 4);
                BackOrRun([this, address, data = std::move(data), data_size] {
                    if (rasterizer) {
                        rasterizer->OnFence();
                    }
                    std::memcpy(address, data.data(), data_size);
                    Core::MemoryManager::NoteHostWrite(reinterpret_cast<VAddr>(address), data_size,
                                                       4);
                });
            } else {
                UNREACHABLE();
            }
            break;
        }
        case PM4ItOpcode::MemSemaphore: {
            const auto* mem_semaphore = reinterpret_cast<const PM4CmdMemSemaphore*>(header);
            if (mem_semaphore->IsSignaling()) {
                mem_semaphore->Signal();
            } else {
                while (!mem_semaphore->Signaled()) {
                    YIELD_ASC(vqid);
                }
                mem_semaphore->Decrement();
            }
            break;
        }
        case PM4ItOpcode::WaitRegMem: {
            const auto* wait_reg_mem = reinterpret_cast<const PM4CmdWaitRegMem*>(header);
            ASSERT(wait_reg_mem->engine.Value() == PM4CmdWaitRegMem::Engine::Me);
            while (!wait_reg_mem->Test(regs.reg_array)) {
                YIELD_ASC(vqid);
            }
            break;
        }
        case PM4ItOpcode::ReleaseMem: {
            const PM4CmdReleaseMem release = *reinterpret_cast<const PM4CmdReleaseMem*>(header);
            BackOrRun([this, release, pipe_id = queue.pipe_id] {
                if (rasterizer) {
                    rasterizer->OnFence();
                }
                release.SignalFence(
                    [pipe_id] {
                        Platform::IrqC::Instance()->Signal(
                            static_cast<Platform::InterruptId>(pipe_id));
                    },
                    [this](VAddr dst, u16 gds_index, u16 num_dwords) {
                        rasterizer->CopyBuffer(dst, gds_index, num_dwords * sizeof(u32), false,
                                               true);
                    });
            }, back_strict);
            break;
        }
        case PM4ItOpcode::EventWrite: {
            // const auto* event = reinterpret_cast<const PM4CmdEventWrite*>(header);
            break;
        }
        default:
            UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                            static_cast<u32>(opcode), header->type3.NumWords());
        }

        acb = NextPacket(acb, next_dw_off);

        if constexpr (!is_indirect) {
            *queue.read_addr += next_dw_off;
            *queue.read_addr %= queue.ring_size_dw;
        }
    }

    FIBER_EXIT;
}

Liverpool::CmdBuffer Liverpool::CopyCmdBuffers(std::span<const u32> dcb, std::span<const u32> ccb) {
    auto& queue = mapped_queues[GfxQueueId];
    ASSERT_MSG(queue.dcb_buffer.capacity() >= queue.dcb_buffer_offset + dcb.size(),
               "dcb copy buffer out of reserved space");
    ASSERT_MSG(queue.ccb_buffer.capacity() >= queue.ccb_buffer_offset + ccb.size(),
               "ccb copy buffer out of reserved space");

    queue.dcb_buffer.resize(
        std::max(queue.dcb_buffer.size(), queue.dcb_buffer_offset + dcb.size()));
    queue.ccb_buffer.resize(
        std::max(queue.ccb_buffer.size(), queue.ccb_buffer_offset + ccb.size()));

    const u32 prev_dcb_buffer_offset = queue.dcb_buffer_offset;
    const u32 prev_ccb_buffer_offset = queue.ccb_buffer_offset;
    if (!dcb.empty()) {
        std::memcpy(queue.dcb_buffer.data() + queue.dcb_buffer_offset, dcb.data(),
                    dcb.size_bytes());
        queue.dcb_buffer_offset += dcb.size();
        dcb = std::span<const u32>{queue.dcb_buffer.begin() + prev_dcb_buffer_offset,
                                   queue.dcb_buffer.begin() + queue.dcb_buffer_offset};
    }

    if (!ccb.empty()) {
        std::memcpy(queue.ccb_buffer.data() + queue.ccb_buffer_offset, ccb.data(),
                    ccb.size_bytes());
        queue.ccb_buffer_offset += ccb.size();
        ccb = std::span<const u32>{queue.ccb_buffer.begin() + prev_ccb_buffer_offset,
                                   queue.ccb_buffer.begin() + queue.ccb_buffer_offset};
    }

    return std::make_pair(dcb, ccb);
}

void Liverpool::SubmitGfx(std::span<const u32> dcb, std::span<const u32> ccb) {
    auto& queue = mapped_queues[GfxQueueId];

    if (EmulatorSettings.IsCopyGpuBuffers()) {
        std::tie(dcb, ccb) = CopyCmdBuffers(dcb, ccb);
    }

    auto task = ProcessGraphics(dcb, ccb);
    {
        std::scoped_lock lock{queue.m_access};
        queue.submits.emplace(task.handle);
    }

    std::scoped_lock lk{submit_mutex};
    ++num_submits;
    submit_cv.notify_one();
}

void Liverpool::SubmitAsc(u32 gnm_vqid, std::span<const u32> acb) {
    ASSERT_MSG(gnm_vqid > 0 && gnm_vqid < NumTotalQueues, "Invalid virtual ASC queue index");
    auto& queue = mapped_queues[gnm_vqid];

    const auto vqid = gnm_vqid - 1;
    const auto& task = ProcessCompute(acb, vqid);
    {
        std::scoped_lock lock{queue.m_access};
        queue.submits.emplace(task.handle);
    }

    std::scoped_lock lk{submit_mutex};
    num_mapped_queues = std::max(num_mapped_queues, gnm_vqid + 1);
    ++num_submits;
    submit_cv.notify_one();
}

} // namespace AmdGpu
