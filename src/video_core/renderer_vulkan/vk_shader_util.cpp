// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>

#include <fmt/format.h>

#include <xxhash.h>

#include "common/assert.h"
#include "common/path_util.h"
#include "common/io_file.h"
#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

static u64 DumpSpirvIfRequested(std::span<const u32> code) {
    const u64 code_hash = XXH3_64bits(code.data(), code.size() * sizeof(u32));
    static const bool dump_enabled = std::getenv("SHADPS4_DUMP_SPV") != nullptr;
    if (dump_enabled) {
        const auto dir = Common::FS::GetUserPath(Common::FS::PathType::ShaderDir) / "spv";
        std::filesystem::create_directories(dir);
        const auto path = dir / fmt::format("{:#x}.spv", code_hash);
        Common::FS::IOFile file{path, Common::FS::FileAccessMode::Create,
                                Common::FS::FileType::BinaryFile};
        if (file.IsOpen()) {
            file.WriteRaw<u32>(code.data(), code.size());
        }
    }
    LOG_INFO(Render_Vulkan, "SPIR-V code_hash={:#x} words={}", code_hash, code.size());
    return code_hash;
}

vk::ShaderModule CompileSPV(std::span<const u32> code, vk::Device device) {
    const u64 dumped_hash = DumpSpirvIfRequested(code);
    const vk::ShaderModuleCreateInfo shader_info = {
        .codeSize = code.size() * sizeof(u32),
        .pCode = code.data(),
    };
    auto [module_result, module] = device.createShaderModule(shader_info);
    if (std::getenv("SHADPS4_DUMP_SPV") != nullptr) {
        LOG_INFO(Render_Vulkan, "SPV module hash={:#x} bytes={} module={:#x}", dumped_hash,
                 code.size() * 4, reinterpret_cast<u64>(static_cast<VkShaderModule>(module)));
    }
    ASSERT_MSG(module_result == vk::Result::eSuccess, "Failed to compile SPIR-V shader: {}",
               vk::to_string(module_result));
    return module;
}

} // namespace Vulkan
