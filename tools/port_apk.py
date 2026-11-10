#!/usr/bin/env python3
"""Prepare an apktool-decoded 32-bit ARM game for a32bridge: 64-bit layouts plus fixes for modern Android.

    apktool d game.apk -o decoded
    python port_apk.py decoded build_arm64/libGame.so ported --bridge build_x86_64/libGame.so \
        [--pin-intents com/example/BootReceiver.smali]
    apktool b -api 30 ported -o game_unsigned.apk        # then zipalign + apksign it
    (without -api 30 the dex targets minSdk and Java 8 interface methods fail with IncompatibleClassChangeError)

What it does:
  1. For each bridge (positional and --bridge; the ABI, arm64-v8a or x86_64, comes from the ELF header):
     lib/<abi>/lib<name>.so          the bridge (the file name chooses <name>, which must match the library the
                                     game loads with System.loadLibrary("<name>"))
     lib/<abi>/lib<name>_a32.so      the game's original armeabi/armeabi-v7a library, which the bridge loads
     An x86_64 bridge lets x86 emulators (MuMu, LDPlayer, WSA) run the game without their slower ARM translator.
  2. Keeps the 32-bit libraries under lib/armeabi{,-v7a}, so the same APK also installs natively on 32-bit ARM
     devices (Android picks the first ABI of the device's list that the APK has). With --arm64-only they are removed.
     Other 32-bit SDK libraries cannot be loaded into a 64-bit process, so their System.loadLibrary calls are
     redirected to "<name>" (loading an already-loaded library is a no-op) and the Java side still starts. Natives of
     those libraries are NOT available (on 32-bit devices too, because the redirect is in the Java code); code that
     calls them throws UnsatisfiedLinkError when reached.
  2b. Signature checks are answered by the bridge (ORIG_SIG_HASH, default: the PopCap/EA certificate), so the game
     library is left unmodified. The real hash is read from the APK's original signature and reported; if it differs
     from the bridge's default, rebuild the bridge with -DORIG_SIG_HASH=<value>.
  3. Raises minSdk/targetSdk to values arm64 devices accept, keeps scoped storage off for old games, and removes
     android:debuggable.
  4. With --pin-intents, pins implicit service intents in the given smali files to the app's own package (Android 5+
     throws "Service Intent must be explicit"). Candidates are listed; pin only services that belong to the app itself,
     never ones that live in another package (for example Google Play billing).
  5. Fixes style parents that apktool decodes as attribute references and that aapt2 then rejects.
  6. Stubs the native methods of SDK classes (payment, analytics, ...): their 32-bit libraries cannot be loaded, and
     calling them would otherwise throw UnsatisfiedLinkError, often on a worker thread. The game's own classes
     (com/popcap), and the packer stub (com/stub, com/qihoo), keep theirs.
  7. Guards TelephonyManager.getDeviceId / getImei / getSubscriberId / getSimSerialNumber / getMeid / getLine1Number,
     which throw SecurityException on Android 10+, so that SDKs calling them at start-up do not crash.
     Games that hash "IMEI + MAC" into a device id would give every Android 10+ device the same id (the IMEI is
     empty and the MAC is the constant 02:00:00:00:00:00), so an empty getDeviceId / getImei and a masked
     WifiInfo.getMacAddress are replaced by values derived from the device's ANDROID_ID (tools/install_ids). The id
     survives restarts and reinstalls and only changes on a factory reset or when the APK's signing key changes.
  8. 360 Jiagu packed games (--jiagu-bridge, --jiagu-dex, --baksmali): see "Packed games" in the README. After
     apktool has rebuilt the APK, `port_apk.py finish original.apk rebuilt.apk output.apk [--dex-in-lib]` puts the packed classes.dex
     and the ZIP comment back; then zipalign and sign.
  8b. Muzhiwan embed SDK (a wrapper around some Chinese builds, 1.0.x): removed. Its Application, launcher activity
     and signature checks (`PopUtils`) are not needed and cannot run here.
  9. Talkweb / China Unicom SDKs of the Chinese Plants vs. Zombies 2 builds (2.x): their start-up needs 32-bit natives,
     so UnicomApplicationWrapper is made null-safe and TWPurchase's SDK initialisation reports success directly.
     Applied only when those classes exist; payments through these SDKs do not work.
"""
import argparse
import os
import re
import shutil
import subprocess
import zipfile
import sys


def read(path):
    with open(path, encoding='utf8') as f:
        return f.read()


def write(path, text):
    with open(path, 'w', encoding='utf8') as f:
        f.write(text)


def smali_dirs(root):
    """The `smali` directory and the `smali_classesN` directories apktool writes for additional dex files."""
    return [os.path.join(root, d) for d in sorted(os.listdir(root)) if d == 'smali' or d.startswith('smali_classes')]


def smali_files(root):
    for smali_dir in smali_dirs(root):
        for base, _, files in os.walk(smali_dir):
            for name in files:
                if name.endswith('.smali'):
                    yield os.path.join(base, name)


def class_path(root, path):
    """`com/example/Foo.smali` for a file under any of the smali directories."""
    return os.path.relpath(path, root).replace(os.sep, '/').split('/', 1)[1]


DEFAULT_SIG_HASH = 500290098  # CMakeLists.txt default: the PopCap/EA signing certificate


def der_item(data, pos):
    """Returns (tag, start_of_content, end) of the DER element at `pos`."""
    tag = data[pos]
    length = data[pos + 1]
    pos += 2
    if length & 0x80:
        count = length & 0x7f
        length = int.from_bytes(data[pos:pos + count], 'big')
        pos += count
    return tag, pos, pos + length


