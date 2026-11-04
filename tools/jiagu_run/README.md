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
| `syscalls.cpp` | Emulation of Linux ARM system calls issued with a raw `svc #0` |
| `jni_names.inc` | Names of the `JNIEnv` function table slots, in order (generated from `jni.h`) |
| `shim/android/log.h` | Minimal `<android/log.h>`, so the shared runtime builds on a Linux host |

The translation runtime (`src/runtime`) and the libc layer (`src/libc`) are shared with the Android bridge. The host
build differs only where `A32_HOST_RUNNER` is defined (`src/libc/stdio_imports.cpp`): glibc's `fpos_t` and the log
function. Two optional hooks let an embedding program extend the runtime: `g_path_hook` (rewrite opened paths, see
`libc/stdio_imports.h`) and `g_syscall_hook` (emulate raw system calls, see `runtime/cpu.h`).

## Status

Tested with the 360 Jiagu stub of Plants vs. Zombies 2 Chinese 2.2.7. `libjiagu.so` loads, runs its anti-debug and
device-information checks, unpacks and loads its embedded second stage, and `JNI_OnLoad` returns success. It does not
call `RegisterNatives`, so the `com.stub.StubApp` natives that decrypt the dex are not available yet; the second stage
has to be analysed to find out how they are bound.
