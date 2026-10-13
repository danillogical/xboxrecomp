# xbox_video: Vulkan build plumbing

The Vulkan pieces are optional. `cmake/XbrVulkan.cmake` runs `find_package(Vulkan)` and looks
for `dxc` and `spirv-val` (hinted from `$VULKAN_SDK/Bin`). If any is missing, a status line
says so and everything Vulkan is skipped; the rest builds as before. `XBR_VULKAN_FOUND` is the
switch.

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
fails that target's build. `shaders/smoke_tri_{vs,ps}.hlsl` are the proof pair.

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