def original_certificate(decoded):
    """DER of the first signer certificate of the original APK (what PackageInfo.signatures[0] holds), or None."""
    meta = os.path.join(decoded, 'original', 'META-INF')
    if not os.path.isdir(meta):
        return None
    for name in sorted(os.listdir(meta)):
        if not name.upper().endswith(('.RSA', '.DSA', '.EC')):
            continue
        with open(os.path.join(meta, name), 'rb') as f:
            data = f.read()
        try:  # PKCS#7: ContentInfo { oid, [0] SignedData { version, digestAlgs, contentInfo, [0] certificates ... } }
            _, start, _ = der_item(data, 0)
            _, start, _ = der_item(data, der_item(data, start)[2])      # skip the OID, enter [0]
            _, start, _ = der_item(data, start)                          # enter SignedData
            pos = start
            for _ in range(3):                                           # version, digestAlgorithms, contentInfo
                pos = der_item(data, pos)[2]
            tag, start, _ = der_item(data, pos)                          # [0] certificates
            if tag != 0xa0:
                continue
            _, _, end = der_item(data, start)
            cert = data[start:end]
        except IndexError:
            continue
        return cert
    return None


def original_signature_hash(decoded):
    """Java's Signature.hashCode() for the first signer certificate of the original APK, or None."""
    cert = original_certificate(decoded)
    if cert is None:
        return None
    h = 1
    for b in cert:  # java.util.Arrays.hashCode(byte[])
        h = (31 * h + (b - 256 if b > 127 else b)) & 0xffffffff
    return h - (1 << 32) if h > 0x7fffffff else h


# The bridge answers signature checks with the original certificate (Signature.toByteArray, JarEntry.getCertificates,
# ...), which packers such as 360 Jiagu 2.3.4+ compare byte for byte. It reads it from this file in the native library
# directory (the APK is re-signed, so the real one is gone).
CERT_FILE = 'liba32sig.so'


def install_original_certificate(decoded, out):
    cert = original_certificate(decoded)
    if cert is None:
        return
    for abi in ('arm64-v8a', 'x86_64'):
        if os.path.isdir(os.path.join(out, 'lib', abi)):
            with open(os.path.join(out, 'lib', abi, CERT_FILE), 'wb') as f:
                f.write(cert)


ELF_MACHINE_ABIS = {183: 'arm64-v8a', 62: 'x86_64'}  # EM_AARCH64, EM_X86_64


def library_abi(path):
    """The Android ABI directory for a 64-bit library, from its ELF header."""
    with open(path, 'rb') as f:
        header = f.read(20)
    if header[:4] != b'\x7fELF':
        sys.exit('%s is not an ELF library' % path)
    machine = int.from_bytes(header[18:20], 'little')
    if machine not in ELF_MACHINE_ABIS:
        sys.exit('%s: ELF machine %d; bridges and stubs must be built for arm64-v8a or x86_64' % (path, machine))
    return ELF_MACHINE_ABIS[machine]


def by_abi(paths):
    """{abi: path} for libraries of different ABIs; two of the same ABI is an error."""
    result = {}
    for path in paths:
        abi = library_abi(path)
        if abi in result:
            sys.exit('two %s libraries given: %s and %s' % (abi, result[abi], path))
        result[abi] = path
    return result


def original_library(lib, name):
    for abi in ('armeabi-v7a', 'armeabi'):
        candidate = os.path.join(lib, abi, 'lib%s.so' % name)
        if os.path.exists(candidate):
            return candidate
    return None


def install_mods(out, mods, abis, stubs=None):
    """Hook libraries that patch the game at run time: shipped as lib<mod>_a32.so for the bridge to run, with the
    empty stub ({abi: path}) under the original name so Java's loadLibrary still succeeds."""
    lib = os.path.join(out, 'lib')
    stubs = stubs or {}
    for mod in mods:
        original = original_library(lib, mod)
        if original is None:
            print('WARNING: mod library lib%s.so not found' % mod)
            continue
        for abi in abis:
            shutil.copy(original, os.path.join(lib, abi, 'lib%s_a32.so' % mod))
            if abi in stubs:
                shutil.copy(stubs[abi], os.path.join(lib, abi, 'lib%s.so' % mod))
            elif stubs:
                print('WARNING: no %s --stub: System.loadLibrary("%s") fails on %s devices' % (abi, mod, abi))
        print('mod library installed: lib%s_a32.so (%s)' % (mod, ', '.join(abis)))


def pad_abi_dirs(out, abis, stubs):
    """Puts the empty stub under every 32-bit library name missing from the bridge ABI directories. x86 emulators
    pick the ARM ABI when lib/x86_64 lacks libraries that lib/armeabi has. Returns the number of stubs installed."""
    lib = os.path.join(out, 'lib')
    names = set()
    for abi in ('armeabi', 'armeabi-v7a'):
        if os.path.isdir(os.path.join(lib, abi)):
            names.update(n for n in os.listdir(os.path.join(lib, abi)) if n.endswith('.so'))
    count = 0
    for abi in abis:
        missing = sorted(n for n in names if not os.path.exists(os.path.join(lib, abi, n)))
        if missing and abi not in stubs:
            print('WARNING: no %s --stub: lib/%s lacks %s; x86 emulators may pick the 32-bit ARM libraries instead'
                  % (abi, abi, ', '.join(missing)))
            continue
        for name in missing:
            shutil.copy(stubs[abi], os.path.join(lib, abi, name))
            count += 1
    return count


