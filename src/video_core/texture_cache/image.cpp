// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <memory>
#include "common/assert.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

using namespace Vulkan;

Common::IncrementalIdProvider<u64> Image::global_image_uid{};

static vk::ImageUsageFlags ImageUsageFlags(const Vulkan::Instance& instance,
                                           const ImageInfo& info) {
    vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eTransferSrc |
                                vk::ImageUsageFlagBits::eTransferDst |
                                vk::ImageUsageFlagBits::eSampled;
    if (!info.props.is_block) {
        if (info.props.is_depth) {
            usage |= vk::ImageUsageFlagBits::eDepthStencilAttachment;
        } else {
            usage |= vk::ImageUsageFlagBits::eColorAttachment;
            if (instance.IsAttachmentFeedbackLoopLayoutSupported()) {
                usage |= vk::ImageUsageFlagBits::eAttachmentFeedbackLoopEXT;
            }
            // Always create images with storage flag to avoid needing re-creation in case of e.g
            // compute clears This sacrifices a bit of performance but is less work. ExtendedUsage
            // flag is also used. The exception here is for multisample images when storage is not
            // supported, where even with ExtendedUsage we may get only one supported sample back.
            if (info.num_samples == 1 || instance.IsMultisampleStorageImageSupported())
                usage |= vk::ImageUsageFlagBits::eStorage;
        }
    } else {
        // Similarly to above, we specify storage usage. This is typically not supported by
        // compressed formats, but may be used for uncompressed views. In order to satisfy this,
        // we will also specify the extended usage bit.
        usage |= vk::ImageUsageFlagBits::eStorage;
    }

    return usage;
}

static vk::ImageType ConvertImageType(AmdGpu::ImageType type) noexcept {
    switch (type) {
    case AmdGpu::ImageType::Color1D:
    case AmdGpu::ImageType::Color1DArray:
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DMsaa:
    case AmdGpu::ImageType::Color2DArray:
    case AmdGpu::ImageType::Cube:
        return vk::ImageType::e2D;
    case AmdGpu::ImageType::Color3D:
        return vk::ImageType::e3D;
    default:
        UNREACHABLE();
    }
}

static vk::FormatFeatureFlags2 FormatFeatureFlags(const vk::ImageUsageFlags usage_flags) {
    vk::FormatFeatureFlags2 feature_flags{};
    if (usage_flags & vk::ImageUsageFlagBits::eTransferSrc) {
        feature_flags |= vk::FormatFeatureFlagBits2::eTransferSrc;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eTransferDst) {
        feature_flags |= vk::FormatFeatureFlagBits2::eTransferDst;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eSampled) {
        feature_flags |= vk::FormatFeatureFlagBits2::eSampledImage;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eColorAttachment) {
        feature_flags |= vk::FormatFeatureFlagBits2::eColorAttachment;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eDepthStencilAttachment) {
        feature_flags |= vk::FormatFeatureFlagBits2::eDepthStencilAttachment;
    }
    // Note: StorageImage is intentionally ignored for now since it is always set, and can mess up
    // compatibility checks.
    return feature_flags;
}

UniqueImage::~UniqueImage() {
    if (image) {
        vmaDestroyImage(allocator, image, allocation);
    }
}

void UniqueImage::Destroy() {
    if (image) {
        vmaDestroyImage(allocator, image, allocation);
        image = vk::Image{};
        allocation = {};
    }
}

