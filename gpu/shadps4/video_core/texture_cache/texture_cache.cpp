// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <xxhash.h>

#include "common/assert.h"
#include "common/debug.h"
#include "common/div_ceil.h"
#include "common/hash.h"
#include "common/scope_exit.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/sotc_stats.h"
#include "video_core/texture_cache/host_compatibility.h"

#include <vk_mem_alloc.h>
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/texture_cache/tile_manager.h"

namespace VideoCore {

static constexpr u64 PageShift = 12;
static constexpr u64 NumFramesBeforeRemoval = 32;

/// Writes downloaded image data back to guest memory, leaving out guest stack pages (never
/// protected, the CPU may hold newer data there).
static void WriteBackSkippingStacks(VAddr guest_address, const u8* data, u64 size) {
    ForEachNonStackRange(guest_address, size, [&](VAddr addr, u64 part) {
        Core::Memory::Instance()->TryWriteBacking(std::bit_cast<u8*>(addr),
                                                  data + (addr - guest_address), part, 3);
    });
}

TextureCache::TextureCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                           Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                           BufferCache& buffer_cache_, PageManager& tracker_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_}, liverpool{liverpool_},
      buffer_cache{buffer_cache_}, tracker{tracker_}, blit_helper{instance, scheduler},
      tile_manager{instance, scheduler, runtime, buffer_cache.GetStreamBuffer()},
      readback_linear_images{EmulatorSettings.IsReadbackLinearImagesEnabled()} {

    u32 max_samplers = instance.GetMaxSamplerAllocationCount();
    trigger_gc_samplers = max_samplers * 3 / 4;
    pressure_gc_samplers = max_samplers * 7 / 8;
    critical_gc_samplers = max_samplers * 15 / 16;

    // Set up garbage collection parameters.
    if (!instance.CanReportMemoryUsage()) {
        trigger_gc_memory = 0;
        pressure_gc_memory = DEFAULT_PRESSURE_GC_MEMORY;
        critical_gc_memory = DEFAULT_CRITICAL_GC_MEMORY;
        return;
    }

    const s64 device_local_memory = static_cast<s64>(instance.GetTotalMemoryBudget());
    const s64 min_spacing_expected = device_local_memory - 1_GB;
    const s64 min_spacing_critical = device_local_memory - 512_MB;
    const s64 mem_threshold = std::min<s64>(device_local_memory, TARGET_GC_THRESHOLD);
    const s64 min_vacancy_expected = (6 * mem_threshold) / 10;
    const s64 min_vacancy_critical = (2 * mem_threshold) / 10;
    const s64 min_pressure_floor =
        std::clamp<s64>(device_local_memory / 4, 256_MB, DEFAULT_PRESSURE_GC_MEMORY);
    const s64 min_critical_floor =
        std::clamp<s64>(device_local_memory / 2, 512_MB, DEFAULT_CRITICAL_GC_MEMORY);
    pressure_gc_memory = static_cast<u64>(
        std::max<s64>(std::min(device_local_memory - min_vacancy_expected, min_spacing_expected),
                      min_pressure_floor));
    critical_gc_memory = static_cast<u64>(
        std::max<s64>(std::min(device_local_memory - min_vacancy_critical, min_spacing_critical),
                      min_critical_floor));
    trigger_gc_memory = static_cast<u64>((device_local_memory - mem_threshold) / 2);
    // Buffer arena residency is never released and competes for the same budget, so start
    // aggressive collection well before the budget is reached (80% left too little room on
    // 8 GB cards and new allocations failed).
    critical_gc_memory = std::min<u64>(critical_gc_memory, device_local_memory * 6 / 10);
    pressure_gc_memory = std::min<u64>(pressure_gc_memory, critical_gc_memory);
}

TextureCache::~TextureCache() = default;

void TextureCache::ProcessDownloadImages() {
    // sotc: download_images is only ever filled (FindTexture/FindRenderTarget) under
    // readback_linear_images, a const member, so without it the set is always empty: skip the
    // mutex the GPU thread took twice per draw/dispatch for nothing.
    if (!readback_linear_images) {
        return;
    }
    std::unique_lock lk{download_images_mutex};
    for (const ImageId image_id : download_images) {
        DownloadImageMemory(image_id, true);
    }
    download_images.clear();
}

