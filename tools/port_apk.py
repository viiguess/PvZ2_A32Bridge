#!/usr/bin/env python3
"""Prepare an apktool-decoded 32-bit ARM game for a32bridge: arm64 layout plus fixes for modern Android.

    apktool d game.apk -o decoded
    python port_apk.py decoded build/libGame.so ported [--pin-intents com/example/BootReceiver.smali]
    apktool b ported -o game_unsigned.apk        # then zipalign + apksign it

What it does:
  1. lib/arm64-v8a/lib<name>.so      the bridge (the file name chooses <name>, which must match the library the
                                     game loads with System.loadLibrary("<name>"))
     lib/arm64-v8a/lib<name>_a32.so  the game's original armeabi/armeabi-v7a library, which the bridge loads
  2. Keeps the 32-bit libraries under lib/armeabi{,-v7a}, so the same APK also installs natively on 32-bit ARM
     devices (Android picks arm64-v8a on 64-bit devices, armeabi on 32-bit ones). With --arm64-only they are removed.
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
  8. 360 Jiagu packed games (--jiagu-bridge, --jiagu-dex, --baksmali): see "Packed games" in the README. After
     apktool has rebuilt the APK, `port_apk.py finish original.apk rebuilt.apk output.apk` puts the packed classes.dex
     and the ZIP comment back; then zipalign and sign.
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


def original_signature_hash(decoded):
    """Java's Signature.hashCode() for the first signer certificate of the original APK, or None."""
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
        h = 1
        for b in cert:  # java.util.Arrays.hashCode(byte[])
            h = (31 * h + (b - 256 if b > 127 else b)) & 0xffffffff
        return h - (1 << 32) if h > 0x7fffffff else h
    return None


def install_libraries(out, bridge, name, keep_32bit=True):
    lib = os.path.join(out, 'lib')
    arm64 = os.path.join(lib, 'arm64-v8a')
    os.makedirs(arm64, exist_ok=True)
    original = None
    for abi in ('armeabi-v7a', 'armeabi'):
        candidate = os.path.join(lib, abi, 'lib%s.so' % name)
        if os.path.exists(candidate):
            original = candidate
            break
    if original is None:
        sys.exit('no lib%s.so found under lib/armeabi{,-v7a}' % name)
    shutil.copy(bridge, os.path.join(arm64, 'lib%s.so' % name))
    shutil.copy(original, os.path.join(arm64, 'lib%s_a32.so' % name))
    if not keep_32bit:
        for abi in ('armeabi', 'armeabi-v7a'):
            shutil.rmtree(os.path.join(lib, abi), ignore_errors=True)


LOAD_LIBRARY = re.compile(
    r'(const-string v\d+, )"(?!%s")[A-Za-z0-9_]+"(\s*\n\s*invoke-static \{v\d+\}, '
    r'Ljava/lang/System;->loadLibrary\(Ljava/lang/String;\)V)')


def redirect_load_library(out, name):
    pattern = re.compile(LOAD_LIBRARY.pattern % re.escape(name))
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
            found.append(os.path.relpath(path, os.path.join(out, 'smali')).replace(os.sep, '/'))
    return found


def pin_service_intents(out, package, files):
    """Adds Intent.setPackage(package) after every `new Intent(String)` in service-starting methods of `files`."""
    changed = 0
    for rel in files:
        path = os.path.join(out, 'smali', *rel.split('/'))
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


def safe_ids_method(method, with_slot):
    return SAFE_IDS_METHOD.format(
        helper=GUARDED_TELEPHONY[method] + ('Slot' if with_slot else ''), tm=TELEPHONY[1:-1], method=method,
        param='I' if with_slot else '', regs='p0, p1' if with_slot else 'p0')


def guard_identifier_calls(out):
    """Rewrites the calls and adds the helper class. Returns the number of calls rewritten."""
    used = set()  # (method, with_slot) pairs that need a helper

    def rewrite(m):
        used.add((m.group(2), bool(m.group(3))))
        return 'invoke-static {%s}, L%s;->%s%s(L%s;%s)Ljava/lang/String;' % (
            m.group(1), SAFE_IDS_CLASS, GUARDED_TELEPHONY[m.group(2)], 'Slot' if m.group(3) else '',
            TELEPHONY[1:-1], m.group(3))

    count = 0
    for path in smali_files(out):
        text = read(path)
        if TELEPHONY + '->' not in text:
            continue
        new, n = TELEPHONY_CALL.subn(rewrite, text)
        if n:
            write(path, new)
            count += n
    if used:
        path = os.path.join(smali_dirs(out)[-1], SAFE_IDS_CLASS + '.smali')  # last dex: the first may be a packed stub
        os.makedirs(os.path.dirname(path), exist_ok=True)
        header = '.class public L%s;\n.super Ljava/lang/Object;\n\n' % SAFE_IDS_CLASS
        write(path, header + '\n'.join(safe_ids_method(method, slot) for method, slot in sorted(used)))
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