def install_libraries(out, bridges, name, keep_32bit=True):
    """`bridges`: {abi: bridge library}."""
    lib = os.path.join(out, 'lib')
    original = original_library(lib, name)
    if original is None:
        sys.exit('no lib%s.so found under lib/armeabi{,-v7a}' % name)
    for abi, bridge in sorted(bridges.items()):
        target = os.path.join(lib, abi)
        os.makedirs(target, exist_ok=True)
        shutil.copy(bridge, os.path.join(target, 'lib%s.so' % name))
        shutil.copy(original, os.path.join(target, 'lib%s_a32.so' % name))
        print('bridge installed: lib/%s/lib%s.so + lib%s_a32.so' % (abi, name, name))
    if not keep_32bit:
        for abi in ('armeabi', 'armeabi-v7a'):
            shutil.rmtree(os.path.join(lib, abi), ignore_errors=True)


LOAD_LIBRARY = re.compile(
    r'(const-string v\d+, )"(?!(?:%s)")[A-Za-z0-9_]+"(\s*\n\s*invoke-static \{v\d+\}, '
    r'Ljava/lang/System;->loadLibrary\(Ljava/lang/String;\)V)')


def redirect_load_library(out, name, keep=()):
    """Every System.loadLibrary("x") becomes loadLibrary("<name>"), except the names in `keep` (the mod libraries: on
    32-bit devices the original ones load; on 64-bit devices stub libraries of the same name do)."""
    pattern = re.compile(LOAD_LIBRARY.pattern % '|'.join(re.escape(n) for n in (name,) + tuple(keep)))
    total = 0
    for path in smali_files(out):
        text = read(path)
        new, count = pattern.subn(lambda m: m.group(1) + '"%s"' % name + m.group(2), text)
        if count:
            write(path, new)
            total += count
    return total


def patch_apktool_yml(out):
    path = os.path.join(out, 'apktool.yml')
    text = read(path)
    text = re.sub(r'minSdkVersion: \d+', 'minSdkVersion: 24', text)
    text = re.sub(r'targetSdkVersion: \d+', 'targetSdkVersion: 29', text)
    write(path, text)


def patch_manifest(out):
    path = os.path.join(out, 'AndroidManifest.xml')
    text = read(path)
    text = text.replace('<application ', '<application android:requestLegacyExternalStorage="true" ', 1)
    text = text.replace('</application>',
                        '    <uses-library android:name="org.apache.http.legacy" android:required="false"/>\n'
                        '    </application>', 1)
    text = text.replace('android:debuggable="true" ', '')
    text = re.sub(r'\s+android:qihoo="[^"]*"', '', text)  # 360 Jiagu marker; aapt2 rejects the unknown attribute
    write(path, text)
    match = re.search(r'package="([^"]+)"', text)
    return match.group(1) if match else None


INTENT_NEW = re.compile(r'(invoke-direct \{(v\d+), v\d+\}, Landroid/content/Intent;-><init>\(Ljava/lang/String;\)V\n)')
METHOD = re.compile(r'\.method [^\n]*\n(?:(?!\.end method).*\n)*?\.end method')


def service_intent_candidates(out):
    """Smali files that start/bind a service with a `new Intent(String)` (an implicit intent)."""
    found = []
    for path in smali_files(out):
        text = read(path)
        if ('startService' in text or 'bindService' in text) and INTENT_NEW.search(text):
            found.append(class_path(out, path))
    return found


def pin_service_intents(out, package, files):
    """Adds Intent.setPackage(package) after every `new Intent(String)` in service-starting methods of `files`."""
    changed = 0
    for rel in files:
        path = find_class(out, rel)
        if path is None:
            continue
        text = read(path)

        def fix(match):
            body = match.group(0)
            if ('startService' not in body and 'bindService' not in body) or not INTENT_NEW.search(body):
                return body
            locals_ = int(re.search(r'\.locals (\d+)', body).group(1))
            body = body.replace('.locals %d' % locals_, '.locals %d' % (locals_ + 1), 1)  # one scratch register
            return INTENT_NEW.sub(
                lambda m: m.group(1) + '\n    const-string v%d, "%s"\n\n'
                '    invoke-virtual {%s, v%d}, Landroid/content/Intent;->setPackage(Ljava/lang/String;)'
                'Landroid/content/Intent;\n' % (locals_, package, m.group(2), locals_), body)

        new = METHOD.sub(fix, text)
        if new != text:
            write(path, new)
            changed += 1
    return changed


# ---- telephony identifiers -------------------------------------------------------------------------------------------
# On Android 10+ these TelephonyManager calls throw SecurityException for ordinary apps, and old payment / analytics SDKs
# call them unguarded (often in Application.onCreate, which kills the app at start-up). Every call is routed through a
# generated helper class that catches the exception and returns null, which is what a device without a SIM reports.
TELEPHONY = 'Landroid/telephony/TelephonyManager;'
SAFE_IDS_CLASS = 'com/a32/SafeIds'
GUARDED_TELEPHONY = {  # TelephonyManager method -> name of the helper
    'getSimSerialNumber': 'iccid',
    'getSubscriberId': 'imsi',
    'getDeviceId': 'deviceId',
    'getImei': 'imei',
    'getMeid': 'meid',
    'getLine1Number': 'line1',
}
TELEPHONY_CALL = re.compile(
    r'invoke-virtual \{([^}]*)\}, %s->(%s)\((I?)\)Ljava/lang/String;' % (TELEPHONY, '|'.join(GUARDED_TELEPHONY)))

SAFE_IDS_METHOD = '''\
.method public static {helper}(L{tm};{param})Ljava/lang/String;
    .locals 1
    :try_start
    invoke-virtual {{{regs}}}, L{tm};->{method}({param})Ljava/lang/String;
    move-result-object v0
    :try_end
    .catch Ljava/lang/Throwable; {{:try_start .. :try_end}} :handler
    return-object v0
    :handler
    const/4 v0, 0x0
    return-object v0
.end method
'''


