// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>
#include "common/types.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Backend::SPIRV {

[[nodiscard]] std::vector<u32> EmitDiscardFragmentShader(std::array<Shader::OutputMap, 3> outputs);

/// Returns an empty fragment shader. Used when the guest disables the pixel shader stage but
/// rasterization (and therefore a fragment stage in the pipeline) is still required.
[[nodiscard]] std::vector<u32> EmitNullFragmentShader();

} // namespace Shader::Backend::SPIRV
