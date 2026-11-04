# a32bridge

Run a 32-bit ARM (armeabi / armeabi-v7a) Android game library inside a **64-bit-only** app, so old native games
install and run on arm64-only devices and current Android versions.

The game's native library is not recompiled. A small arm64 library takes its place, loads the original ARM library
into a private 4 GiB address space and executes it with the [Dynarmic](https://github.com/merryhime/dynarmic) JIT.
Everything the game imports (libc, libm, zlib, OpenGL ES, OpenSL ES, JNI) is re-implemented on the host side.

```
   Java (ART, arm64)
        |  System.loadLibrary("Game")            lib/arm64-v8a/libGame.so        <- a32bridge
        v                                          lib/arm64-v8a/libGame_a32.so   <- the original ARM library
 +--------------------+     svc stubs      +----------------------------+
 | guest ARM code     | -----------------> | host HLE: libc, GLES, SLES, |
 | (Dynarmic JIT)     | <----------------- | JNI facade, threads, zlib   |
 +--------------------+     guest_call     +----------------------------+
```

## Status

Built for and tested with several Plants vs. Zombies 2 builds (1.4 and 5.1) and Chinese Version (1.1.0, 1.3.4 and 1.6.3); ARMv5 to ARMv7, GLES 1.1 and 2.0, OpenSL ES
audio, Java SDK integrations. It is not a general Android emulator: it implements exactly the imports those games
use, and a different game will likely need a few more stubs. Missing imports are logged (see *Debugging*) and return 0.

Limitations worth knowing:

* Only the game's main native library runs. Other 32-bit libraries shipped in the APK (ad/analytics/payment SDKs,
  packers) cannot be loaded into a 64-bit process; their `System.loadLibrary` calls are redirected and their native
  methods are unavailable.
* C++ exceptions that need unwind tables from libraries other than the game's own are not supported.
* Direct `ByteBuffer`s allocated on the Java side live outside the guest address space and cannot be handed to
  guest code (`GetDirectBufferAddress` returns NULL for them).
* No debugger support; guest faults are logged with registers and stop the faulting call.

## How it works

| Path | Role |
| --- | --- |
| `src/runtime/cpu.*`, `jit.h`, `thread.*` | One Dynarmic JIT per guest thread (and nesting depth), the CP15 coprocessor, `svc` dispatch, `guest_call`, per-thread stacks and TLS |
| `src/runtime/guest_memory.*` | The 4 GiB reservation, a permanent data arena, the guest heap |
| `src/runtime/stubs.*` | Import stubs (`svc #n; bx lr`), exported-symbol table, Linux *kuser helpers* for ARMv5 code |
| `src/runtime/elf_loader.*`, `elf_format.h` | Maps the ARM ELF, applies relocations against the stubs, collects constructors; the ELF structures and constants |
| `src/libc/` | bionic libc/libm/zlib/GLES imports, one `*_imports` module per area. Most imports are forwarded through a signature string (`forward.*`); anything whose layout differs between 32- and 64-bit is written by hand. `format.*` is the printf/scanf engine and `guest_abi.h` holds the 32-bit bionic layouts (`FILE`, `stat`, `pthread_attr_t`, `open` flags) |
| `src/jni/` | 32-bit `JNIEnv`/`JavaVM` facade: `handles` (object/method/field handles), `calls` (`Call*Method`, fields), `objects`, `strings`, `arrays`, `natives` (`RegisterNatives` trampolines), `vm`, and `bridge` (table construction and `JNI_OnLoad`) |
| `src/opensles.*` | OpenSL ES interfaces as guest vtables over the host objects |
| `src/jni/game_exports.cpp` | JNI exports the VM resolves by name rather than via `RegisterNatives` (game specific) |

A guest address is converted to a host pointer by adding one base (`G + addr`), so memory access inside the JIT is
a plain load/store and host code can read guest buffers directly.

Behaviours that exist because the games assume an old Dalvik VM, and that are deliberate:

* Java exceptions are logged and cleared after each call (the games never check them).
* JNI calls with a null class/object/method id are skipped instead of reaching ART, which aborts the process.
* Local references are recycled slowly so stale handles held by the game do not alias new objects.
* `Signature.hashCode()` can be answered with a fixed value (`-DORIG_SIG_HASH=...`) for games that check their own
  signing certificate and have been re-signed.

## Building

Requirements: Android NDK (r26+), CMake 3.22+, Ninja, git.

```sh
git clone <this repository> && cd a32bridge
git submodule update --init third_party/dynarmic
tools/init_boost.sh                                    # only the ~20 Boost libraries Dynarmic needs
cmake -B build -G Ninja \n  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \n  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 -DCMAKE_BUILD_TYPE=Release \n  -DLIBNAME=Game
cmake --build build          # -> build/libGame.so
```

Third-party code:

* **Dynarmic** is a git submodule (`third_party/dynarmic`, the [touchHLE fork](https://github.com/touchHLE/dynarmic)
  at the pinned commit). One small change is needed for Android (no fastmem signal handler: it would replace ART's
  SIGSEGV handling); `patches/dynarmic-android.patch` is applied to the checkout automatically at configure time.
* **Boost** (1.85) is a git submodule too (`third_party/boost`), used only for headers. The superproject has ~170 library
  submodules, so `tools/init_boost.sh` checks out the tag and initializes just the ~20 libraries Dynarmic includes
  (shallow). Without the submodule, `-DA32_BOOST_DIR=<dir containing boost/>` points at an existing copy, and if neither
  exists CMake downloads the release archive.

Options:

| Option | Meaning |
| --- | --- |
| `LIBNAME` | Name of the game library to stand in for (`libGame.so`). Must match `System.loadLibrary("Game")` |
| `ORIG_SIG_HASH` | Value returned for `Signature.hashCode()` (see above). Default `500290098` (PopCap/EA); empty disables |
| `A32_LOG` | Log translator messages and the game's own log/stdio output to logcat (tag `a32`) |
| `A32_TRACE` | Additionally log every stub and JNI call (very verbose) |
| `A32_DYNARMIC_DIR`, `A32_BOOST_DIR` | Locations of the third-party sources (defaults: `third_party/...`) |

## Using it on an APK

1. Decode the APK: `apktool d game.apk -o decoded`.
2. Build the bridge with `-DLIBNAME=<name>`, where `<name>` is the argument the game passes to `System.loadLibrary`.
3. Prepare the app:
   ```sh
   python tools/port_apk.py decoded build/lib<name>.so ported
   ```
   This installs `lib<name>.so` (the bridge) and `lib<name>_a32.so` (the original) under `lib/arm64-v8a`, **keeps the
   original 32-bit libraries** so the same APK also runs natively on 32-bit ARM devices (`--arm64-only` drops them),
   redirects the other libraries' `System.loadLibrary` calls, and adjusts SDK levels and the manifest. Signature
   checks are answered by the bridge itself (`ORIG_SIG_HASH`, which defaults to the PopCap/EA certificate hash
   `500290098` shared by every PvZ2 build), so the game library stays unmodified. The script reads the real hash from the
   APK's original signature and warns if it differs; then rebuild the bridge with `-DORIG_SIG_HASH=<value>`. Read its
   output: it lists implicit service intents that Android 5+ rejects; pin the ones that belong to the app with
   `--pin-intents`.
4. `apktool b ported -o game.apk`, then `zipalign` and sign it.

## Debugging

Build with `-DA32_LOG=ON` and read `adb logcat -s a32`. Typical messages:

* `UNIMPLEMENTED import called: <name>`: the game needs an import that has no stub yet. Add it to the matching
  `src/libc/*_imports.cpp` (a signature-string `gen(...)` line is often enough).
* `BAD MEM ...`: the guest touched unmapped memory; the line shows pc/lr/sp and registers.
* `JIT cannot translate ...`: an instruction Dynarmic does not support.
* `JNI call ... skipped`: the game used a class or method that does not exist in the repackaged APK.

## License

[WTFPL](LICENSE): do what you want. Third-party components keep their own licenses: Dynarmic is 0BSD, Boost is under
the Boost Software License.