# getDeviceId / getImei: a missing IMEI (Android 10+) is replaced by one derived from the device id (InstallIds)
SAFE_IMEI_METHOD = '''.method public static {helper}(L{tm};)Ljava/lang/String;
    .locals 1
    :try_start
    invoke-virtual {{p0}}, L{tm};->{method}()Ljava/lang/String;
    move-result-object v0
    :try_end
    .catch Ljava/lang/Throwable; {{:try_start .. :try_end}} :handler
    :done
    invoke-static {{v0}}, Lcom/a32/InstallIds;->imei(Ljava/lang/String;)Ljava/lang/String;
    move-result-object v0
    return-object v0
    :handler
    const/4 v0, 0x0
    goto :done
.end method
'''

# WifiInfo.getMacAddress: Android masks the MAC (02:00:00:00:00:00) for apps; InstallIds supplies a per-device one
WIFI = 'Landroid/net/wifi/WifiInfo;'
WIFI_MAC_CALL = re.compile(r'invoke-virtual \{([^}]*)\}, %s->getMacAddress\(\)Ljava/lang/String;' % WIFI)
SAFE_MAC_METHOD = '''.method public static wifiMac(%s)Ljava/lang/String;
    .locals 1
    :try_start
    invoke-virtual {p0}, %s->getMacAddress()Ljava/lang/String;
    move-result-object v0
    :try_end
    .catch Ljava/lang/Throwable; {:try_start .. :try_end} :handler
    :done
    invoke-static {v0}, Lcom/a32/InstallIds;->mac(Ljava/lang/String;)Ljava/lang/String;
    move-result-object v0
    return-object v0
    :handler
    const/4 v0, 0x0
    goto :done
.end method
''' % (WIFI, WIFI)
# generated from tools/install_ids/src by tools/install_ids/build.sh
INSTALL_IDS_SMALI = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'install_ids', 'InstallIds.smali')


def safe_ids_method(method, with_slot):
    if method in ('getDeviceId', 'getImei') and not with_slot:
        return SAFE_IMEI_METHOD.format(helper=GUARDED_TELEPHONY[method], tm=TELEPHONY[1:-1], method=method)
    return SAFE_IDS_METHOD.format(
        helper=GUARDED_TELEPHONY[method] + ('Slot' if with_slot else ''), tm=TELEPHONY[1:-1], method=method,
        param='I' if with_slot else '', regs='p0, p1' if with_slot else 'p0')


def guard_identifier_calls(out):
    """Rewrites the calls and adds the helper classes (SafeIds, InstallIds). Returns the number of calls rewritten."""
    used = set()  # (method, with_slot) pairs that need a helper
    wifi_used = False

    def rewrite(m):
        used.add((m.group(2), bool(m.group(3))))
        return 'invoke-static {%s}, L%s;->%s%s(L%s;%s)Ljava/lang/String;' % (
            m.group(1), SAFE_IDS_CLASS, GUARDED_TELEPHONY[m.group(2)], 'Slot' if m.group(3) else '',
            TELEPHONY[1:-1], m.group(3))

    count = 0
    for path in smali_files(out):
        text = read(path)
        if TELEPHONY + '->' not in text and WIFI + '->' not in text:
            continue
        new, n = TELEPHONY_CALL.subn(rewrite, text)
        new, macs = WIFI_MAC_CALL.subn(
            lambda m: 'invoke-static {%s}, L%s;->wifiMac(%s)Ljava/lang/String;' % (m.group(1), SAFE_IDS_CLASS, WIFI), new)
        if n or macs:
            write(path, new)
            count += n + macs
            wifi_used |= bool(macs)
    if used or wifi_used:
        path = os.path.join(smali_dirs(out)[-1], SAFE_IDS_CLASS + '.smali')  # last dex: the first may be a packed stub
        os.makedirs(os.path.dirname(path), exist_ok=True)
        header = '.class public L%s;\n.super Ljava/lang/Object;\n\n' % SAFE_IDS_CLASS
        methods = [safe_ids_method(method, slot) for method, slot in sorted(used)]
        if wifi_used:
            methods.append(SAFE_MAC_METHOD)
        write(path, header + '\n'.join(methods))
        install_ids = os.path.join(os.path.dirname(path), 'InstallIds.smali')  # com/a32, next to SafeIds
        shutil.copyfile(INSTALL_IDS_SMALI, install_ids)
    return count


# ---- native methods of SDK classes -----------------------------------------------------------------------------------
# `.method <flags> native <name>(<params>)<ret>` ... `.end method`
NATIVE_METHOD = re.compile(
    r'^(\.method [^\n]*?) native ([^\n(]+\([^)]*\)(\S+))[ \t]*\n'  # header: flags, "name(params)ret", return type
    r'((?:(?!\.end method)[^\n]*\n)*?)'                            # annotations / .param lines
    r'\.end method', re.M)


def default_return(ret):
    if ret == 'V':
        return '    return-void'
    if ret in 'JD':
        return '    const-wide/16 v0, 0x0\n    return-wide v0'
    if ret[0] in 'L[':
        return '    const/4 v0, 0x0\n    return-object v0'
    return '    const/4 v0, 0x0\n    return v0'


