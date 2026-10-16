# xbox_video: Vulkan build plumbing

The Vulkan pieces are optional. `cmake/XbrVulkan.cmake` runs `find_package(Vulkan)` and looks
for `dxc` and `spirv-val` (hinted from `$VULKAN_SDK/Bin`), and it requires the loader, its
headers and both tools to EXIST: a cached path that no longer exists is not a toolchain. If any
is missing, a status line says which check failed and everything Vulkan is skipped; the rest
builds as before. `XBR_VULKAN_FOUND` is the ONE switch: a cache BOOL, FORCE-refreshed on every
configure, so no decision from an earlier configure survives, and it is visible to a parent
project that adds this toolkit with `add_subdirectory` (a plain variable would not reach it).
When it is FALSE, the cache STRING `XBR_VULKAN_UNAVAILABLE_REASON` names the failed check, so a
caller can report the reason instead of re-deriving it.

## Ahead-of-time shaders

No shader is compiled at run time. `xbr_add_spirv` turns an HLSL entry point into a header at
build time:

```cmake
xbr_add_spirv(my_target SOURCE shaders/foo_vs.hlsl ENTRY main PROFILE vs_6_0 INVERT_Y
              SYMBOL foo_vs [DEFINES FOO=1] [TARGET_ENV vulkan1.1] [DEPENDS extra.hlsli])
```

It runs `dxc -spirv -T <profile> -E <entry>` (plus `-fvk-invert-y` with `INVERT_Y`, vertex/domain/geometry
profiles only), validates with `spirv-val`, and writes `<build>/spirv/<SYMBOL>.spv.h` containing
`static const uint32_t <SYMBOL>[]` and `static const size_t <SYMBOL>_size` (bytes). The header is
added to the target's sources and its directory to its include path. A compile or validation error
fails that target's build.

`nv2a_vk_stage0.hlsl` (the stage-0 path) and `nv2a_vk_rc.hlsl` (the NV2A register combiners) are each
built twice, once per entry point; the combiner shader is one uber shader for every combiner program,
so nothing is specialised or compiled per configuration. `shaders/smoke_tri_{vs,ps}.hlsl` are the
proof pair for the smoke test.

## Vertex programs: the corpus and the tool

The register-combiner uber shader is compiled once. Vertex programs are not: a title's
static programs are translated and compiled ahead of time by the game's own build, and the
Vulkan back end consumes that table (`nv2a_vsh_aot.h`) at run time. It never translates or
compiles a guest program — a program the corpus does not carry is declined, and the
executor's CPU interpreter draws the batch.

`nv2a_vsh_hlsl` is the host tool that produces one shader for that pipeline:

```
nv2a_vsh_hlsl --bin <program.bin> --mask <hex> [--vk] [--out <file>]
```

`--bin` is `len*16` little-endian bytes, one NV2A instruction per 16 bytes, the last FINAL;
`--mask` is the program's input mask. Without `--vk` the output is `nv2a_vsh_hlsl.c`'s bytes
unchanged. With `--vk` it also carries what the pipeline needs and dxc cannot infer: an
explicit `[[vk::location(n)]]` on every vertex input (dxc otherwise packs ATTR1 and ATTR3 to
locations 0 and 1, which is not the layout the back end builds from the mask), a location on
every varying so the shader agrees with `ps_rc`'s inputs, and bindings 9 and 10 for the two
cbuffers. Each rewrite must find its pattern exactly once, so a change to the translator
fails the tool rather than emitting a mis-bound shader. The exit codes are 0 success, 2 usage
or I/O error, 3 translation or rewrite failure.

## Software Vulkan driver for CTest

`tests/vk_smoke` draws a triangle and checks the pixels. `vk_smoke_hw` uses the default devices
(skipped if only a CPU device exists). `vk_smoke_sw` forces a software ICD through
`VK_DRIVER_FILES` / `VK_ICD_FILENAMES`, set from the cache variable `XBR_VK_SW_ICD`
(the ICD manifest, e.g. Mesa lavapipe's `lvp_icd.x86_64.json`). It defaults to the environment
variable of the same name, else empty; when empty `vk_smoke_sw` reports as skipped.

The Vulkan loader ignores `VK_DRIVER_FILES` in an elevated (High integrity) process, so from an
administrator shell, including an ssh session as an administrator, `vk_smoke_sw` finds no CPU
device and skips with a message saying so. Run CTest non-elevated for it to execute.

Enable it once per build tree:

```
cmake -S . -B build -DXBR_VK_SW_ICD=C:/Users/logic/tools/vk-sw/x64/lvp_icd.x86_64.json
```

The value stays in the cache. Install lavapipe from the `mesa3d-<ver>-release-msvc.7z` asset of
https://github.com/pal1000/mesa-dist-win/releases (verify the asset's sha256 digest, extract with
`tar -xf`), outside the repositories.
