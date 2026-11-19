# Static TLS in one authored Win32 DLL

The reference runs a 32-bit LLVM DLL under Windows. The translation runs the same DLL's compiler-generated accessors, callbacks and entry point against synthetic 32-bit thread records. All build products, translated code and traces go in the output directories. The Windows reference loads only the sibling image whose size and SHA-256 the script checks.

## Reproduce

Run on Windows with LLVM 19.1.1, a configured Microsoft C toolchain and Windows SDK 10.0.22621.0. The output directory must be new.

```powershell
py -3.12 compare.py windows C:/temp/owned-tls-windows
```

Run the AOT comparison under Linux with the project's pinned Python packages, a local checkout containing the pinned Skydream commit and a C compiler.

```sh
python compare.py aot /tmp/owned-tls-aot --reference /mnt/c/temp/owned-tls-windows \
  --skydream /path/to/skydream --cc gcc --sanitize
```

## Verified behavior

- Windows 10 22H2 assigned a module slot through `AddressOfIndex`. The DLL's instructions load the synthetic-equivalent pointer from `FS:[0x2c]`, index the pointer array using four times the module slot, and read or write the selected thread's values at byte offsets 4 and 8.
- On explicit load, two successive new worker threads, and explicit unload, the Windows loader invoked callback A, callback B and then `DllMain` in that order for each of the six notifications. This was measured on this OS and loading route. The three execution contexts started at `0x13579bdf, 0`, retained independent writes, and presented their final values during detachment.
- Each of two image bases and each of two synthetic slot numbers reproduced all 22 Windows rows. GCC ASan and UBSan cover the translated caller and the harness. Windows LLVM also produced four arm64 iOS 18 Mach-O objects from the two translations and harnesses. Apple SDK linking and iPhone execution remain separate checks.
- The initialized template occupies 12 bytes. The directory declares a further 12 zero-fill bytes and 4-byte alignment. The synthetic storage explicitly zeros that tail. Native comparison reads the two C fields inside the initialized template, including its zero word.
- The standalone Windows image retains one unused KERNEL32 `MulDiv` import. With the import directory removed, the Windows reference on this machine left the authored module-index sentinel untouched and omitted the TLS callbacks. The comparison requires assignment, initial values and all three process-attach records before allowing any writes.

The harness uses private byte arrays, compile-time callback targets, two synthetic thread slots and bounded trace storage. The translated run never calls any imported Windows function. The experiment covers explicit load/unload and threads created after load. Static TLS for multiple simultaneously loaded modules, pre-existing threads, dynamic `TlsAlloc` APIs and real MOD DLLs require their own verification.

The pinned guest runtime assigns static TLS slot zero to the main EXE in `rt_set_static_tls` and when building a thread's TEB. Extending this to additional DLLs requires separate module indices, per-thread blocks and lifecycle dispatch. Microsoft documents the x86 TLS array/index, callback table and loader notifications in the [PE format](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format#the-tls-section) and [DllMain](https://learn.microsoft.com/en-us/windows/win32/dlls/dllmain) references.