void TextureCache::DownloadImageMemory(ImageId image_id, bool sync) {
    Image& image = slot_images[image_id];
    if (False(image.flags & ImageFlagBits::GpuModified)) {
        return;
    }
    const u32 download_size = image.info.pitch * image.info.size.height * image.info.size.depth *
                              image.info.resources.layers * (image.info.num_bits / 8);
    ASSERT(download_size <= image.info.guest_size);
    const auto download =
        runtime.GetStagingPool().Request(download_size, MemoryType::HostCached, 16, !sync);
    const vk::BufferImageCopy image_download = {
        .bufferOffset = download.offset,
        .bufferRowLength = image.info.pitch,
        .bufferImageHeight = image.info.size.height,
        .imageSubresource =
            {
                .aspectMask = image.info.props.is_depth ? vk::ImageAspectFlagBits::eDepth
                                                        : vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {image.info.size.width, image.info.size.height, image.info.size.depth},
    };
    runtime.DownloadImage(&image, download.buffer, std::span{&image_download, 1});
    if (sync) {
        scheduler.Finish();
        download.Invalidate();
        WriteBackSkippingStacks(image.info.guest_address, download.mapped, download_size);
    } else {
        scheduler.DeferPriorityOperation(
            [this, device_addr = image.info.guest_address, download, download_size] {
                download.Invalidate();
                WriteBackSkippingStacks(device_addr, download.mapped, download_size);
                runtime.GetStagingPool().FreeDeferred(download);
            });
    }
}

void TextureCache::DumpImage(ImageId image_id, const std::filesystem::path& path) {
    Image& image = slot_images[image_id];
    const bool is_depth = image.info.props.is_depth;
    const u32 texel = is_depth ? 4 : std::max(1u, image.info.num_bits / 8);
    const u32 width = image.info.size.width;
    const u32 height = image.info.size.height;
    const u32 depth = image.info.size.depth;
    const u32 layers = image.info.resources.layers;
    const u32 size = width * height * depth * layers * texel;
    if (size == 0 || size > 512_MB) {
        return;
    }
    const auto download = runtime.GetStagingPool().Request(size, MemoryType::HostCached, 16, true);
    const vk::BufferImageCopy copy = {
        .bufferOffset = download.offset,
        .bufferRowLength = width,
        .bufferImageHeight = height,
        .imageSubresource =
            {
                .aspectMask = is_depth ? vk::ImageAspectFlagBits::eDepth
                                       : vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = layers,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {width, height, depth},
    };
    runtime.DownloadImage(&image, download.buffer, std::span{&copy, 1});
    const auto name = fmt::format("{}_{:#x}_{}x{}x{}x{}_{}.raw", path.string(),
                                  image.info.guest_address, width, height, depth, layers,
                                  vk::to_string(image.info.pixel_format));
    scheduler.DeferPriorityOperation([this, download, size, name] {
        download.Invalidate();
        if (auto* f = std::fopen(name.c_str(), "wb")) {
            std::fwrite(download.mapped, 1, size, f);
            std::fclose(f);
        }
        runtime.GetStagingPool().FreeDeferred(download);
    });
}

void TextureCache::MarkAsMaybeDirty(ImageId image_id, Image& image) {
    // Do not hash guest memory here: this runs from the CPU fault handler with the cache mutex
    // held, and reading a GPU-modified (read-protected) page faults again, waits for the GPU
    // thread to flush it, while the GPU thread waits for this mutex. RefreshImage hashes the
    // first texels on the GPU thread anyway (an unset hash simply means "re-upload").
    image.flags |= ImageFlagBits::MaybeCpuDirty;
    UntrackImage(image_id);
}

void TextureCache::InvalidateMemory(VAddr addr, size_t size) {
    std::scoped_lock lock{mutex};
    const auto pages_start = PageManager::GetPageAddr(addr);
    const auto pages_end = PageManager::GetNextPageAddr(addr + size - 1);
    ForEachImageInRegion(pages_start, pages_end - pages_start, [&](ImageId image_id, Image& image) {
        const auto image_begin = image.info.guest_address;
        const auto image_end = image.info.guest_address + image.info.guest_size;
        if (image.Overlaps(addr, size)) {
            // Modified region overlaps image, so the image was definitely accessed by this fault.
            // Untrack the image, so that the range is unprotected and the guest can write freely.
            image.flags |= ImageFlagBits::CpuDirty;
            UntrackImage(image_id);
        } else if (pages_end < image_end) {
            // This page access may or may not modify the image.
            // We should not mark it as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            // Remove tracking from this page only.
            UntrackImageHead(image_id);
        } else if (image_begin < pages_start) {
            // This page access does not modify the image but the page should be untracked.
            // We should not mark this image as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            UntrackImageTail(image_id);
        } else {
            // Image begins and ends on this page so it can not receive any more invalidations.
            // We will check it's hash later to see if it really was modified.
            MarkAsMaybeDirty(image_id, image);
        }
    });
}

void TextureCache::InvalidateMemoryFromGPU(VAddr address, size_t max_size) {
    std::scoped_lock lock{mutex};
    // sotc: SOTC_IMG_GPU_OVERLAP=1 also reuploads images inside a range a shader wrote as a
    // buffer, not only an image at its exact base (test for the empty 960x540 A2B10G10R10
    // lighting input behind the black shadows; it did not change the image, 2026-10-12).
    static const bool overlap = [] {
        const char* v = std::getenv("SOTC_IMG_GPU_OVERLAP");
        return v && v[0] == '1';
    }();
    ForEachImageInRegion(address, max_size, [&](ImageId image_id, Image& image) {
        // Only consider images that match base address.
        if (image.info.guest_address != address &&
            !(overlap && image.info.guest_address >= address &&
              image.info.guest_address + image.info.guest_size <= address + max_size)) {
            return;
        }
        // Ensure image is reuploaded when accessed again.
        image.flags |= ImageFlagBits::GpuDirty;
    });
}

void TextureCache::UnmapMemory(VAddr cpu_addr, size_t size) {
    std::scoped_lock lk{mutex};

    ImageIds deleted_images;
    ForEachImageInRegion(cpu_addr, size, [&](ImageId id, Image&) { deleted_images.push_back(id); });
    for (const ImageId id : deleted_images) {
        // TODO: Download image data back to host.
        FreeImage(id);
    }
}

ImageId TextureCache::ResolveDepthOverlap(const ImageInfo& requested_info, BindingType binding,
                                          ImageId cache_image_id) {
    auto& cache_image = slot_images[cache_image_id];

    if (!cache_image.info.props.is_depth && !requested_info.props.is_depth) {
        return {};
    }

    const bool stencil_match =
        requested_info.props.has_stencil == cache_image.info.props.has_stencil;
    const bool bpp_match = requested_info.num_bits == cache_image.info.num_bits;

    // If an image in the cache has less slices we need to expand it
    bool recreate = cache_image.info.resources < requested_info.resources;

    switch (binding) {
    case BindingType::Texture:
        // The guest requires a depth sampled texture, but cache can offer only Rxf. Need to
        // recreate the image.
        recreate |= requested_info.props.is_depth && !cache_image.info.props.is_depth;
        break;
    case BindingType::Storage:
        // If the guest is going to use previously created depth as storage, the image needs to be
        // recreated. (TODO: Probably a case with linear rgba8 aliasing is legit)
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::RenderTarget:
        // Render target can have only Rxf format. If the cache contains only Dx[S8] we need to
        // re-create the image.
        ASSERT(!requested_info.props.is_depth);
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::DepthTarget:
        // The guest has requested previously allocated texture to be bound as a depth target.
        // In this case we need to convert Rx float to a Dx[S8] as requested
        recreate |= !cache_image.info.props.is_depth;

        // The guest is trying to bind a depth target and cache has it. Need to be sure that aspects
        // and bpp match
        recreate |= cache_image.info.props.is_depth && !(stencil_match && bpp_match);
        break;
    default:
        break;
    }

    if (recreate) {
        auto new_info = requested_info;
        new_info.resources = std::max(requested_info.resources, cache_image.info.resources);
        new_info.UpdateSize();
        // sotc: the game switches some depth buffers between depth and colour use every frame
        // (~100 conversions/s in the open world): each one created an image and freed the old
        // one (~3% of the GPU thread). The old image is parked as the new one's twin instead,
        // and the next conversion back reuses it when it has exactly the requested layout. The
        // copy stays: the contents move every time. Opt-in (SOTC_DEPTH_TWIN=1) until measured:
        // the A/B runs so far were all disturbed.
        static const bool twins = [] {
            const char* v = std::getenv("SOTC_DEPTH_TWIN");
            return v && v[0] == '1';
        }();
        ImageId new_image_id{};
        if (const ImageId twin_id = cache_image.parked_twin) {
            cache_image.parked_twin = {};
            const auto& twin = slot_images[twin_id].info;
            const bool same = twin.guest_address == new_info.guest_address &&
                              twin.guest_size == new_info.guest_size &&
                              twin.pixel_format == new_info.pixel_format &&
                              twin.type == new_info.type && twin.size == new_info.size &&
                              twin.pitch == new_info.pitch &&
                              !(twin.resources < new_info.resources) &&
                              !(new_info.resources < twin.resources) &&
                              twin.num_samples == new_info.num_samples &&
                              twin.num_bits == new_info.num_bits &&
                              twin.tile_mode == new_info.tile_mode &&
                              twin.props.is_depth == new_info.props.is_depth &&
                              twin.props.has_stencil == new_info.props.has_stencil &&
                              twin.props.is_tiled == new_info.props.is_tiled;
            const auto& twin_image = slot_images[twin_id];
            const bool idle = !twin_image.binding.is_bound && !twin_image.binding.is_target &&
                              twin_image.backing &&
                              twin_image.backing->num_samples == new_info.num_samples;
            if (twins && same && idle) {
                // Everything that shaped the Vulkan image matched: take the request's metadata
                // (meta areas, layout details) as a new image would.
                slot_images[twin_id].info = new_info;
                UnparkImage(twin_id);
                new_image_id = twin_id;
            } else {
                ReclaimImage(twin_id);
            }
        }
        if (!new_image_id) {
            new_image_id = slot_images.insert(instance, runtime, slot_image_views, new_info);
            RegisterImage(new_image_id);
        }

        // Inherit image usage
        auto& new_image = slot_images[new_image_id];
        auto& old_image = slot_images[cache_image_id];
        new_image.usage = old_image.usage;
        new_image.flags &= ~ImageFlagBits::Dirty;
        // When creating a depth buffer through overlap resolution don't clear it on first use.
        new_image.info.meta_info.htile_clear_mask = 0;
        runtime.CopyColorAndDepth(&old_image, &new_image);

        if (twins) {
            ParkImage(cache_image_id);
            slot_images[new_image_id].parked_twin = cache_image_id;
        } else {
            FreeImage(cache_image_id);
        }
        return new_image_id;
    }

    // Will be handled by view
    return cache_image_id;
}

std::tuple<ImageId, int, int> TextureCache::ResolveOverlap(const ImageInfo& image_info,
                                                           BindingType binding,
                                                           ImageId cache_image_id,
                                                           ImageId merged_image_id) {
    auto& cache_image = slot_images[cache_image_id];
    const bool safe_to_delete =
        scheduler.CurrentTick() - cache_image.tick_accessed_last > NumFramesBeforeRemoval;

    // Equal address
    if (image_info.guest_address == cache_image.info.guest_address) {
        const u32 lhs_block_size = image_info.num_bits * image_info.num_samples;
        const u32 rhs_block_size = cache_image.info.num_bits * cache_image.info.num_samples;
        if (image_info.BlockDim() != cache_image.info.BlockDim() ||
            lhs_block_size != rhs_block_size) {
            // Very likely this kind of overlap is caused by allocation from a pool.
            if (safe_to_delete) {
                FreeImage(cache_image_id);
            }
            return {merged_image_id, -1, -1};
        }

        if (const auto depth_image_id = ResolveDepthOverlap(image_info, binding, cache_image_id)) {
            return {depth_image_id, -1, -1};
        }

        // Compressed view of uncompressed image with same block size.
        if (image_info.props.is_block && !cache_image.info.props.is_block) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        if (image_info.guest_size == cache_image.info.guest_size &&
            (image_info.type == AmdGpu::ImageType::Color3D ||
             cache_image.info.type == AmdGpu::ImageType::Color3D)) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        const bool pow2_padding_only =
            image_info.props.is_pow2 != cache_image.info.props.is_pow2 &&
            image_info.tile_mode == cache_image.info.tile_mode &&
            image_info.size == cache_image.info.size &&
            image_info.pitch == cache_image.info.pitch && image_info.resources.levels == 1 &&
            cache_image.info.resources.levels == 1 && image_info.resources.layers == 1 &&
            cache_image.info.resources.layers == 1;

        // Size and resources are less than or equal, use image view.
        if (image_info.pixel_format != cache_image.info.pixel_format ||
            image_info.guest_size <= cache_image.info.guest_size || pow2_padding_only) {
            auto result_id = merged_image_id ? merged_image_id : cache_image_id;
            const auto& result_image = slot_images[result_id];
            const bool is_compatible =
                IsVulkanFormatCompatible(result_image.info.pixel_format, image_info.pixel_format);
            return {is_compatible ? result_id : ImageId{}, -1, -1};
        }

        // Size and resources are greater, expand the image.
        if (image_info.type == cache_image.info.type &&
            image_info.resources > cache_image.info.resources) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        // Size is greater but resources are not, because the tiling mode is different.
        // Likely the address is reused for a image with a different tiling mode.
        if (image_info.tile_mode != cache_image.info.tile_mode) {
            if (safe_to_delete) {
                FreeImage(cache_image_id);
            }
            return {merged_image_id, -1, -1};
        }

        // Enhanced debug logging for unreachable case
        // Calculate expected size based on format and dimensions
        u64 expected_size =
            (static_cast<u64>(image_info.size.width) * static_cast<u64>(image_info.size.height) *
             static_cast<u64>(image_info.size.depth) * static_cast<u64>(image_info.num_bits) / 8);
        LOG_ERROR(Render_Vulkan,
                  "Unresolvable image overlap with equal memory address:\n"
                  "=== OLD IMAGE (cached) ===\n"
                  "  Address:        {:#x}\n"
                  "  Size:           {:#x} bytes\n"
                  "  Format:         {}\n"
                  "  Type:           {}\n"
                  "  Width:          {}\n"
                  "  Height:         {}\n"
                  "  Depth:          {}\n"
                  "  Pitch:          {}\n"
                  "  Mip levels:     {}\n"
                  "  Array layers:   {}\n"
                  "  Samples:        {}\n"
                  "  Tile mode:      {:#x}\n"
                  "  Block size:     {} bits\n"
                  "  Is block-comp:  {}\n"
                  "  Guest size:     {:#x}\n"
                  "  Last accessed:  tick {}\n"
                  "  Safe to delete: {}\n"
                  "  isPow2:         {}\n"
                  "  Alt tile:       {}\n"
                  "\n"
                  "=== NEW IMAGE (requested) ===\n"
                  "  Address:        {:#x}\n"
                  "  Size:           {:#x} bytes\n"
                  "  Format:         {}\n"
                  "  Type:           {}\n"
                  "  Width:          {}\n"
                  "  Height:         {}\n"
                  "  Depth:          {}\n"
                  "  Pitch:          {}\n"
                  "  Mip levels:     {}\n"
                  "  Array layers:   {}\n"
                  "  Samples:        {}\n"
                  "  Tile mode:      {:#x}\n"
                  "  Block size:     {} bits\n"
                  "  Is block-comp:  {}\n"
                  "  Guest size:     {:#x}\n"
                  "  isPow2:         {}\n"
                  "  Alt tile:       {}\n"
                  "\n"
                  "=== COMPARISON ===\n"
                  "  Same format:           {}\n"
                  "  Same type:             {}\n"
                  "  Same tile mode:        {}\n"
                  "  Same block size:       {}\n"
                  "  Same BlockDim:         {}\n"
                  "  Same pitch:            {}\n"
                  "  Same pow2:             {}\n"
                  "  Same alt tile:         {}\n"
                  "  Old resources <= new:  {} (old: {}, new: {})\n"
                  "  Old size <= new size:  {}\n"
                  "  Expected size (calc):  {} bytes\n"
                  "  Size ratio (new/expected): {:.2f}x\n"
                  "  Size ratio (new/old):  {:.2f}x\n"
                  "  Old vs expected diff:  {} bytes ({:+.2f}%)\n"
                  "  New vs expected diff:  {} bytes ({:+.2f}%)\n"
                  "  Merged image ID:       {}\n"
                  "  Binding type:          {}\n"
                  "  Current tick:          {}\n"
                  "  Age (ticks since last access): {}",

                  // Old image details
                  cache_image.info.guest_address, cache_image.info.guest_size,
                  vk::to_string(cache_image.info.pixel_format),
                  static_cast<int>(cache_image.info.type), cache_image.info.size.width,
                  cache_image.info.size.height, cache_image.info.size.depth, cache_image.info.pitch,
                  cache_image.info.resources.levels, cache_image.info.resources.layers,
                  cache_image.info.num_samples, static_cast<u32>(cache_image.info.tile_mode),
                  cache_image.info.num_bits, +cache_image.info.props.is_block,
                  cache_image.info.guest_size, cache_image.tick_accessed_last, safe_to_delete,
                  bool(cache_image.info.props.is_pow2), cache_image.info.alt_tile,

                  // New image details
                  image_info.guest_address, image_info.guest_size,
                  vk::to_string(image_info.pixel_format), static_cast<int>(image_info.type),
                  image_info.size.width, image_info.size.height, image_info.size.depth,
                  image_info.pitch, image_info.resources.levels, image_info.resources.layers,
                  image_info.num_samples, static_cast<u32>(image_info.tile_mode),
                  image_info.num_bits, image_info.props.is_block, image_info.guest_size,
                  bool(image_info.props.is_pow2), image_info.alt_tile,

                  // Comparison
                  (image_info.pixel_format == cache_image.info.pixel_format),
                  (image_info.type == cache_image.info.type),
                  (image_info.tile_mode == cache_image.info.tile_mode),
                  (image_info.num_bits == cache_image.info.num_bits),
                  (image_info.BlockDim() == cache_image.info.BlockDim()),
                  (image_info.pitch == cache_image.info.pitch),
                  (image_info.props.is_pow2 == cache_image.info.props.is_pow2),
                  (image_info.alt_tile == cache_image.info.alt_tile),
                  (cache_image.info.resources <= image_info.resources),
                  cache_image.info.resources.levels, image_info.resources.levels,
                  (cache_image.info.guest_size <= image_info.guest_size), expected_size,

                  // Size ratios
                  static_cast<double>(image_info.guest_size) / expected_size,
                  static_cast<double>(image_info.guest_size) / cache_image.info.guest_size,

                  // Difference between actual and expected sizes with percentages
                  static_cast<s64>(cache_image.info.guest_size) - static_cast<s64>(expected_size),
                  (static_cast<double>(cache_image.info.guest_size) / expected_size - 1.0) * 100.0,

                  static_cast<s64>(image_info.guest_size) - static_cast<s64>(expected_size),
                  (static_cast<double>(image_info.guest_size) / expected_size - 1.0) * 100.0,

                  merged_image_id.index, static_cast<int>(binding), scheduler.CurrentTick(),
                  scheduler.CurrentTick() - cache_image.tick_accessed_last);

        UNREACHABLE_MSG("Encountered unresolvable image overlap with equal memory address.");
    }

    // Right overlap, the image requested is a possible subresource of the image from cache.
    if (image_info.guest_address > cache_image.info.guest_address) {
        if (auto mip = image_info.MipOf(cache_image.info); mip >= 0) {
            if (auto slice = image_info.SliceOf(cache_image.info, mip); slice >= 0) {
                return {cache_image_id, mip, slice};
            }
        }

        // Image isn't a subresource but a chance overlap.
        if (safe_to_delete) {
            FreeImage(cache_image_id);
        }

        return {{}, -1, -1};
    } else {
        // Left overlap, the image from cache is a possible subresource of the image requested
        if (auto mip = cache_image.info.MipOf(image_info); mip >= 0) {
            if (auto slice = cache_image.info.SliceOf(image_info, mip); slice >= 0) {
                // We have a larger image created and a separate one, representing a subres of it
                // bound as render target. In this case we need to rebind render target.
                if (cache_image.binding.is_target) {
                    cache_image.binding.needs_rebind = 1u;
                    if (merged_image_id) {
                        GetImage(merged_image_id).binding.is_target = 1u;
                    }

                    FreeImage(cache_image_id);
                    return {merged_image_id, -1, -1};
                }

                // We need to have a larger, already allocated image to copy this one into
                if (merged_image_id) {
                    auto& merged_image = slot_images[merged_image_id];
                    runtime.CopyMip(&cache_image, &merged_image, mip, slice);
                    FreeImage(cache_image_id);
                }
            }
        }
    }

    return {merged_image_id, -1, -1};
}

ImageId TextureCache::ExpandImage(const ImageInfo& info, ImageId image_id) {
    const auto new_image_id = slot_images.insert(instance, runtime, slot_image_views, info);
    RegisterImage(new_image_id);

    auto& src_image = slot_images[image_id];
    auto& new_image = slot_images[new_image_id];

    RefreshImage(new_image);
    runtime.CopyImage(&src_image, &new_image);

    if (src_image.binding.is_bound || src_image.binding.is_target) {
        src_image.binding.needs_rebind = 1u;
    }

    FreeImage(image_id);
    TrackImage(new_image_id);
    return new_image_id;
}

ImageId TextureCache::FindImage(ImageDesc& desc, bool exact_fmt) {
    const auto& info = desc.info;
    ASSERT(info.guest_address != 0);

    std::scoped_lock lock{mutex};
    // sotc: thousands of lookups per frame find the same images; a perfect match stays one
    // until an image is registered or unregistered (SOTC_FIND_MEMO=0: always search).
    static const bool memo_enabled = [] {
        const char* v = std::getenv("SOTC_FIND_MEMO");
        return !v || v[0] != '0';
    }();
    const u64 memo_hash = (info.guest_address * 0x9E3779B97F4A7C15ULL) ^
                          (u64{info.guest_size} * 0xC2B2AE3D27D4EB4FULL) ^
                          (u64(info.pixel_format) << 7) ^ (u64{info.size.width} << 23) ^
                          (u64{info.size.height} << 41) ^ u64{exact_fmt};
    FindMemo& memo = find_memos[(memo_hash ^ (memo_hash >> 31)) % NumFindMemos];
    // sotc: validate the memo against the generation of its guest_address only: perfect
    // matches share guest_address (so they share the first page, whose list keeps insertion
    // order on erase), hence (un)registering images elsewhere cannot change the result.
    // SOTC_MEMO_ADDR_GEN=0: any register/unregister invalidates every memo.
    static const bool memo_addr_gen = [] {
        const char* v = std::getenv("SOTC_MEMO_ADDR_GEN");
        return !v || v[0] != '0';
    }();
    const u64 memo_gen = memo_addr_gen ? u64{addr_gens[AddrGenSlot(info.guest_address)]}
                                       : image_set_generation;
    if (memo_enabled && memo.generation == memo_gen &&
        memo.guest_address == info.guest_address && memo.guest_size == info.guest_size &&
        memo.size == info.size && memo.pixel_format == info.pixel_format &&
        memo.type == info.type && memo.exact_fmt == exact_fmt) {
        Image& image = slot_images[memo.image_id];
        if (!(image.info.resources < info.resources)) {
            image.tick_accessed_last = scheduler.CurrentTick();
            TouchImage(image);
            return memo.image_id;
        }
    }
    // sotc: SOTC_IMG_WATCH=<address>: every lookup (texture, storage, render target) that
    // overlaps it, first 200 (diagnostic for the empty AO input behind the black shadows).
    static const VAddr img_watch = [] {
        const char* v = std::getenv("SOTC_IMG_WATCH");
        return v ? static_cast<VAddr>(std::strtoull(v, nullptr, 0)) : VAddr{0};
    }();
    if (img_watch && info.guest_address <= img_watch &&
        img_watch < info.guest_address + info.guest_size) {
        static int shown = 0;
        if (shown++ < 200) {
            std::fprintf(stderr, "SOTC img-watch type %u %#llx+%#x %ux%u fmt %d tile %d\n",
                         static_cast<u32>(desc.type),
                         static_cast<unsigned long long>(info.guest_address), info.guest_size,
                         info.size.width, info.size.height, static_cast<int>(info.pixel_format),
                         static_cast<int>(info.tile_mode));
        }
    }
    ImageIds image_ids;
    ForEachImageInRegion(info.guest_address, info.guest_size,
                         [&](ImageId image_id, Image& image) { image_ids.push_back(image_id); });

    ImageId image_id{};

    // Check for a perfect match first
    for (const auto& cache_id : image_ids) {
        auto& cache_image = slot_images[cache_id];
        if (cache_image.info.guest_address != info.guest_address) {
            continue;
        }
        if (cache_image.info.guest_size != info.guest_size) {
            continue;
        }
        if (cache_image.info.size != info.size) {
            continue;
        }
        if (!IsVulkanFormatCompatible(cache_image.info.pixel_format, info.pixel_format) ||
            (cache_image.info.type != info.type && info.size != Extent3D{1, 1, 1})) {
            continue;
        }
        if (exact_fmt && info.pixel_format != cache_image.info.pixel_format) {
            continue;
        }
        image_id = cache_id;
    }
    if (image_id && !(slot_images[image_id].info.resources < info.resources)) {
        memo = {info.guest_address, info.guest_size, info.size, info.pixel_format, info.type,
                exact_fmt,         memo_gen, image_id}; // sotc: per-address generation
    }

    // Try to resolve overlaps (if any)
    int view_mip{-1};
    int view_slice{-1};
    if (!image_id) {
        for (const auto& cache_id : image_ids) {
            view_mip = -1;
            view_slice = -1;

            const auto& merged_info = image_id ? slot_images[image_id].info : info;
            auto [overlap_image_id, overlap_view_mip, overlap_view_slice] =
                ResolveOverlap(merged_info, desc.type, cache_id, image_id);
            if (overlap_image_id) {
                image_id = overlap_image_id;
                view_mip = overlap_view_mip;
                view_slice = overlap_view_slice;
            }
        }
    }

    if (image_id) {
        Image& image_resolved = slot_images[image_id];
        if (exact_fmt && info.pixel_format != image_resolved.info.pixel_format) {
            // Cannot reuse this image as we need the exact requested format.
            image_id = {};
        } else if (image_resolved.info.resources < info.resources) {
            // The image was clearly picked up wrong.
            FreeImage(image_id);
            image_id = {};
            LOG_WARNING(Render_Vulkan, "Image overlap resolve failed");
        }
    }
    // Create and register a new image
    if (!image_id) {
        image_id = slot_images.insert(instance, runtime, slot_image_views, info);
        RegisterImage(image_id);
    }

    Image& image = slot_images[image_id];
    image.tick_accessed_last = scheduler.CurrentTick();
    TouchImage(image);

    // If the image requested is a subresource of the image from cache record its location.
    if (view_mip > 0) {
        desc.view_info.range.base.level = view_mip;
    }
    if (view_slice > 0) {
        desc.view_info.range.base.layer = view_slice;
    }

    return image_id;
}

ImageId TextureCache::FindImageFromRange(VAddr address, size_t size, bool ensure_valid) {
    ImageIds image_ids;
    ForEachImageInRegion(address, size, [&](ImageId image_id, Image& image) {
        if (image.info.guest_address != address) {
            return;
        }
        if (ensure_valid && !image.SafeToDownload()) {
            return;
        }
        image_ids.push_back(image_id);
    });
    if (image_ids.size() == 1) {
        // Sometimes image size might not exactly match with requested buffer size
        // If we only found 1 candidate image use it without too many questions.
        return image_ids.back();
    }
    if (!image_ids.empty()) {
        for (s32 i = 0; i < image_ids.size(); ++i) {
            Image& image = slot_images[image_ids[i]];
            if (image.info.guest_size == size) {
                return image_ids[i];
            }
        }
        LOG_WARNING(Render_Vulkan,
                    "Failed to find exact image match for copy addr={:#x}, size={:#x}", address,
                    size);
    }
    return {};
}

ImageView& TextureCache::FindTexture(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    if (desc.type == BindingType::Storage) {
        image.flags |= ImageFlagBits::GpuModified;
        if (readback_linear_images && (!image.info.props.is_tiled || image.info.size.width <= 8) &&
            image.info.guest_address != 0) {
            std::unique_lock lk{download_images_mutex};
            download_images.emplace(image_id);
        }
    }
    UpdateImage(image_id);
    return image.FindView(desc.view_info);
}

ImageView& TextureCache::FindRenderTarget(ImageId image_id, const ImageDesc& desc,
                                          bool update) {
    Image& image = slot_images[image_id];
    image.flags |= ImageFlagBits::GpuModified;
    if (readback_linear_images && (!image.info.props.is_tiled || image.info.size.width <= 8)) {
        std::unique_lock lk{download_images_mutex};
        download_images.emplace(image_id);
    }
    image.usage.render_target = 1u;
    if (update) {
        UpdateImage(image_id);
    } else if (True(image.flags & ImageFlagBits::Dirty) ||
               (True(image.flags & ImageFlagBits::Registered) &&
                (image.track_addr != image.info.guest_address ||
                 image.track_addr_end != image.info.guest_address + image.info.guest_size))) {
        // sotc: update=false: the caller (BeginRendering) ran UpdateImage just before, so a
        // second one only matters when the image is still Dirty (RefreshImage's MaybeCpuDirty
        // hash match clears only MaybeCpuDirty, a GpuDirty upload must still happen, and a CPU
        // fault may have dirtied it meanwhile) or lost page tracking (TrackImage would re-track).
        // Otherwise TrackImage, TouchImage (same gc_tick, already most recent) and RefreshImage
        // would all be no-ops.
        UpdateImage(image_id);
    }

    // Register meta data for this color buffer
    if (desc.info.meta_info.cmask_addr) {
        surface_metas.emplace(desc.info.meta_info.cmask_addr,
                              MetaDataInfo{.type = MetaType::CMask});
        image.info.meta_info.cmask_addr = desc.info.meta_info.cmask_addr;
    }

    if (desc.info.meta_info.fmask_addr) {
        surface_metas.emplace(desc.info.meta_info.fmask_addr,
                              MetaDataInfo{.type = MetaType::FMask});
        image.info.meta_info.fmask_addr = desc.info.meta_info.fmask_addr;
    }

    return image.FindView(desc.view_info, false);
}

ImageView& TextureCache::FindDepthTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    image.flags |= ImageFlagBits::GpuModified;
    image.usage.depth_target = 1u;
    UpdateImage(image_id);

    // Register meta data for this depth buffer
    if (desc.info.meta_info.htile_addr) {
        surface_metas.emplace(desc.info.meta_info.htile_addr,
                              MetaDataInfo{.type = MetaType::HTile,
                                           .clear_mask = image.info.meta_info.htile_clear_mask});
        image.info.meta_info.htile_addr = desc.info.meta_info.htile_addr;
    }

    // If there is a stencil attachment, link depth and stencil.
    if (desc.info.stencil_addr != 0) {
        ImageId stencil_id{};
        ForEachImageInRegion(desc.info.stencil_addr, desc.info.stencil_size,
                             [&](ImageId image_id, Image& image) {
                                 if (image.info.guest_address == desc.info.stencil_addr) {
                                     stencil_id = image_id;
                                 }
                             });
        if (!stencil_id) {
            ImageInfo info{};
            info.guest_address = desc.info.stencil_addr;
            info.guest_size = desc.info.stencil_size;
            info.size = desc.info.size;
            stencil_id = slot_images.insert(instance, runtime, slot_image_views, info);
            RegisterImage(stencil_id);
        }
        Image& stencil_image = slot_images[stencil_id];
        TouchImage(stencil_image);
        stencil_image.AssociateDepth(image_id, image.image_uid);
    }

    return image.FindView(desc.view_info, false);
}

void TextureCache::RefreshImage(Image& image) {
    if (False(image.flags & ImageFlagBits::Dirty) || image.info.num_samples > 1) {
        return;
    }
    // sotc: RefreshImage always runs under `mutex` (UpdateImage, FindImage -> ExpandImage) and can
    // reach EnsureResident through ObtainBufferForImage; see TryEmergencyCollect.
    ++refresh_depth;
    SCOPE_EXIT {
        --refresh_depth;
    };

    RENDERER_TRACE;
    TRACE_HINT(fmt::format("{:x}:{:x}", image.info.guest_address, image.info.guest_size));

    if (True(image.flags & ImageFlagBits::MaybeCpuDirty) &&
        False(image.flags & ImageFlagBits::CpuDirty)) {
        // The image size should be less than page size to be considered MaybeCpuDirty
        // So this calculation should be very uncommon and reasonably fast
        // For now we'll just check up to 64 first pixels
        const auto addr = std::bit_cast<u8*>(image.info.guest_address);
        const u32 w = std::min(image.info.size.width, u32(8));
        const u32 h = std::min(image.info.size.height, u32(8));

        const u32 s_w = image.info.props.is_block ? Common::DivCeil(w, 4u) : w;
        const u32 s_h = image.info.props.is_block ? Common::DivCeil(h, 4u) : h;
        const u32 size = s_w * s_h * (image.info.num_bits / 8);
        const u64 hash = XXH3_64bits(addr, size);
        if (image.hash == hash) {
            image.flags &= ~ImageFlagBits::MaybeCpuDirty;
            return;
        }
        image.hash = hash;
    }

    const u32 num_layers = image.info.resources.layers;
    const u32 num_mips = image.info.resources.levels;
    const bool is_gpu_modified = True(image.flags & ImageFlagBits::GpuModified);
    const bool is_gpu_dirty = True(image.flags & ImageFlagBits::GpuDirty);

    boost::container::small_vector<vk::BufferImageCopy, 14> image_copies;
    for (u32 m = 0; m < num_mips; m++) {
        const u32 width = std::max(image.info.size.width >> m, 1u);
        const u32 height = std::max(image.info.size.height >> m, 1u);
        const u32 depth =
            image.info.props.is_volume ? std::max(image.info.size.depth >> m, 1u) : 1u;
        const auto [mip_size, mip_pitch, mip_height, mip_offset] = image.info.mips_layout[m];

        // Protect GPU modified resources from accidental CPU reuploads.
        if (is_gpu_modified && !is_gpu_dirty) {
            const u8* addr = std::bit_cast<u8*>(image.info.guest_address);
            const u64 hash = XXH3_64bits(addr + mip_offset, mip_size);
            if (image.mip_hashes[m] == hash) {
                continue;
            }
            image.mip_hashes[m] = hash;
        }

        const u32 extent_width = mip_pitch ? std::min(mip_pitch, width) : width;
        const u32 extent_height = mip_height ? std::min(mip_height, height) : height;
        image_copies.push_back({
            .bufferOffset = mip_offset,
            .bufferRowLength = mip_pitch,
            .bufferImageHeight = mip_height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = m,
                .baseArrayLayer = 0,
                .layerCount = num_layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {extent_width, extent_height, depth},
        });
    }

    if (image_copies.empty()) {
        image.flags &= ~ImageFlagBits::Dirty;
        return;
    }

    scheduler.EndRendering();

    const auto [in_buffer, in_offset] =
        buffer_cache.ObtainBufferForImage(image.info.guest_address, image.info.guest_size);
    const auto [buffer, offset] = tile_manager.DetileImage(in_buffer, in_offset, image.info);
    for (auto& copy : image_copies) {
        copy.bufferOffset += offset;
    }

    runtime.UploadImage(&image, buffer, image_copies);
}

vk::Sampler TextureCache::GetSampler(const AmdGpu::Sampler& sampler,
                                     AmdGpu::BorderColorBuffer border_color_base,
                                     const bool is_depth) {
    // sotc: per-thread memo of S# -> sampler for the current GC tick: a repeated S# skips the
    // hash, the lock, the map and the LRU touch (already touched at this tick). Samplers are only
    // erased by the garbage collector, which bumps sampler_epoch. SOTC_SAMPLER_MEMO=0 disables.
    static_assert(sizeof(AmdGpu::Sampler) == 16);
    static const bool sampler_memo = [] {
        const char* v = std::getenv("SOTC_SAMPLER_MEMO");
        return !v || v[0] != '0';
    }();
    struct SamplerMemo {
        u64 raw[2];
        u64 tick = ~0ULL;
        u64 epoch;
        bool is_depth;
        vk::Sampler handle;
    };
    thread_local std::array<SamplerMemo, 64> memo{};
    u64 raw[2];
    std::memcpy(raw, &sampler, sizeof(raw));
    const u64 tick = gc_tick;
    const u64 epoch = sampler_epoch.load(std::memory_order_acquire);
    SamplerMemo* slot = nullptr;
    if (sampler_memo) {
        slot = &memo[((raw[0] * 0x9E3779B97F4A7C15ULL) ^ raw[1] ^ (is_depth ? 1 : 0)) >> 58];
        if (slot->tick == tick && slot->epoch == epoch && slot->is_depth == is_depth &&
            slot->raw[0] == raw[0] && slot->raw[1] == raw[1]) {
            return slot->handle;
        }
    }
    // Compare and plain uses of one S# need separate samplers.
    const u64 hash = HashCombine(XXH3_64bits(&sampler, sizeof(sampler)), is_depth);

    std::scoped_lock lock{samplers_mutex};
    const auto [it, new_sampler] =
        samplers.try_emplace(hash, instance, sampler, border_color_base, is_depth);
    if (new_sampler) {
        samplers.at(hash).lru_id = sampler_lru_cache.Insert(hash, tick);
    } else {
        sampler_lru_cache.Touch(it->second.lru_id, tick);
    }
    if (slot) {
        *slot = SamplerMemo{{raw[0], raw[1]}, tick, epoch, is_depth, it->second.Handle()};
    }
    return it->second.Handle();
}

void TextureCache::RegisterImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered),
               "Trying to register an already registered image");
    image.flags |= ImageFlagBits::Registered;
    ++image_set_generation;
    ++addr_gens[AddrGenSlot(image.info.guest_address)]; // sotc: FindMemo per-address gen
    total_used_memory += Common::AlignUp(image.info.guest_size, 1024);
    image.lru_id = lru_cache.Insert(image_id, gc_tick);
    ForEachPage(image.info.guest_address, image.info.guest_size,
                [this, image_id](u64 page) { page_table[page].push_back(image_id); });
}