void UniqueImage::Create(const vk::ImageCreateInfo& image_ci) {
    vk::ImageCreateInfo ci = image_ci;
    const bool huge = ci.extent.width > 16384 || ci.extent.height > 16384 ||
                      ci.extent.depth > 2048 || ci.arrayLayers > 256 ||
                      ci.mipLevels > 16;
    if (huge) {
        LOG_WARNING(Render_Vulkan,
                    "GT7: invalid image {}x{}x{} layers={} mips={} -> dummy",
                    ci.extent.width, ci.extent.height, ci.extent.depth, ci.arrayLayers,
                    ci.mipLevels);
        ci.extent = vk::Extent3D{1, 1, 1};
        ci.mipLevels = 1;
        ci.format = vk::Format::eR8G8B8A8Unorm;
    }
    this->image_ci = ci;
    ASSERT(!image);
    VmaAllocationCreateInfo alloc_ci = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };
    const VkImageCreateInfo image_ci_unsafe = static_cast<VkImageCreateInfo>(ci);
    VkImage unsafe_image{};
    VmaAllocationInfo alloc_info{};
    VkResult result = vmaCreateImage(allocator, &image_ci_unsafe, &alloc_ci, &unsafe_image,
                                     &allocation, &alloc_info);
    if (result != VK_SUCCESS && !huge) {
        LOG_WARNING(Render_Vulkan, "Allocating image of size {} bytes over budget.", alloc_info.size);
        alloc_ci.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        result = vmaCreateImage(allocator, &image_ci_unsafe, &alloc_ci, &unsafe_image,
                                &allocation, &alloc_info);
    }
    if (result != VK_SUCCESS) {
        LOG_ERROR(Render_Vulkan, "GT7: image create failed, using 1x1 dummy");
        vk::ImageCreateInfo dummy{};
        dummy.imageType = vk::ImageType::e2D;
        dummy.format = vk::Format::eR8G8B8A8Unorm;
        dummy.extent = vk::Extent3D{1, 1, 1};
        dummy.mipLevels = 1;
        dummy.arrayLayers = 1;
        dummy.samples = vk::SampleCountFlagBits::e1;
        dummy.tiling = vk::ImageTiling::eOptimal;
        dummy.usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc |
                       vk::ImageUsageFlagBits::eTransferDst;
        dummy.sharingMode = vk::SharingMode::eExclusive;
        dummy.initialLayout = vk::ImageLayout::eUndefined;
        this->image_ci = dummy;
        const VkImageCreateInfo dummy_unsafe = static_cast<VkImageCreateInfo>(dummy);
        result = vmaCreateImage(allocator, &dummy_unsafe, &alloc_ci, &unsafe_image, &allocation,
                                &alloc_info);
    }
    ASSERT_MSG(result == VK_SUCCESS, "Failed dummy image");
    image = vk::Image{unsafe_image};
    size_bytes = alloc_info.size;
}