def stub_foreign_natives(out, keep=('com/popcap/', 'com/stub/', 'com/qihoo', 'com/common/busi/CustomView')):
    """Native methods of SDK classes live in 32-bit libraries the bridge cannot load, so calling them throws
    UnsatisfiedLinkError (often on a worker thread, which kills the process). They are turned into methods that return
    0 / null / void. Classes under the `keep` prefixes (the game itself and the packer stub) are left alone: their
    natives are provided by the game library or the packer. Returns the number of methods stubbed."""
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
    subprocess.run(['java', '-jar', baksmali, 'd', dex, '-o', os.path.join(out, 'smali_classes%d' % number)], check=True)


def install_jiagu(out, bridge):
    target = os.path.join(out, 'lib', 'arm64-v8a')
    os.makedirs(target, exist_ok=True)
    shutil.copy(os.path.join(out, 'assets', 'libjiagu.so'), os.path.join(target, 'libjiagu_a32.so'))
    shutil.copy(bridge, os.path.join(out, 'assets', 'libjiagu_a64.so'))


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


def fix_styles(out):
    path = os.path.join(out, 'res', 'values', 'styles.xml')
    if not os.path.exists(path):
        return
    text = read(path)
    text = text.replace('parent="@android:attr/buttonStyleSmall"', 'parent="@android:style/Widget.Button.Small"')
    text = text.replace('parent="@android:attr/buttonStyle"', 'parent="@android:style/Widget.Button"')
    write(path, text)


def finish(original, rebuilt, output):
    """Puts the original classes.dex and ZIP comment of a packed APK back into the APK that apktool rebuilt.

    A packer such as 360 Jiagu hides its payload inside classes.dex and keeps data in the ZIP comment. apktool rebuilds
    classes.dex from smali, which drops the payload, and the comment is lost too, so the packer cannot find what it
    needs at start-up. Run this before zipalign and signing: the signature covers both."""
    with zipfile.ZipFile(original) as source:
        packed_dex = source.read('classes.dex')
        comment = source.comment
    with zipfile.ZipFile(rebuilt) as source, zipfile.ZipFile(output, 'w') as destination:
        for info in source.infolist():
            data = packed_dex if info.filename == 'classes.dex' else source.read(info)
            destination.writestr(info, data, compress_type=info.compress_type)
        destination.comment = comment
    print('kept the packed classes.dex (%d bytes) and a %d-byte ZIP comment' % (len(packed_dex), len(comment)))


def main():
    if len(sys.argv) == 5 and sys.argv[1] == 'finish':  # port_apk.py finish original.apk rebuilt.apk output.apk
        finish(*sys.argv[2:])
        return
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('decoded', help='apktool-decoded APK directory')
    parser.add_argument('bridge', help='the built arm64 bridge, named lib<name>.so')
    parser.add_argument('out', help='output directory (created or updated)')
    parser.add_argument('--package', help="app package for service-intent pinning (default: from the manifest)")
    parser.add_argument('--pin-intents', nargs='*', default=[], metavar='SMALI',
                        help='smali files (relative to smali/) whose implicit service intents are pinned to the app')
    parser.add_argument('--jiagu-bridge', metavar='LIB', help='bridge built with -DLIBNAME=jiagu: runs the 360 Jiagu stub')
    parser.add_argument('--jiagu-dex', metavar='DEX', help='decrypted dex of a 360 Jiagu app, added as an extra dex')
    parser.add_argument('--baksmali', metavar='JAR', help='baksmali jar, needed with --jiagu-dex')
    parser.add_argument('--arm64-only', action='store_true',
                        help='drop the 32-bit libraries (default: keep them so the APK also runs natively on 32-bit ARM)')
    args = parser.parse_args()

    base = os.path.basename(args.bridge)
    if not (base.startswith('lib') and base.endswith('.so')):
        sys.exit('the bridge file must be named lib<name>.so')
    name = base[3:-3]

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
    install_libraries(args.out, args.bridge, name, keep_32bit=not args.arm64_only)
    print('loadLibrary calls redirected to "%s": %d' % (name, redirect_load_library(args.out, name)))
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
        add_unpacked_dex(args.out, args.jiagu_dex, args.baksmali)
    if args.jiagu_bridge:
        install_jiagu(args.out, args.jiagu_bridge)
    guarded, skipped = guard_unicom_wrapper(args.out), skip_talkweb_init(args.out)
    if guarded or skipped:
        print('Talkweb/Unicom SDK patches: %d Unicom call(s) guarded, Talkweb init %s'
              % (guarded, 'skipped' if skipped else 'left alone'))
    print('SDK native methods stubbed: %d' % stub_foreign_natives(args.out))
    print('telephony identifier calls guarded: %d' % guard_identifier_calls(args.out))
    print('ported ->', args.out)


if __name__ == '__main__':
    main()