void TextureCache::UnregisterImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(True(image.flags & ImageFlagBits::Registered),
               "Trying to unregister an already unregistered image");
    image.flags &= ~ImageFlagBits::Registered;
    ++image_set_generation;
    ++addr_gens[AddrGenSlot(image.info.guest_address)]; // sotc: FindMemo per-address gen
    lru_cache.Free(image.lru_id);
    total_used_memory -= Common::AlignUp(image.info.guest_size, 1024);
    ForEachPage(image.info.guest_address, image.info.guest_size, [this, image_id](u64 page) {
        const auto page_it = page_table.find(page);
        if (page_it == nullptr) {
            UNREACHABLE_MSG("Unregistering unregistered page=0x{:x}", page << PageShift);
            return;
        }
        auto& image_ids = *page_it;
        const auto vector_it = std::ranges::find(image_ids, image_id);
        if (vector_it == image_ids.end()) {
            ASSERT_MSG(false, "Unregistering unregistered image in page=0x{:x}", page << PageShift);
            return;
        }
        image_ids.erase(vector_it);
    });
}

void TextureCache::TrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_begin = image.info.guest_address;
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image_begin == image.track_addr && image_end == image.track_addr_end) {
        return;
    }

    if (!image.IsTracked()) {
        // Re-track the whole image
        image.track_addr = image_begin;
        image.track_addr_end = image_end;
        tracker.UpdatePageWatchers(image_begin, image.info.guest_size, PageOp::Track);
    } else {
        if (image_begin < image.track_addr) {
            TrackImageHead(image_id);
        }
        if (image.track_addr_end < image_end) {
            TrackImageTail(image_id);
        }
    }
}