def stub_foreign_natives(out, keep=('com/popcap/', 'com/ideaworks3d/marmalade/', 'com/stub/', 'com/qihoo', 'com/jg/',
                                    'com/common/busi/CustomView')):
    """Native methods of SDK classes live in 32-bit libraries the bridge cannot load, so calling them throws
    UnsatisfiedLinkError (often on a worker thread, which kills the process). They are turned into methods that return
    0 / null / void. Classes under the `keep` prefixes (the game, the Marmalade loader and the packer stub) are left
    alone: their natives come from the game library or the packer. Returns the number of methods stubbed."""
    count = 0

    def stub(m):
        nonlocal count
        if m.group(2).startswith('onCreate('):  # hollowed by a packer, bound by its natives
            return m.group(0)
        count += 1
        return '%s %s\n    .locals 2\n%s%s\n.end method' % (m.group(1), m.group(2), m.group(4), default_return(m.group(3)))

    for path in smali_files(out):
        if class_path(out, path).startswith(keep):
            continue
        text = read(path)
        if ' native ' in text:
            new = NATIVE_METHOD.sub(stub, text)
            if new != text:
                write(path, new)
    return count


# ---- 360 Jiagu -------------------------------------------------------------------------------------------------
# The game's dex is encrypted inside classes.dex and its Activity.onCreate methods are hollowed out; both are handled by
# a native library (libjiagu) that cannot run in a 64-bit process. The bridge runs it instead:
#   * build the bridge with -DLIBNAME=jiagu and pass it as --jiagu-bridge. The stub copies assets/libjiagu_a64.so out of
#     the APK on 64-bit devices and loads it, so the bridge takes that place; the packer's own 32-bit library becomes
#     lib/arm64-v8a/libjiagu_a32.so, where the bridge finds it.
#   * the stub and its encrypted payload live in classes.dex, which apktool cannot rebuild (the payload is not part of
#     the dex's classes). Run `port_apk.py finish` on the rebuilt APK to put the original file back.
#   * the decrypted dex (tools/jiagu_run saves it as classes_unpacked.dex) is added as an extra dex with --jiagu-dex, so
#     that the classes exist: the bridge cannot hand a dex to ART the way the packer does on an old Android.
def add_unpacked_dex(out, dex, baksmali):
    number = 2
    while os.path.exists(os.path.join(out, 'smali_classes%d' % number)):
        number += 1
    target = os.path.join(out, 'smali_classes%d' % number)
    subprocess.run(['java', '-jar', baksmali, 'd', dex, '-o', target], check=True)
    return target


# 360 Jiagu turns the onCreate of some activities into natives whose real code lives in the packer's payload (it needs the
# packer's ART hooks and cannot run here). With --unhollow they are replaced by the shortest bodies that keep the game
# running: the launcher goes straight to the game activity (Talkweb's PermisionActivity), the game activity calls super,
# every other one (payment, ad, share and crash screens) calls super and finishes.
HOLLOW_ONCREATE = re.compile(r'^\.method ([^\n]*?)native onCreate\(Landroid/os/Bundle;\)V\n(?:    [^\n]*\n)*?\.end method', re.M)
TUNE_CALLS = re.compile(r'    invoke-static \{\}, Lcom/tune/Tune;->getInstance\(\)Lcom/tune/Tune;\n\n    move-result-object v0\n\n'
                        r'    invoke-virtual \{v0(?:, p0)?\}, Lcom/tune/Tune;->(?:setReferralSources\(Landroid/app/Activity;\)|measureSession\(\))V\n\n?')


# The packer's support classes are plain Java stubs in the unpacked dex; on a device the packer turns these methods into
# natives (its ART hooks) before RegisterNatives binds them. Declare them native so that RegisterNatives accepts them.
JIAGU_NATIVE_METHODS = {
    'com/common/busi/CustomView': ['getAppkey()Ljava/lang/String;', 'getPT()J'],
    'com/jg/bh/BH': ['mark2(Ljava/lang/String;DD)V', 'mark3()Ljava/lang/String;'],
    'com/jg/ce/Interface2': ['interface%d(Landroid/content/Context;)I' % n for n in range(1, 6)],
    'com/qihoo/bugreport/javacrash/CrashReportDataFactory': ['interface9()Ljava/lang/String;'],
    'com/qihoo/jg/DeviceEntry': ['interface1()Ljava/lang/String;', 'interface2()Ljava/lang/String;', 'interface3()Ljava/lang/String;'],
    'com/stub/stub07/Stub01': ['mark1(Ljava/lang/String;)V'],
}


def nativize_jiagu(smali_dirs):
    changed = 0
    for root_dir in smali_dirs:
        for cls, methods in JIAGU_NATIVE_METHODS.items():
            path = os.path.join(root_dir, cls + '.smali')
            if not os.path.isfile(path):
                continue
            text = open(path, encoding='utf-8').read()
            for method in methods:
                pattern = re.compile(r'^\.method ([^\n]*?)%s\n(?:    [^\n]*\n|\n)*?\.end method' % re.escape(method), re.M)
                text, count = pattern.subn(lambda m: '.method %snative %s\n.end method' % (m.group(1), method)
                                           if ' native ' not in ' ' + m.group(1) else m.group(0), text)
                changed += count
            open(path, 'w', encoding='utf-8', newline='').write(text)
    return changed


