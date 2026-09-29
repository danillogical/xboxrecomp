# Kernel DATA export regression

No game files are required. The test imports all 34 DATA exports of the retail
kernel (the ordinals nxdk's `lib/xboxkrnl/xboxkrnl.exe.def` marks `DATA`; the
same 34 that Cxbx-Reloaded's `KernelThunk.cpp` wraps in `VARIABLE()`) plus one
function, runs `xbox_kernel_bridge_init()`, and checks that every data slot
holds an address inside the kernel data page, that no two exports' storage
overlaps, and that the function slot still holds a synthetic dispatch VA.

```
cmake -S tests/kernel_data_exports -B build/kernel-data-exports -A x64
cmake --build build/kernel-data-exports --config Release --target kernel-data-exports-test
ctest --test-dir build/kernel-data-exports -C Release --output-on-failure
```

Before the fix, ordinals 88, 89, 102, 120, 154, 162, 240, 245, 249 and 321
were routed to function bridges, so their slots held `0xFE000000 + 4*slot`
and a title dereferencing one read unmapped memory; this test reports each of
them as "not backed by the kernel data page".

Sizes are the kernel's layouts from nxdk `lib/xboxkrnl/xboxkrnl.h`, except the
older object-type exports, which reserve 4 bytes (only their identity is
modelled). MmGlobalData, KiBugCheckData, ObpObjectHandleTable and XboxEEPROMKey
are zeroed: their real contents are not modelled. KeInterruptTime and
KeSystemTime are set once from the host clocks and not advanced.

It is a separate project, like `tests/idex_channel`, because linking the
bridge pulls in the whole runtime, D3D8 and video layers included.