void TextureCache::TrackImageHead(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_begin = image.info.guest_address;
    if (image_begin == image.track_addr) {
        return;
    }
    ASSERT(image.track_addr != 0 && image_begin < image.track_addr);
    const auto size = image.track_addr - image_begin;
    image.track_addr = image_begin;
    tracker.UpdatePageWatchers(image_begin, size, PageOp::Track);
}

void TextureCache::TrackImageTail(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image_end == image.track_addr_end) {
        return;
    }
    ASSERT(image.track_addr_end != 0 && image.track_addr_end < image_end);
    const auto addr = image.track_addr_end;
    const auto size = image_end - image.track_addr_end;
    image.track_addr_end = image_end;
    tracker.UpdatePageWatchers(addr, size, PageOp::Track);
}

void TextureCache::UntrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!image.IsTracked()) {
        return;
    }
    const auto addr = image.track_addr;
    const auto size = image.track_addr_end - image.track_addr;
    image.track_addr = 0;
    image.track_addr_end = 0;
    if (size != 0) {
        tracker.UpdatePageWatchers(addr, size, PageOp::Untrack);
    }
}

void TextureCache::UntrackImageHead(ImageId image_id) {
    auto& image = slot_images[image_id];
    const auto image_begin = image.info.guest_address;
    if (!image.IsTracked() || image_begin < image.track_addr) {
        return;
    }
    const auto addr = tracker.GetNextPageAddr(image_begin);
    const auto size = addr - image_begin;
    image.track_addr = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
    tracker.UpdatePageWatchers(image_begin, size, PageOp::Untrack);
}