def unhollow_jiagu(smali_dirs):
    replaced = 0
    for root_dir in smali_dirs:
        for directory, _, files in os.walk(root_dir):
            for name in files:
                if not name.endswith('.smali'):
                    continue
                path = os.path.join(directory, name)
                text = open(path, encoding='utf-8').read()
                if 'native onCreate(' not in text:
                    continue
                parent = re.search(r'^\.super (L[^;]+;)', text, re.M).group(1)
                cls = re.search(r'^\.class [^\n]*?(L[^;\s]+;)', text, re.M).group(1)
                lines = ['invoke-super {p0, p1}, %s->onCreate(Landroid/os/Bundle;)V' % parent]
                if cls.endswith('/PermisionActivity;') and 'jumpToLaunchActivity()V' in text:
                    lines += ['invoke-direct {p0}, %s->jumpToLaunchActivity()V' % cls, 'invoke-virtual {p0}, %s->finish()V' % cls]
                elif not cls.endswith('/SexyAppActivity;'):
                    lines += ['invoke-virtual {p0}, %s->finish()V' % cls]
                body = '\n'.join('    ' + line for line in lines)
                text, count = HOLLOW_ONCREATE.subn(
                    lambda m: '.method %sonCreate(Landroid/os/Bundle;)V\n    .locals 0\n%s\n    return-void\n.end method' % (m.group(1), body),
                    text)
                if count:
                    if cls.endswith('/SexyAppActivity;'):  # its hollowed onCreate also initialised the Tune SDK that onResume expects
                        text = TUNE_CALLS.sub('', text)
                    open(path, 'w', encoding='utf-8', newline='').write(text)
                    replaced += count
    return replaced


JIAGU_ASSETS = {'arm64-v8a': 'libjiagu_a64.so', 'x86_64': 'libjiagu_x64.so'}  # what the packer's stub loads per ABI


def install_jiagu(out, bridges):
    """`bridges`: {abi: bridge built with -DLIBNAME=jiagu}. The x86_64 one is untested."""
    for abi, bridge in sorted(bridges.items()):
        target = os.path.join(out, 'lib', abi)
        os.makedirs(target, exist_ok=True)
        shutil.copy(os.path.join(out, 'assets', 'libjiagu.so'), os.path.join(target, 'libjiagu_a32.so'))
        shutil.copy(bridge, os.path.join(out, 'assets', JIAGU_ASSETS[abi]))


# ---- Talkweb / China Unicom SDKs -------------------------------------------------------------------------------------
# The Chinese Plants vs. Zombies 2 builds (2.x) start two SDKs that depend on 32-bit native libraries. Without the
# following patches the game either crashes in Application.onCreate or sits on its "loading" screen. Payments through
# these SDKs do not work either way; this only lets the game start. Both patches apply only when the classes exist.
WRAPPED_CALL = re.compile(
    r'(sget-object (v\d+), Lcom/unicom/shield/UnicomApplicationWrapper;->mApplication:Landroid/app/Application;\s+)'
    r'(invoke-virtual \{\2[,}][^\n]*\n)')
TW_INIT_CALL = """    # the Talkweb offline SDK cannot initialise here (it needs its 32-bit natives): report success directly
    iget-object v0, p0, Lcom/popcap/purchase/TW/TWPurchase$1;->this$0:Lcom/popcap/purchase/TW/TWPurchase;
    iget-object v1, p0, Lcom/popcap/purchase/TW/TWPurchase$1;->this$0:Lcom/popcap/purchase/TW/TWPurchase;
    iget-wide v2, v1, Lcom/popcap/purchase/TW/TWPurchase;->mNativeDriverPtr:J
    const/4 v4, 0x1
    invoke-virtual {v0, v2, v3, v4}, Lcom/popcap/purchase/TW/TWPurchase;->Native_SDKInitEnd(JI)V
"""


def find_class(out, relative):
    """The smali file for `com/example/Foo.smali` in whichever smali directory holds it, or None."""
    for smali_dir in smali_dirs(out):
        path = os.path.join(smali_dir, *relative.split('/'))
        if os.path.exists(path):
            return path
    return None


def guard_unicom_wrapper(out):
    """UnicomApplicationWrapper keeps the real application in a field that stays null when the Unicom shield's encrypted
    jar cannot be loaded, and its onCreate / onLowMemory / ... forward to it unconditionally: guard those calls."""
    path = find_class(out, 'com/unicom/shield/UnicomApplicationWrapper.smali')
    if not path:
        return 0
    count = 0

    def guard(m):
        nonlocal count
        count += 1
        label = ':unicom_none_%d' % count
        return '%sif-eqz %s, %s\n\n    %s    %s' % (m.group(1), m.group(2), label, m.group(3), label)

    write(path, WRAPPED_CALL.sub(guard, read(path)))
    return count


def skip_talkweb_init(out):
    """TWPurchase.InitializeSDK starts the Talkweb offline SDK and waits for its callback, which reports to the game
    through Native_SDKInitEnd. The SDK never answers, so the game waits forever: report success directly."""
    path = find_class(out, 'com/popcap/purchase/TW/TWPurchase$1.smali')
    if not path:
        return False
    text = read(path)
    method = re.search(r'\.method public run\(\)V.*?\.end method', text, re.S)
    first = text.find('    invoke-static {}, Lcom/popcap/SexyAppFramework/SexyAppFrameworkActivity;->instance()', method.start())
    last = text.find('    return-void', first)
    if first < 0 or last < 0:
        sys.exit('TWPurchase$1.run() does not look like the expected Talkweb initialiser')
    body = text[method.start():first] + TW_INIT_CALL + text[last:method.end()]
    body = re.sub(r'\.registers \d+', '.registers 8', body, count=1)
    write(path, text[:method.start()] + body + text[method.end():])
    return True