Image::Image(const Vulkan::Instance& instance, Vulkan::Runtime& runtime_,
             Common::SlotVector<ImageView>& slot_image_views_, const ImageInfo& info_)
    : runtime{&runtime_}, slot_image_views{&slot_image_views_}, info{info_} {
    if (info.pixel_format == vk::Format::eUndefined) {
        return;
    }

    image_uid = global_image_uid.Next();
    vk::ImageCreateFlags flags{vk::ImageCreateFlagBits::eMutableFormat |
                               vk::ImageCreateFlagBits::eExtendedUsage};
    if (info.props.is_volume) {
        flags |= vk::ImageCreateFlagBits::e2DArrayCompatible;
        if (instance.Is2dViewOf3dSupported()) {
            flags |= vk::ImageCreateFlagBits::e2DViewCompatibleEXT;
        }
    }
    if (info.props.is_block && instance.IsBlockTexelViewSupported()) {
        flags |= vk::ImageCreateFlagBits::eBlockTexelViewCompatible;
    }

    usage_flags = ImageUsageFlags(instance, info);
    format_features = FormatFeatureFlags(usage_flags);
    if (info.props.is_depth) {
        aspect_mask = vk::ImageAspectFlagBits::eDepth;
        if (info.props.has_stencil) {
            aspect_mask |= vk::ImageAspectFlagBits::eStencil;
        }
    }

    constexpr auto tiling = vk::ImageTiling::eOptimal;
    const auto supported_format = instance.GetSupportedFormat(info.pixel_format, format_features);
    vk::Format used_format = supported_format;
    const auto query_format = [&](vk::ImageCreateFlags query_flags,
                                  vk::ImageUsageFlags query_usage) {
        const vk::PhysicalDeviceImageFormatInfo2 format_info{
            .format = used_format,
            .type = ConvertImageType(info.type),
            .tiling = tiling,
            .usage = query_usage,
            .flags = query_flags,
        };
        return instance.GetPhysicalDevice().getImageFormatProperties2(format_info);
    };

    vk::ImageCreateFlags used_flags = flags;
    vk::ImageUsageFlags used_usage = usage_flags;
    auto image_format_properties = query_format(used_flags, used_usage);
    if (image_format_properties.result == vk::Result::eErrorFormatNotSupported) {
        LOG_ERROR(Render_Vulkan, "image format {} type {} is not supported (flags {}, usage {})",
                  vk::to_string(supported_format), vk::to_string(ConvertImageType(info.type)),
                  vk::to_string(used_flags), vk::to_string(used_usage));
        // GT7 asks for a mutable A2B10G10R10_SNORM image that also wants storage and the
        // attachment-feedback-loop usage. The driver refuses that combination and creating it
        // anyway faults (integer division by zero) inside the driver, so retry without the
        // optional parts and keep whatever the driver does accept.
        used_flags &= ~vk::ImageCreateFlagBits::eExtendedUsage;
        used_usage &= ~(vk::ImageUsageFlagBits::eStorage |
                        vk::ImageUsageFlagBits::eAttachmentFeedbackLoopEXT);
        image_format_properties = query_format(used_flags, used_usage);
        if (image_format_properties.result == vk::Result::eSuccess) {
            LOG_WARNING(Render_Vulkan,
                        "image format {}: created with reduced flags {} usage {}",
                        vk::to_string(supported_format), vk::to_string(used_flags),
                        vk::to_string(used_usage));
        } else {
            // This driver refuses A2B10G10R10_SNORM outright; the same-size UNORM sibling is
            // accepted (the scanout images use it) and eMutableFormat lets the views keep asking
            // for SNORM, so substitute the format instead of handing the driver a combination
            // it answers with an integer division by zero.
            vk::Format substitute = supported_format;
            switch (supported_format) {
            case vk::Format::eA2B10G10R10SnormPack32:
                substitute = vk::Format::eA2B10G10R10UnormPack32;
                break;
            case vk::Format::eB10G11R11UfloatPack32:
                substitute = vk::Format::eR16G16B16A16Sfloat;
                break;
            default:
                break;
            }
            if (substitute != supported_format) {
                used_format = substitute;
                image_format_properties = query_format(used_flags, used_usage);
                if (image_format_properties.result != vk::Result::eSuccess) {
                    used_flags = flags;
                    used_usage = usage_flags;
                    image_format_properties = query_format(used_flags, used_usage);
                }
                LOG_WARNING(Render_Vulkan,
                            "image format {}: substituting {} (flags {} usage {})",
                            vk::to_string(supported_format), vk::to_string(used_format),
                            vk::to_string(used_flags), vk::to_string(used_usage));
            }
        }
    }
    supported_samples = image_format_properties.result == vk::Result::eSuccess
                            ? image_format_properties.value.imageFormatProperties.sampleCounts
                            : vk::SampleCountFlagBits::e1;

    const vk::ImageCreateInfo image_ci = {
        .flags = used_flags,
        .imageType = ConvertImageType(info.type),
        .format = used_format,
        .extent{
            .width = info.size.width,
            .height = info.size.height,
            .depth = info.size.depth,
        },
        .mipLevels = static_cast<u32>(info.resources.levels),
        .arrayLayers = static_cast<u32>(info.resources.layers),
        .samples = LiverpoolToVK::NumSamples(info.num_samples, supported_samples),
        .tiling = tiling,
        .usage = used_usage,
        .initialLayout = vk::ImageLayout::eUndefined,
    };


    LOG_DEBUG(Render_Vulkan,
              "creating image {}x{}x{} {} format {} usage {} flags {} samples {}",
             info.size.width, info.size.height, info.size.depth,
             vk::to_string(ConvertImageType(info.type)), vk::to_string(used_format),
             vk::to_string(used_usage), vk::to_string(used_flags),
             vk::to_string(image_ci.samples));
    backing = &backing_images.emplace_back();
    backing->num_samples = info.num_samples;
    backing->image = UniqueImage{instance.GetDevice(), instance.GetAllocator()};
    backing->image.Create(image_ci);
    LOG_DEBUG(Render_Vulkan, "image created");

    Vulkan::SetObjectName(instance.GetDevice(), GetImage(),
                          "Image {}x{}x{} {} {} {:#x}:{:#x} L:{} M:{} S:{}", info.size.width,
                          info.size.height, info.size.depth, AmdGpu::NameOf(info.tile_mode),
                          vk::to_string(info.pixel_format), info.guest_address, info.guest_size,
                          info.resources.layers, info.resources.levels, info.num_samples);
}

Image::~Image() = default;

ImageView& Image::FindView(const ImageViewInfo& view_info, bool ensure_guest_samples) {
    if (ensure_guest_samples && backing->num_samples > 1 != info.num_samples > 1) {
        runtime->SetBackingSamples(this, info.num_samples);
    }
    const auto& view_infos = backing->image_view_infos;
    const auto it = std::ranges::find(view_infos, view_info);
    if (it != view_infos.end()) {
        const auto view_id = backing->image_view_ids[std::distance(view_infos.begin(), it)];
        return (*slot_image_views)[view_id];
    }
    const auto view_id = slot_image_views->Insert(runtime->GetInstance(), view_info, *this);
    backing->image_view_infos.emplace_back(view_info);
    backing->image_view_ids.emplace_back(view_id);
    return (*slot_image_views)[view_id];
}