void TextureCache::UntrackImageTail(ImageId image_id) {
    auto& image = slot_images[image_id];
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (!image.IsTracked() || image.track_addr_end < image_end) {
        return;
    }
    ASSERT(image.track_addr_end != 0);
    const auto addr = tracker.GetPageAddr(image_end);
    const auto size = image_end - addr;
    image.track_addr_end = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
    tracker.UpdatePageWatchers(addr, size, PageOp::Untrack);
}

bool TextureCache::TryEmergencyCollect(u32 level) {
    // sotc: inside RefreshImage this thread already holds `mutex`; EmergencyCollect would lock it
    // again (self-deadlock), and freeing images there could erase the one being refreshed. Defer
    // the collection to the next RunGarbageCollector (a submit, no lock held) instead.
    if (refresh_depth > 0) {
        // The caller cannot escalate in place, so ask for the deepest level (2, stored as 3).
        static_cast<void>(level);
        constexpr u32 wanted = 3;
        u32 pending = pending_emergency.load(std::memory_order_relaxed);
        while (pending < wanted &&
               !pending_emergency.compare_exchange_weak(pending, wanted, std::memory_order_relaxed)) {
        }
        // sotc: no counter here any more (it was never read); the warning reports it.
        LOG_WARNING(Render, "Emergency texture collection deferred (texture cache lock held)");
        return false;
    }
    EmergencyCollect(level);
    return true;
}