# ---- Muzhiwan embed SDK ----------------------------------------------------------------------------------------------
# Some Chinese builds are wrapped by the Muzhiwan (muzhiwan.com) layer: a custom Application whose native library
# unpacks an encrypted SDK jar, a launcher activity (MzwActivity) that shows its splash, checks for updates and verifies
# the APK signature, and `PopUtils` calls inside the game that exit the process when the signing certificate differs.
# None of it is needed to run the game, and the SDK cannot start here (it needs 32-bit natives, an old Android's file
# access and an online service), so the layer is removed and the game's own activity becomes the launcher.
MZW_POP_UTILS = 'com/muzhiwan/embed/sdk/PopUtils.smali'
MZW_APPLICATION = ' android:name="com.muzhiwan.embed.sdk.MzwApplication"'
MZW_ACTIVITY = re.compile(r'\s*<activity android:name="com\.muzhiwan\.embed\.MzwActivity".*?</activity>', re.S)
MZW_SDK_CALL = re.compile(r'[ \t]*invoke-static(?:/range)? \{[^}]*\}, Lcom/muzhiwan/embed/utils/SDKUtils;->[^\n]*\n')
STATIC_VOID_METHOD = re.compile(r'^(\.method public static [^\n]*\)V)\n.*?^\.end method', re.S | re.M)


def remove_muzhiwan(out):
    """Returns the number of PopUtils methods turned into no-ops (0 when the game is not wrapped)."""
    pop = find_class(out, MZW_POP_UTILS)
    if not pop:
        return 0
    count = 0

    def empty(m):
        nonlocal count
        count += 1
        return m.group(1) + '\n    .locals 0\n    return-void\n.end method'

    write(pop, STATIC_VOID_METHOD.sub(empty, read(pop)))  # capture, showWindow, ... and the signature check
    for path in smali_files(out):  # calls into the SDK jar that is no longer there
        text = read(path)
        if 'Lcom/muzhiwan/embed/utils/SDKUtils;' in text:
            write(path, MZW_SDK_CALL.sub('', text))

    manifest = os.path.join(out, 'AndroidManifest.xml')
    text = read(manifest).replace(MZW_APPLICATION, '', 1)
    launcher = MZW_ACTIVITY.search(text)
    if launcher:  # the game activity takes over the MAIN / LAUNCHER filter
        game = re.search(r'(<activity [^>]*android:name="[^"]*SexyAppActivity"[^>]*>)\s*<intent-filter/>', text)
        filter_xml = re.search(r'<intent-filter>.*?</intent-filter>', launcher.group(0), re.S).group(0)
        text = text.replace(launcher.group(0), '', 1)
        if game:
            text = text.replace(game.group(0), game.group(1) + '\n            ' + filter_xml, 1)
    write(manifest, text)
    return count


def fix_styles(out):
    path = os.path.join(out, 'res', 'values', 'styles.xml')
    if not os.path.exists(path):
        return
    text = read(path)
    text = text.replace('parent="@android:attr/buttonStyleSmall"', 'parent="@android:style/Widget.Button.Small"')
    text = text.replace('parent="@android:attr/buttonStyle"', 'parent="@android:style/Widget.Button"')
    write(path, text)


def finish(original, rebuilt, output, dex_in_lib=False):
    """Puts the original classes.dex and ZIP comment of a packed APK back into the APK that apktool rebuilt.

    A packer such as 360 Jiagu hides its payload inside classes.dex and keeps data in the ZIP comment. apktool rebuilds
    classes.dex from smali, which drops the payload, and the comment is lost too, so the packer cannot find what it
    needs at start-up. Run this before zipalign and signing: the signature covers both.

    With dex_in_lib (`--dex-in-lib`) the extra dex files added with --jiagu-dex are moved out of the APK into
    lib/<abi>/liba32dex<N>.so, which the bridge loads at run time (jni/dexload.cpp). Jiagu 2.4.x needs this: it walks the
    APK's central directory and refuses to start when it finds a classes2.dex."""
    with zipfile.ZipFile(original) as source:
        packed_dex = source.read('classes.dex')
        comment = source.comment
        original_names = set(source.namelist())
    with zipfile.ZipFile(rebuilt) as source, zipfile.ZipFile(output, 'w') as destination:
        # Keep the original order of the entries and put new ones (extra dex files, bridge libraries) last: newer
        # packers walk the central directory and expect the entries around classes.dex to be the original ones.
        with zipfile.ZipFile(original) as reference:
            position = {info.filename: n for n, info in enumerate(reference.infolist())}
        entries = sorted(source.infolist(), key=lambda info: position.get(info.filename, len(position)))
        # Extra dex files (--jiagu-dex) must not stay in the APK as classesN.dex: Jiagu 2.4.x walks the central directory
        # and gives up on them. They ship as lib/<abi>/liba32dex<N>.so instead; the bridge loads them (jni/dexload.cpp).
        abis = [abi for abi in ('arm64-v8a', 'x86_64') if 'lib/%s/libjiagu_a32.so' % abi in source.namelist()] if dex_in_lib else []
        extra_dex = re.compile(r'classes([2-9])\.dex$')
        moved = 0
        for info in entries:
            match = extra_dex.match(info.filename)
            if match and info.filename not in original_names and abis:
                for abi in abis:
                    destination.writestr('lib/%s/liba32dex%s.so' % (abi, match.group(1)), source.read(info),
                                         compress_type=zipfile.ZIP_STORED)
                moved += 1
                continue
            data = packed_dex if info.filename == 'classes.dex' else source.read(info)
            destination.writestr(info, data, compress_type=info.compress_type)
        if moved:
            print('moved %d extra dex file(s) into lib/<abi>/liba32dex<N>.so' % moved)
        destination.comment = comment
    print('kept the packed classes.dex (%d bytes) and a %d-byte ZIP comment' % (len(packed_dex), len(comment)))