void Image::GetBarriers(Barriers& barriers, vk::ImageLayout dst_layout, vk::AccessFlags2 dst_mask,
                        vk::PipelineStageFlags2 dst_stage,
                        std::optional<SubresourceRange> subres_range) {
    auto& last_state = backing->state;
    auto& subresource_states = backing->subresource_states;

    const bool needs_partial_transition =
        subres_range &&
        (subres_range->base != SubresourceBase{} || subres_range->extent != info.resources);
    const bool partially_transited = !subresource_states.empty();

    if (needs_partial_transition || partially_transited) {
        if (!partially_transited) {
            subresource_states.resize(info.resources.levels * info.resources.layers);
            std::fill(subresource_states.begin(), subresource_states.end(), last_state);
        }

        // In case of partial transition, we need to change the specified subresources only.
        // Otherwise all subresources need to be set to the same state so we can use a full
        // resource transition for the next time.
        const u32 start_mip = needs_partial_transition ? subres_range->base.level : u16{0};
        u32 end_mip = needs_partial_transition
                          ? subres_range->base.level + subres_range->extent.levels
                          : info.resources.levels;
        const u32 start_layer = needs_partial_transition ? subres_range->base.layer : u16{0};
        u32 end_layer = needs_partial_transition
                            ? subres_range->base.layer + subres_range->extent.layers
                            : info.resources.layers;
        if (end_mip > info.resources.levels || end_layer > info.resources.layers) {
            // A guest view can reference levels/layers the backing image does not have; clamp
            // instead of asserting so the rest of the frame can still be processed.
            LOG_WARNING(Render_Vulkan,
                        "Transit range exceeds image: mips {}-{} of {}, layers {}-{} of {}",
                        start_mip, end_mip, info.resources.levels, start_layer, end_layer,
                        info.resources.layers);
            end_mip = std::min<u32>(end_mip, info.resources.levels);
            end_layer = std::min<u32>(end_layer, info.resources.layers);
        }
        for (u32 mip = start_mip; mip < end_mip; ++mip) {
            for (u32 layer = start_layer; layer < end_layer; ++layer) {
                // NOTE: these loops may produce a lot of small barriers.
                // If this becomes a problem, we can optimize it by merging adjacent barriers.
                const auto subres_idx = mip * info.resources.layers + layer;
                ASSERT(subres_idx < subresource_states.size());
                auto& state = subresource_states[subres_idx];

                constexpr auto write_flags = vk::AccessFlagBits2::eTransferWrite |
                                             vk::AccessFlagBits2::eShaderWrite |
                                             vk::AccessFlagBits2::eMemoryWrite;
                const bool is_write = static_cast<bool>(state.access_mask & write_flags);
                if (state.layout != dst_layout || state.access_mask != dst_mask || is_write) {
                    barriers.emplace_back(vk::ImageMemoryBarrier2{
                        .srcStageMask = state.pl_stage,
                        .srcAccessMask = state.access_mask,
                        .dstStageMask = dst_stage,
                        .dstAccessMask = dst_mask,
                        .oldLayout = state.layout,
                        .newLayout = dst_layout,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = GetImage(),
                        .subresourceRange{
                            .aspectMask = aspect_mask,
                            .baseMipLevel = mip,
                            .levelCount = 1,
                            .baseArrayLayer = layer,
                            .layerCount = 1,
                        },
                    });
                    state.layout = dst_layout;
                    state.access_mask = dst_mask;
                    state.pl_stage = dst_stage;
                }
            }
        }

        if (!needs_partial_transition) {
            subresource_states.clear();
        }
    } else { // Full resource transition
        constexpr auto write_flags = vk::AccessFlagBits2::eTransferWrite |
                                     vk::AccessFlagBits2::eShaderWrite |
                                     vk::AccessFlagBits2::eMemoryWrite;
        const bool is_write = static_cast<bool>(last_state.access_mask & write_flags);
        if (last_state.layout == dst_layout && last_state.access_mask == dst_mask && !is_write) {
            return;
        }
        barriers.emplace_back(vk::ImageMemoryBarrier2{
            .srcStageMask = last_state.pl_stage,
            .srcAccessMask = last_state.access_mask,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_mask,
            .oldLayout = last_state.layout,
            .newLayout = dst_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = GetImage(),
            .subresourceRange{
                .aspectMask = aspect_mask,
                .baseMipLevel = 0,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        });
    }

    last_state.layout = dst_layout;
    last_state.access_mask = dst_mask;
    last_state.pl_stage = dst_stage;
}

} // namespace VideoCore