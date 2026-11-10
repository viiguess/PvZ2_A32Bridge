# jiagu_run

Runs a 32-bit ARM Android library on a **Linux host** (WSL works) inside the a32bridge runtime, against a fake JNI
environment that logs everything the library asks for. It exists to study and unpack packers such as 360 Jiagu
(`libjiagu.so`) on a PC, without a device or emulator.

```
jiagu_run <lib.so> <apk> <workdir> [--package NAME] [--version-name NAME] [--version-code N]
```

It loads the library, runs its constructors and `JNI_OnLoad`, then prints what happened. Nothing is installed anywhere.

## Building and running

```sh
sudo apt install g++ cmake ninja-build zlib1g-dev        # once
tools/jiagu_run/jr.sh build                              # -> ~/jr_build/jiagu_run
tools/jiagu_run/jr.sh run libjiagu.so game.apk /data/data/com.example.game
tools/jiagu_run/jr.sh run --trace ...                    # also log every guest -> host call (very verbose)
```

From Windows: `wsl -e bash /mnt/c/.../tools/jiagu_run/jr.sh run ...`.

The library file **must be named `libjiagu.so`**: the packer looks for that name in its own path and kills itself otherwise.
The runner needs `python3` on the host, plus `openssl` (the APK's signing certificate, `apk_cert.py`).

The workdir stands in for the app's data directory. Packers hard-code `/data/data/<package>`, so create it first
(`sudo mkdir -p /data/data/<package>/files && sudo chmod -R 777 /data`) and pass it as the workdir.

`JR_DUMP=<hexaddr>:<hexlen>[,...]` writes guest memory ranges to `<workdir>/dump_<hexaddr>.bin` after the run, for
example to inspect a second-stage library that the packer unpacked into memory.

## Reading the output

| Prefix | Meaning |
| --- | --- |
| `[run]` | The runner itself: load address, constructors, `mmap` calls, system properties read, dumps |
| `[jni]` | A JNI call made by the library, with decoded arguments and the fake result. `UNIMPLEMENTED` marks a JNI function with no implementation yet |
| `[sys]` | A raw Linux system call (`svc #0`), with its result. `UNIMPLEMENTED` fails with `ENOSYS` |
| `[a32]` | The shared runtime: `unresolved import`, `UNIMPLEMENTED import called`, `BAD MEM`, `dlsym miss`, and (with `--trace`) every call |

New requirements of a library show up as `UNIMPLEMENTED` lines or wrong answers in `[jni]` lines; the fix is almost
always one case in `fake_java.cpp` (what a Java method or field returns), `fake_jni.cpp` (a JNI function), `host_env.cpp`
(a libc function) or `syscalls.cpp` (a system call).

## Layout

| File | Role |
| --- | --- |
| `main.cpp` | Command line, guest address space, load library, run constructors and `JNI_OnLoad`, memory dumps |
| `fake_java.*` | The fake Java world: object handles, classes, method and field ids, and the canned answers for Android APIs (package info, `Build.*`, the APK path, ...) |
| `fake_jni.*` | The guest `JNIEnv` / `JavaVM` tables; every JNI function logs and dispatches to `fake_java` |
| `host_env.*` | Libc imports the game library never needed (`mmap`, `strdup`, `uname`, rwlocks, ...), the system-property table, and the synthetic `/proc/self/maps` |
| `apk_cert.py` | Prints the APK's v1 signing certificate (DER hex, or `--subject`); the fake `PackageManager` answers `GET_SIGNATURES` with it, because newer packers compare its hash |
| `fake_libart.py`, `art_symbols.txt` | Build a tiny ELF `libart.so` / `linker` exporting the given symbols. Old packers parse `/system/lib/libart.so` and inline-hook ART; `art_symbols.txt` is the union of the ART symbols all old builds ask for |
| `syscalls.cpp` | Emulation of Linux ARM system calls issued with a raw `svc #0` |
| `jni_names.inc` | Names of the `JNIEnv` function table slots, in order (generated from `jni.h`) |
| `shim/android/log.h` | Minimal `<android/log.h>`, so the shared runtime builds on a Linux host |

The translation runtime (`src/runtime`) and the libc layer (`src/libc`) are shared with the Android bridge. The host
build differs only where `A32_HOST_RUNNER` is defined (`src/libc/stdio_imports.cpp`): glibc's `fpos_t` and the log
function. Two optional hooks let an embedding program extend the runtime: `g_path_hook` (rewrite opened paths, see
`libc/stdio_imports.h`) and `g_syscall_hook` (emulate raw system calls, see `runtime/cpu.h`).

## Getting the decrypted dex

A packer hands its decrypted dex to ART through `art::DexFile::OpenMemory` (Jiagu 2.1.x and 2.2.0 use `OpenCommon`; the
Android 6+ variants return a `unique_ptr` through a hidden pointer, which the stubs handle). The runner answers those
lookups with a function that saves each dex to `<workdir>/classes_unpacked.dex`, `classes_unpacked2.dex`, ... and returns a
fake `DexFile` (virtual methods that return 0, fields pointing at the dex), so the packer carries on. Newer packers hand
over two or three dex files; pass them all to the ported APK (`port_apk.py --jiagu-dex` is repeatable). The very old
packers decrypt into fresh mappings without calling ART; the runner then scans guest memory for valid dex images.

Other things the runner fakes for the packers: an Android environment (`getenv`,
`Build.*`, system properties, a Pixel-like device), `/proc/self/maps` with libc, libart, the linker and friends, `execve` of
`/system/bin/dex2oat` (creates the oat file, exits 0), `fork`/`waitpid` for the helper process of old packers, and the
`.jiagu` files they write. The old packers (version code <= 865) also get `JR_OAT=1` and a libart with the ART symbol list;
the environment variables `JR_OAT`, `JR_LIBART_SYMS` (`none` disables) and `JR_LINKER_SYMS` override those defaults, and
`JR_SDK=<n>` pretends to be another Android version.

`JR_CALL=name[,name...]` calls natives the library registered (`RegisterNatives`) once `JNI_OnLoad` has run, with
placeholder arguments (`JR_INT` is the value of integer arguments), and the `[jni]` lines show what they do. This is how
the protected classes were found: `JR_CALL=interface11 JR_INT=9` makes the stub register the native `onCreate` of the
class whose static initializer calls `StubApp.interface11(9)`.

## Running it on a device

There is no need for a Linux host: the runner is an ordinary arm64 program. Build it with the NDK
(`cmake -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a ...`; bionic has no
`libpthread`, so pass `-DCMAKE_EXE_LINKER_FLAGS=-L<dir with an empty libpthread.a>`), push it with the library and the APK to
`/data/local/tmp`, and run it with `adb shell`.
