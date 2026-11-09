#!/bin/bash
# Compiles InstallIds.java and writes tools/install_ids/InstallIds.smali (the file port_apk.py copies into an APK).
# Needs a JDK, d8 (Android build-tools) and baksmali.   build.sh <d8.jar> <baksmali.jar>
set -e
cd "$(dirname "$0")"
D8=${1:-/c/Android/build-tools/lib/d8.jar}
BAKSMALI=${2:-baksmali.jar}
rm -rf out && mkdir out
javac --release 8 -d out src/com/a32/InstallIds.java
java -cp "$D8" com.android.tools.r8.D8 --release --min-api 21 --output out out/com/a32/InstallIds.class
java -jar "$BAKSMALI" d out/classes.dex -o out/smali
cp out/smali/com/a32/InstallIds.smali InstallIds.smali
rm -rf out