void TextureCache::EmergencyCollect(u32 level) {
    std::scoped_lock lock{mutex};
    // sotc: parked depth/colour twins are not in the LRU; under pressure they go first.
    lru_cache.ForEachItemBelow(gc_tick + 1, [&](ImageId image_id) {
        auto& image = slot_images[image_id];
        if (const ImageId twin_id = image.parked_twin) {
            image.parked_twin = {};
            ReclaimImage(twin_id);
        }
        return false;
    });
    size_t num_deletions = level == 0 ? 512 : level == 1 ? 4096 : SIZE_MAX;
    size_t freed = 0;
    const u64 ticks = std::min<u64>(level == 0 ? 4 : level == 1 ? 2 : 1, gc_tick);
    lru_cache.ForEachItemBelow(gc_tick - ticks, [&](ImageId image_id) {
        if (num_deletions == 0) {
            return true;
        }
        auto& image = slot_images[image_id];
        if (image.SafeToDownload()) {
            if (image.info.IsTiled()) {
                return false; // Can't download non-linear images yet
            }
            DownloadImageMemory(image_id);
        }
        FreeImage(image_id);
        --num_deletions;
        ++freed;
        return false;
    });
    LOG_WARNING(Render, "Emergency texture collection (level {}) freed {} images", level, freed);
    SotcStats::vram_emergency.fetch_add(1, std::memory_order_relaxed);
}