def main():
    if len(sys.argv) in (5, 6) and sys.argv[1] == 'finish':  # port_apk.py finish original.apk rebuilt.apk output.apk [--dex-in-lib]
        finish(*sys.argv[2:5], dex_in_lib=len(sys.argv) == 6 and sys.argv[5] == '--dex-in-lib')
        return
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('decoded', help='apktool-decoded APK directory')
    parser.add_argument('bridge', help='the built arm64 bridge, named lib<name>.so')
    parser.add_argument('out', help='output directory (created or updated)')
    parser.add_argument('--package', help="app package for service-intent pinning (default: from the manifest)")
    parser.add_argument('--pin-intents', nargs='*', default=[], metavar='SMALI',
                        help='smali files (relative to smali/) whose implicit service intents are pinned to the app')
    parser.add_argument('--bridge', action='append', default=[], metavar='LIB', dest='bridge_extra',
                        help='the same bridge built for another ABI (e.g. x86_64), same file name')
    parser.add_argument('--jiagu-bridge', action='append', default=[], metavar='LIB',
                        help='bridge built with -DLIBNAME=jiagu: runs the 360 Jiagu stub (repeatable, one per ABI)')
    parser.add_argument('--jiagu-dex', metavar='DEX', action='append', default=[],
                        help='decrypted dex of a 360 Jiagu app, added as an extra dex (repeatable: jiagu_run saves '
                             'classes_unpacked.dex, classes_unpacked2.dex, ... when the packer hands over several)')
    parser.add_argument('--baksmali', metavar='JAR', help='baksmali jar, needed with --jiagu-dex')
    parser.add_argument('--unhollow', action='store_true',
                        help='with --jiagu-dex: replace the packer-hollowed Activity.onCreate natives by minimal bodies')
    parser.add_argument('--mod', action='append', default=[], metavar='NAME',
                        help='hook library lib<NAME>.so to run inside the bridge (repeatable; the bridge loads '
                             'czProject, HookPVZ2 and Restart)')
    parser.add_argument('--stub', '--mod-stub', action='append', default=[], metavar='SO', dest='stub',
                        help='empty library built from tools/mod_stub.c, one per bridge ABI, used as a placeholder '
                             'for --mod names and other 32-bit libraries')
    parser.add_argument('--arm64-only', action='store_true',
                        help='drop the 32-bit libraries (default: keep them so the APK also runs natively on 32-bit ARM)')
    args = parser.parse_args()

    base = os.path.basename(args.bridge)
    if not (base.startswith('lib') and base.endswith('.so')):
        sys.exit('the bridge file must be named lib<name>.so')
    name = base[3:-3]
    for extra in args.bridge_extra:
        if os.path.basename(extra) != base:
            sys.exit('--bridge %s: every bridge build must be named %s' % (extra, base))
    bridges = by_abi([args.bridge] + args.bridge_extra)

    shutil.copytree(args.decoded, args.out, ignore=shutil.ignore_patterns('_backup'), dirs_exist_ok=True)
    sig = original_signature_hash(args.decoded)
    if sig is None:
        print('original signature hash: not found (no original/META-INF in the decoded APK)')
    elif sig == DEFAULT_SIG_HASH:
        print('original signature hash: %d (matches the bridge default)' % sig)
    else:
        print('WARNING: original signature hash is %d, not the bridge default %d.\n'
              '         Rebuild the bridge with -DORIG_SIG_HASH=%d or the game will report a modified APK.'
              % (sig, DEFAULT_SIG_HASH, sig))
    install_libraries(args.out, bridges, name, keep_32bit=not args.arm64_only)
    stubs = by_abi(args.stub)
    install_mods(args.out, args.mod, sorted(bridges), stubs)
    if args.arm64_only:
        print('32-bit libraries dropped (--arm64-only)')
    else:
        print('stub libraries installed for 32-bit library names: %d' % pad_abi_dirs(args.out, sorted(bridges), stubs))
    print('loadLibrary calls redirected to "%s": %d' % (name, redirect_load_library(args.out, name, args.mod)))
    patch_apktool_yml(args.out)
    package = args.package or patch_manifest(args.out)
    if args.package:
        patch_manifest(args.out)
    if args.pin_intents:
        print('service intents pinned in %d file(s) to %s' % (pin_service_intents(args.out, package, args.pin_intents), package))
    candidates = [c for c in service_intent_candidates(args.out) if c not in args.pin_intents]
    if candidates:
        print('implicit service intents found (pin with --pin-intents if the service is part of this app):')
        for c in candidates:
            print('  ' + c)
    fix_styles(args.out)
    if args.jiagu_dex:
        if not args.baksmali:
            sys.exit('--jiagu-dex needs --baksmali')
        added = [add_unpacked_dex(args.out, dex, args.baksmali) for dex in args.jiagu_dex]
        if args.unhollow:
            print('unhollowed %d onCreate native(s) in the unpacked dex' % unhollow_jiagu(added))
            print('declared %d packer support method(s) native' % nativize_jiagu(added))
    if args.jiagu_bridge:
        install_jiagu(args.out, by_abi(args.jiagu_bridge))
        install_original_certificate(args.decoded, args.out)
    removed = remove_muzhiwan(args.out)
    if removed:
        print('Muzhiwan embed SDK removed: %d PopUtils methods turned into no-ops, game activity is the launcher' % removed)
    guarded, skipped = guard_unicom_wrapper(args.out), skip_talkweb_init(args.out)
    if guarded or skipped:
        print('Talkweb/Unicom SDK patches: %d Unicom call(s) guarded, Talkweb init %s'
              % (guarded, 'skipped' if skipped else 'left alone'))
    print('SDK native methods stubbed: %d' % stub_foreign_natives(args.out))
    print('telephony / Wi-Fi identifier calls rewritten: %d' % guard_identifier_calls(args.out))
    print('ported ->', args.out)


if __name__ == '__main__':
    main()
