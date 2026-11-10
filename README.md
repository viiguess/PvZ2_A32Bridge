# a32bridge

Run a 32-bit ARM (armeabi / armeabi-v7a) Android game library inside a **64-bit-only** app, so old native games
install and run on arm64-only devices, current Android versions and x86_64 emulators (MuMu, LDPlayer, WSA).

The game's native library is not recompiled. A small arm64 or x86_64 library takes its place, loads the original ARM
library into a private 4 GiB address space and executes it with the [Dynarmic](https://github.com/merryhime/dynarmic)
JIT.
Everything the game imports (libc, libm, zlib, OpenGL ES, OpenSL ES, JNI) is re-implemented on the host side.

```
   Java (ART, arm64)
        |  System.loadLibrary("Game")            lib/arm64-v8a/libGame.so        <- a32bridge
        v                                          lib/arm64-v8a/libGame_a32.so   <- the original ARM library
        |                                          (the same pair under lib/x86_64 for x86 emulators)
 +--------------------+     svc stubs      +----------------------------+
 | guest ARM code     | -----------------> | host HLE: libc, GLES, SLES, |
 | (Dynarmic JIT)     | <----------------- | JNI facade, threads, zlib   |
 +--------------------+     guest_call     +----------------------------+
```

## Scope

It is not a general Android emulator: it implements the imports the supported games use (ARMv5 to ARMv7, GLES 1.1 and 2.0,
OpenSL ES, Java SDK integrations), and a different game will likely need a few more stubs. Missing imports are logged (see
*Debugging*) and return 0.

Limitations:

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
| `src/runtime/softfloat_accel.*` | armeabi games do all float math through libgcc helpers (`__aeabi_fadd`, ...) that emulate it with integer code. They are replaced by VFP versions (`tools/softfloat_vfp.S`, built into `softfloat_vfp.h` by `tools/gen_softfloat_vfp.py`) that the JIT turns into single host FP instructions: about 2.5x the frame rate on busy screens |
| `src/runtime/mods.*` | Hook libraries the game loads next to its main library (`libczProject.so` and co.) run in the guest too (`--mod` in `port_apk.py`) |
| `src/libc/compat_imports.cpp` | Imports only old games take from the system (libgcc division helpers, old libstdc++ `new`/`delete`) |

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

For x86 emulators, build it a second time with `-DANDROID_ABI=x86_64` (e.g. into `build_x86_64`). The emulator then
runs the bridge natively instead of putting the arm64 bridge through its ARM translator, which is several times slower.

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
| `A32_FREE_SHOP` | Runtime patch for the Lenovo PvZ2C 32-bit builds 1.0.1 - 4.2.4 (ARM and Thumb-2, every optimisation level): `PurchaseBroker::onPurchaseConfirm` reports the order as paid instead of calling the payment SDK, and the dialogs that wait for the SDK are skipped (`src/runtime/free_shop.h`). Sites are found by instruction shape and string references, and nothing is patched unless every site is found; the log (`A32_LOG`) says why a build was skipped. Off by default |
| `A32_LOG` | Log translator messages and the game's own log/stdio output to logcat (tag `a32`) |
| `A32_TRACE` | Additionally log every stub and JNI call (very verbose) |
| `A32_DYNARMIC_DIR`, `A32_BOOST_DIR` | Locations of the third-party sources (defaults: `third_party/...`) |

## Using it on an APK

1. Decode the APK: `apktool d game.apk -o decoded`.
2. Build the bridge with `-DLIBNAME=<name>`, where `<name>` is the argument the game passes to `System.loadLibrary`.
3. Prepare the app:
   ```sh
   python tools/port_apk.py decoded build/lib<name>.so ported --bridge build_x86_64/lib<name>.so
   ```
   This installs `lib<name>.so` (the bridge) and `lib<name>_a32.so` (the original) under `lib/arm64-v8a`, and under
   `lib/x86_64` for the optional `--bridge` build (the ABI comes from the ELF header). Pass `--stub` once per ABI (an
   empty library built from `tools/mod_stub.c`): it fills in the other 32-bit library names, without which x86
   emulators pick the ARM libraries. It **keeps the
   original 32-bit libraries** so the same APK also runs natively on 32-bit ARM devices (`--arm64-only` drops them),
   redirects the other libraries' `System.loadLibrary` calls, and adjusts SDK levels and the manifest. Signature
   checks are answered by the bridge itself (`ORIG_SIG_HASH`, which defaults to the PopCap/EA certificate hash
   `500290098` shared by every PvZ2 build), so the game library stays unmodified. The script reads the real hash from the
   APK's original signature and warns if it differs; then rebuild the bridge with `-DORIG_SIG_HASH=<value>`. Read its
   output: it lists implicit service intents that Android 5+ rejects; pin the ones that belong to the app with
   `--pin-intents`.
4. `apktool b -api 30 ported -o game.apk`, then `zipalign` and sign it. Without `-api 30` the dex targets the
   manifest's `minSdkVersion` and AndroidX/Kotlin code fails with `IncompatibleClassChangeError`.

`port_apk.py` also prepares the SDKs inside the APK for a 64-bit process and modern Android: native methods of SDK
classes (payment, analytics, ...) are stubbed to return 0/null/void instead of throwing `UnsatisfiedLinkError`, and the
`TelephonyManager` identifier calls that throw `SecurityException` on Android 10+ (`getDeviceId`, `getSubscriberId`, ...)
are guarded so they return null. Classes under `com/popcap` keep their natives.

## Packed games (360 Jiagu)

A packed game hides its dex inside `classes.dex` and replaces some `Activity.onCreate` methods with natives that a
packer library (`libjiagu.so`, 32-bit) binds at run time, so the stub cannot run in a 64-bit process either. The bridge
runs the packer's library the same way it runs the game's:

1. Build a second bridge with `-DLIBNAME=jiagu`. The stub copies `assets/libjiagu_a64.so` out of the APK on 64-bit
   devices and loads it, so this bridge takes that place. The packer's own library becomes the guest,
   `lib/arm64-v8a/libjiagu_a32.so`, and the bridge finds it through the app's native library directory. An x86_64
   jiagu bridge goes to `assets/libjiagu_x64.so`.
2. Run `tools/jiagu_run` on the original library (`assets/libjiagu.so` in the APK, under that exact file name) to get the
   decrypted dex files (`classes_unpacked.dex`, and `classes_unpacked2.dex`, ... when the packer hands over more than
   one). The bridge cannot hand a dex to ART the way the packer does on an old Android, so the classes are added to the
   app as extra dex files.
3. Port with `port_apk.py ... --jiagu-bridge libjiagu.so --jiagu-dex classes_unpacked.dex --jiagu-dex classes_unpacked2.dex
   --baksmali baksmali.jar` (one `--jiagu-dex` per file).
   The game's bridge (`-DLIBNAME=<game library>`) is the positional argument as usual.
4. Rebuild with apktool, then `port_apk.py finish original.apk rebuilt.apk output.apk`, then zipalign and sign. This
   step matters: apktool rebuilds `classes.dex` from smali and drops the packer's payload and the ZIP comment it
   reads, and the packer refuses to start without them. The packer's stub is never edited.

Jiagu 2.4.x (Lenovo PvZ2C 2.4.x and later) additionally needs:

* `--unhollow` when porting: the game's activities have their `onCreate` hollowed into natives whose real code lives in
  the packer's payload and cannot run here, so they get minimal bodies; the packer's support classes (`com.jg.*`, ...) are
  declared `native` so that its `RegisterNatives` accepts them.
* `port_apk.py finish ... --dex-in-lib`: this packer walks the APK's central directory and refuses to start when it finds
  a `classes2.dex`, so the extra dex files are shipped as `lib/<abi>/liba32dex<N>.so`. The bridge loads them with an
  `InMemoryDexClassLoader` and appends them to the app class loader (`src/jni/dexload.cpp`, API 26+).
* The original signing certificate, shipped as `lib/<abi>/liba32sig.so` (done by `--jiagu-bridge`): the APK is re-signed
  and the packer compares the certificate bytes, so the bridge answers `Signature.toByteArray` / `toCharsString` /
  `JarEntry.getCertificates` with it.

What the bridge provides for the packer (`src/libc/system_imports.cpp`, `syscalls.cpp`): a synthetic
`/proc/self/maps` (also `/proc/<pid>/maps`, also through a raw `open`), raw `svc #0` system calls, and the
`art::DexFile::OpenMemory` entry points. SDKs inside a packed game that need their own 32-bit natives (carrier payment
and login SDKs) still cannot run; `port_apk.py` makes the Chinese Plants vs. Zombies 2 builds start without them
(null-safe Unicom wrapper, Talkweb initialisation reported as done), but payments through them do not work. See also
`tools/jiagu_run/README.md`.

## Tools

| Tool | Purpose |
| --- | --- |
| `tools/port_apk.py` | Prepares a decoded APK for the bridge (see above); `port_apk.py finish` restores the packed dex of a packed game |
| `tools/install_ids/` | Java helper that `port_apk.py` adds to the APK: gives games that hash `IMEI + MAC` into a device id a per-device value instead of the empty IMEI / masked MAC of Android 10+ |
| `tools/jiagu_run/` | Runs a 32-bit library, for example a packer stub such as 360 Jiagu, with a fake JNI environment, to study or unpack it without running the app (Linux/WSL host, or the device through `adb shell`; see its README) |

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