void TextureCache::GarbageCollectImages() {
    // sotc: this runs on every submit (many per frame with periodic flushes), and the usage query
    // is a kernel call (~2% of the GPU command thread): refresh it at most every 20 ms.
    static std::chrono::steady_clock::time_point last_usage_query{};
    const auto now = std::chrono::steady_clock::now();
    if (instance.CanReportMemoryUsage() &&
        (SotcStats::OptOff() || now - last_usage_query >= std::chrono::milliseconds{20})) {
        last_usage_query = now;
        total_used_memory = instance.GetDeviceMemoryUsage();
        // sotc: shown with the frame rate (SOTC_FPS=1).
        SotcStats::vram_used.store(total_used_memory, std::memory_order_relaxed);
        SotcStats::vram_budget.store(instance.GetTotalMemoryBudget(), std::memory_order_relaxed);
        // What VMA holds in device-local heaps: its blocks against the allocations in them
        // (the difference is free space VMA keeps, i.e. fragmentation).
        VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
        vmaGetHeapBudgets(instance.GetAllocator(), budgets);
        const auto& memory_properties = instance.GetMemoryProperties();
        u64 vma_blocks = 0, vma_allocations = 0;
        for (u32 heap = 0; heap < memory_properties.memoryHeapCount; ++heap) {
            if (memory_properties.memoryHeaps[heap].flags & vk::MemoryHeapFlagBits::eDeviceLocal) {
                vma_blocks += budgets[heap].statistics.blockBytes;
                vma_allocations += budgets[heap].statistics.allocationBytes;
            }
        }
        SotcStats::vma_block_bytes.store(vma_blocks, std::memory_order_relaxed);
        SotcStats::vma_allocation_bytes.store(vma_allocations, std::memory_order_relaxed);
    }
    if (total_used_memory < trigger_gc_memory) {
        return;
    }
    std::scoped_lock lock{mutex};
    bool pressured = false;
    bool aggresive = false;
    u64 ticks_to_destroy = 0;
    size_t num_deletions = 0;

    const auto configure = [&](bool allow_aggressive) {
        pressured = total_used_memory >= pressure_gc_memory;
        aggresive = allow_aggressive && total_used_memory >= critical_gc_memory;
        ticks_to_destroy = aggresive ? 160 : pressured ? 80 : 16;
        ticks_to_destroy = std::min(ticks_to_destroy, gc_tick);
        num_deletions = aggresive ? 128 : pressured ? 20 : 10;
    };
    const auto clean_up = [&](ImageId image_id) {
        // sotc: kept images (GPU-written tiled ones below) also use up num_deletions, so a pass
        // stops early when they lead the LRU. Charging only freed images was measured worse on
        // a 6 GB card (attract demo 27.5 -> 18.5 FPS, 28 emergency collections instead of 0):
        // the extra frees were images needed again a few frames later, and the churn fragmented
        // VMA. Paging the overflow beats evicting the working set.
        if (num_deletions == 0) {
            return true;
        }
        --num_deletions;
        auto& image = slot_images[image_id];
        const bool download = image.SafeToDownload();
        const bool tiled = image.info.IsTiled();
        if (tiled && download) {
            // This is a workaround for now. We can't handle non-linear image downloads.
            SotcStats::gc_kept_tiled.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (download && !pressured) {
            return false;
        }
        if (download) {
            DownloadImageMemory(image_id);
        }
        FreeImage(image_id);
        SotcStats::gc_freed.fetch_add(1, std::memory_order_relaxed);
        if (total_used_memory < critical_gc_memory) {
            if (aggresive) {
                num_deletions >>= 2;
                aggresive = false;
                return false;
            }
            if (pressured && total_used_memory < pressure_gc_memory) {
                num_deletions >>= 1;
                pressured = false;
            }
        }
        return false;
    };

    // Try to remove anything old enough and not high priority.
    configure(false);
    lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);

    if (total_used_memory >= critical_gc_memory) {
        // If we are still over the critical limit, run an aggressive GC
        configure(true);
        lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);
    }
}

