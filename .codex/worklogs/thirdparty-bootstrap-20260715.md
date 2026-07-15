# Vulkan third-party bootstrap progress

Date: 2026-07-15

Constraint: `G:\Teaching\Vulkan\thirdparty\CMakeLists.txt` is read-only and must not be modified.

## Status legend

- `[ ]` pending
- `[-]` in progress
- `[x]` restored and layout-checked
- `[!]` blocked or requires follow-up

## Local framework copies

- [x] `icetea` from `P:\InfraUtilities\icetea`
- [x] `tapioca` from `P:\InfraUtilities\tapioca`

## Git dependencies

- [x] `vulkan-headers`: KhronosGroup/Vulkan-Headers, tag `vulkan-sdk-1.4.350.0`
- [x] `vma`: GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator
- [x] `volk`: zeux/volk
- [x] `vk-bootstrap`: charles-lunarg/vk-bootstrap
- [x] `spirv-cross`: KhronosGroup/SPIRV-Cross
- [x] `spirv-reflect`: KhronosGroup/SPIRV-Reflect
- [x] `ktx`: KhronosGroup/KTX-Software, tag `v5.0.0-rc1`, recursive
- [x] `glfw`: glfw/glfw
- [x] `imgui`: ocornut/imgui, branch `docking`
- [x] `glm`: g-truc/glm
- [x] `stb`: nothings/stb
- [x] `assimp`: assimp/assimp
- [x] `tinyobjloader`: tinyobjloader/tinyobjloader
- [x] `tinygltf`: syoyo/tinygltf
- [x] `cgltf`: jkuhlmann/cgltf
- [x] `fastgltf`: spnda/fastgltf
- [x] `meshoptimizer`: zeux/meshoptimizer
- [x] `mikktspace`: mmikk/MikkTSpace
- [x] `tinyexr`: syoyo/tinyexr, recursive
- [x] `tracy`: wolfpld/tracy
- [x] `spdlog`: gabime/spdlog
- [x] `nlohmann_json`: nlohmann/json
- [x] `entt`: skypjack/entt
- [x] `tinyfiledialogs`: native-toolkit/libtinyfiledialogs
- [x] `cli11`: CLIUtils/CLI11
- [x] `stdexec`: NVIDIA/stdexec
- [x] `taskflow`: taskflow/taskflow
- [x] `miniaudio`: mackron/miniaudio
- [x] `imguizmo/src`: CedricGuillemet/ImGuizmo
- [x] `implot`: epezent/implot
- [x] `catch2`: CatchOrg/Catch2
- [x] `eigen`: libeigen/eigen
- [x] `efsw`: SpartanJ/efsw
- [x] `msdfgen`: Chlumsky/msdfgen
- [x] `jolt`: jrouwe/JoltPhysics
- [x] `physx`: NVIDIA-Omniverse/PhysX
- [x] `tbb`: oneapi-src/oneTBB, tag `v2023.1.0`; patched upstream Windows/libc++ feature detection and missing
  `<process.h>` include, standalone `tbb` target verified under x64-asan
- [x] `gklib`: KarypisLab/GKlib
- [x] `metis`: KarypisLab/METIS

## Generated or packaged dependencies

- [x] `glad`: glad 2.0.8, generated `gl:core=4.6` loader with all extensions and CMake target `glad`
- [x] `slang`: v2026.13.1 Windows x86_64 SDK with required include/lib/bin layout
- [x] `perfetto`: v57.2 official `perfetto-cpp-sdk-src` release layout
- [x] `renderdoc_app`: RenderDoc v1.45 official `renderdoc_app.h` API header

## Validation milestones

- [x] Every CMake-referenced directory and required sentinel file exists.
- [x] `x64-asan` CMake regeneration succeeds without missing dependency/layout errors.
- [x] Sora library and `Test.Sora.Core.DiagnosticsTest` build; 11 assertions in 3 cases pass under x64-asan.
- [x] CMake File API identifies 45 compilable third-party library targets; one aggregate Ninja invocation builds all 45
  successfully under `x64-asan`.
- [x] All 67 artifacts declared by those targets exist and are non-empty.
- [x] All 50 representative public-header and imported-SDK sentinel files exist and are non-empty.
- [x] `thirdparty/CMakeLists.txt` remains byte-for-byte unchanged relative to the worktree baseline.
- [-] Full project build is outside the final requested scope; only third-party completeness is required.

## Final aggregate target set

`assimp`, `astcenc-avx2-static`, `basisu_encoder`, `Catch2`, `Catch2WithMain`, `dfdutils`, `efsw`, `fastgltf`,
`gklib`, `glad`, `glfw`, `icetea`, `imgui`, `imguizmo`, `implot`, `Jolt`, `ktx`, `ktx_read`, `meshoptimizer`,
`metis`, `mikktspace`, `miniaudio`, `msdfgen-core`, `obj_basisu_cbind`, `objUtil`, `perfetto`, `spdlog`,
`spirv-cross-c`, `spirv-cross-core`, `spirv-cross-cpp`, `spirv-cross-glsl`, `spirv-cross-hlsl`, `spirv-cross-msl`,
`spirv-cross-reflect`, `spirv-cross-util`, `spirv-reflect`, `tapioca`, `tbb`, `tbbmalloc`, `tinyexr`,
`tinyfiledialogs`, `tinyobjloader`, `vk-bootstrap`, `volk`, `zlibstatic`.

Header-only targets and imported SDKs were validated separately because they intentionally have no compiler artifact.
`VK_TP_BUILD_TRACY=OFF` is the declared default and exposes the intended `TracyClient` interface shim; Slang is an imported
prebuilt SDK, while PhysX is intentionally exposed as headers only.
