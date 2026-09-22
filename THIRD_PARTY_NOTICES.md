# Third-party notices

## Bundled resource pack (Minecraft assets)

`ressources/default-resource-pack.zip` and the fallback textures under
`ressources/textures/` are unmodified assets taken from Mojang's Minecraft
client (`assets/minecraft/...`). They are **not** covered by this project's
Apache License 2.0: they remain the property of Mojang Studios / Microsoft
and are subject to the
[Minecraft Usage Guidelines](https://www.minecraft.net/usage-guidelines).
They are bundled only as the development-time fallback so a fresh checkout
renders out of the box. Those guidelines do not authorize redistributing
vanilla assets inside other projects — point `FT_VOX_RESOURCE_PACK` (or the
`--resource-pack` CLI flag) at a pack you are allowed to use instead.

## NVIDIA FXAA 3.11

The spatial anti-aliasing pass (`ressources/shaders/vulkan/fxaa.frag.glsl`,
compiled to `fxaa.frag.spv`) is ported from NVIDIA's `FXAA3_11.h` reference
(`Fxaa3_11.h`, FXAA 3.11 by Timothy Lottes, NVIDIA Corporation; mirrored at
https://github.com/GameTechDev/CMAA2/blob/master/Projects/CMAA2/FXAA/Fxaa3_11.h).
The same notice is preserved in the shader source; it is reproduced here so
binary distributions that ship only the compiled SPIR-V remain compliant.

```text
// File:        FXAA\src/FXAA3_11.h
// SDK Version: v1.2
// Email:       gameworks@nvidia.com
// Site:        http://developer.nvidia.com/
//
// Copyright (c) 2014, NVIDIA CORPORATION. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## Dear ImGui (vendored under `src/imgui/`)

Dear ImGui (v1.91.x) by Omar Cornut — MIT License. The upstream license text
ships in `src/imgui/LICENSE.txt`.

## stb_image (vendored under `src/stb_image/stb_image.h`)

stb_image v2.28 by Sean Barrett — dual-licensed to the public domain or under
the MIT license; see the license block embedded at the top of the header.

## ImGuiFileDialog (vendored under `src/ImGuiFileDialog/`)

ImGuiFileDialog by Stéphane Cuillerdier (aiekick) — MIT License, see
`src/ImGuiFileDialog/LICENSE.txt`.