void TextureCache::GarbageCollectSamplers() {
    total_used_samplers = samplers.size();
    if (total_used_samplers < trigger_gc_samplers) {
        return;
    }
    std::scoped_lock lock{samplers_mutex};
    bool pressured = false;
    bool aggresive = false;
    u64 ticks_to_destroy = 0;
    size_t num_deletions = 0;

    const auto configure = [&](bool allow_aggressive) {
        pressured = total_used_samplers >= pressure_gc_samplers;
        aggresive = allow_aggressive && total_used_samplers >= critical_gc_samplers;
        ticks_to_destroy = aggresive ? 160 : pressured ? 80 : 16;
        ticks_to_destroy = std::min(ticks_to_destroy, gc_tick);
        num_deletions = aggresive ? 40 : pressured ? 20 : 10;
    };
    const auto clean_up = [&](u64 hash) {
        if (num_deletions == 0) {
            return true;
        }
        --num_deletions;
        const size_t lru_id = samplers.at(hash).lru_id;
        samplers.erase(hash);
        sampler_epoch.fetch_add(1, std::memory_order_release);
        sampler_lru_cache.Free(lru_id);
        return false;
    };

    // Try to remove anything old enough and not high priority.
    configure(false);
    sampler_lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);

    if (total_used_samplers >= critical_gc_samplers) {
        // If we are still over the critical limit, run an aggressive GC
        configure(true);
        sampler_lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);
    }
}

// sotc: where image memory goes, every 5 s with SOTC_FPS (printed on the "SOTC IMAGES" line):
// GPU-written tiled images (the GC cannot free them), GPU-written linear, streamed and other
// sampled images, the bytes below the first mip their T# can sample (what allocating only the
// reachable mips would save), and the bytes of images used in the last NumFramesBeforeRemoval
// ticks (the live working set). Recomputed from the live images, telemetry only.
void TextureCache::Census() {
    static const bool enabled = std::getenv("SOTC_FPS") != nullptr;
    static std::chrono::steady_clock::time_point last{};
    const auto now = std::chrono::steady_clock::now();
    if (!enabled || now - last < std::chrono::seconds{5}) {
        return;
    }
    last = now;
    u64 gpu_tiled = 0, gpu_linear = 0, streamed = 0, other = 0, below_base = 0, below_minlod = 0;
    u64 live = 0, count = 0;
    const u64 current_tick = scheduler.CurrentTick();
    std::scoped_lock lock{mutex};
    lru_cache.ForEachItemBelow(gc_tick, [&](ImageId image_id) {
        const Image& image = slot_images[image_id];
        if (!image.backing) {
            return;
        }
        const u64 host = image.GetHostImageSize();
        ++count;
        if (current_tick - image.tick_accessed_last <= NumFramesBeforeRemoval) {
            live += host;
        }
        if (True(image.flags & ImageFlagBits::GpuModified)) {
            (image.info.IsTiled() ? gpu_tiled : gpu_linear) += host;
            return;
        }
        (image.info.sotc_streamed ? streamed : other) += host;
        const auto& info = image.info;
        if (info.guest_size != 0) {
            if (info.sotc_reach_mip > 0) {
                below_base += host * info.mips_layout[info.sotc_reach_mip].offset / info.guest_size;
            }
            if (info.sotc_minlod_mip > 0) {
                below_minlod +=
                    host * info.mips_layout[info.sotc_minlod_mip].offset / info.guest_size;
            }
        }
    });
    SotcStats::census_gpu_tiled.store(gpu_tiled, std::memory_order_relaxed);
    SotcStats::census_gpu_linear.store(gpu_linear, std::memory_order_relaxed);
    SotcStats::census_streamed.store(streamed, std::memory_order_relaxed);
    SotcStats::census_other.store(other, std::memory_order_relaxed);
    SotcStats::census_below_base.store(below_base, std::memory_order_relaxed);
    SotcStats::census_below_minlod.store(below_minlod, std::memory_order_relaxed);
    SotcStats::census_live.store(live, std::memory_order_relaxed);
    SotcStats::census_count.store(count, std::memory_order_relaxed);
}

void TextureCache::RunGarbageCollector() {
    SCOPE_EXIT {
        ++gc_tick;
    };

    if (const u32 pending = pending_emergency.exchange(0, std::memory_order_relaxed)) {
        EmergencyCollect(pending - 1);
    }
    Census();
    GarbageCollectImages();
    GarbageCollectSamplers();
}

void TextureCache::TouchImage(const Image& image) {
    // sotc: an unregistered image (parked twin, or deleted this draw) has no LRU node any more.
    if (True(image.flags & ImageFlagBits::Registered)) {
        lru_cache.Touch(image.lru_id, gc_tick);
    }
}

void TextureCache::ParkImage(ImageId image_id) {
    UntrackImage(image_id);
    UnregisterImage(image_id);
    Image& image = slot_images[image_id];
    // As DeleteImage: its meta areas and pending downloads go now.
    const auto& meta_info = image.info.meta_info;
    if (meta_info.cmask_addr) {
        surface_metas.erase(meta_info.cmask_addr);
    }
    if (meta_info.fmask_addr) {
        surface_metas.erase(meta_info.fmask_addr);
    }
    if (meta_info.htile_addr) {
        surface_metas.erase(meta_info.htile_addr);
    }
    std::unique_lock lk{download_images_mutex};
    download_images.erase(image_id);
}

void TextureCache::UnparkImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    // The state of a newly created image (Image::Image), with a new identity.
    image.flags = ImageFlagBits::Dirty;
    image.track_addr = 0;
    image.track_addr_end = 0;
    image.depth_id = {};
    image.depth_uid = 0;
    image.parked_twin = {};
    std::fill(image.mip_hashes.begin(), image.mip_hashes.end(), u64{0});
    image.hash = 0;
    image.tick_accessed_last = 0;
    image.binding = {};
    image.usage = {};
    image.RenewIdentity();
    {
        std::unique_lock lk{download_images_mutex};
        download_images.erase(image_id);
    }
    RegisterImage(image_id);
}

void TextureCache::ReclaimImage(ImageId image_id) {
    // A parked twin: already untracked and unregistered, metas gone.
    {
        std::unique_lock lk{download_images_mutex};
        download_images.erase(image_id);
    }
    scheduler.DeferOperation([this, image_id] {
        Image& image = slot_images[image_id];
        for (auto& backing : image.backing_images) {
            for (const ImageViewId image_view_id : backing.image_view_ids) {
                slot_image_views.erase(image_view_id);
            }
        }
        slot_images.erase(image_id);
    });
}

void TextureCache::DeleteImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(!image.IsTracked(), "Image was not untracked");
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered), "Image was not unregistered");
    if (const ImageId twin_id = image.parked_twin) {
        image.parked_twin = {};
        ReclaimImage(twin_id);
    }

    // Remove any registered meta areas.
    const auto& meta_info = image.info.meta_info;
    if (meta_info.cmask_addr) {
        surface_metas.erase(meta_info.cmask_addr);
    }
    if (meta_info.fmask_addr) {
        surface_metas.erase(meta_info.fmask_addr);
    }
    if (meta_info.htile_addr) {
        surface_metas.erase(meta_info.htile_addr);
    }

    {
        std::unique_lock lk{download_images_mutex};
        if (download_images.contains(image_id)) {
            download_images.erase(image_id);
        }
    }

    // Reclaim image and any image views it references.
    scheduler.DeferOperation([this, image_id] {
        Image& image = slot_images[image_id];
        for (auto& backing : image.backing_images) {
            for (const ImageViewId image_view_id : backing.image_view_ids) {
                slot_image_views.erase(image_view_id);
            }
        }
        slot_images.erase(image_id);
    });
}

} // namespace VideoCore
