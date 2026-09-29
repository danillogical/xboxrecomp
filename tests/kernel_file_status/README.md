# NtCreateFile collision status regression

No game files are required. The test creates a file in a temporary game
directory, then asks `xbox_NtCreateFile` to create it again with `FILE_CREATE`.
That maps to `CREATE_NEW`, which fails with `ERROR_FILE_EXISTS`; the kernel
must answer `STATUS_OBJECT_NAME_COLLISION`, not `STATUS_UNSUCCESSFUL`. A
`FILE_OPEN` of an absent name is the control and must stay
`STATUS_OBJECT_NAME_NOT_FOUND`.

```
cmake -S tests/kernel_file_status -B build/kernel-file-status -A x64
cmake --build build/kernel-file-status --config Release --target kernel-file-status-test
ctest --test-dir build/kernel-file-status -C Release --output-on-failure
```
