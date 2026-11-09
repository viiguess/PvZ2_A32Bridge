# install_ids

Per-device identifiers for games that build a "device id" from `IMEI + Wi-Fi MAC`.

Android 10+ gives ordinary apps an empty IMEI and the constant MAC `02:00:00:00:00:00`, so such a game computes
the same id on every device. `port_apk.py` routes the game's `WifiInfo.getMacAddress()` and
`TelephonyManager.getDeviceId()/getImei()` calls through `com.a32.InstallIds`, which returns the real value when Android
still provides one and otherwise a value derived from the device:

| Value | Source |
| --- | --- |
| id | `MD5(Settings.Secure.ANDROID_ID + ":" + package name)`; if `ANDROID_ID` is unusable, a random id saved in `files/.a32_install_id` |
| MAC | `02:` + five bytes of the id (locally administered) |
| IMEI | 15 digits mixed from the id |

`ANDROID_ID` survives restarts and uninstall / reinstall and changes only on a factory reset or when the APK is signed
with a different key (Android 8+), so a device keeps its game id and different devices get different ones. Always sign
releases with the same key. Two emulator instances cloned from one image share an `ANDROID_ID`.

The class lives in the Java layer, so it works for the arm64 and x86_64 bridges and on 32-bit ARM devices alike.

## Files

* `src/com/a32/InstallIds.java`: the source. Only `java.*` is used, so plain `javac` compiles it.
* `InstallIds.smali`: generated from it and copied into the APK by `port_apk.py`.
* `build.sh <d8.jar> <baksmali.jar>`: regenerates `InstallIds.smali` (needs a JDK, `d8` from the Android build-tools
  and baksmali).
