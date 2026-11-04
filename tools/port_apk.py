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
"""
import argparse
import os
import re
import shutil
import sys


def read(path):
    with open(path, encoding='utf8') as f:
        return f.read()


def write(path, text):
    with open(path, 'w', encoding='utf8') as f:
        f.write(text)


def smali_files(root):
    for base, _, files in os.walk(os.path.join(root, 'smali')):
        for name in files:
            if name.endswith('.smali'):
                yield os.path.join(base, name)


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


def fix_styles(out):
    path = os.path.join(out, 'res', 'values', 'styles.xml')
    if not os.path.exists(path):
        return
    text = read(path)
    text = text.replace('parent="@android:attr/buttonStyleSmall"', 'parent="@android:style/Widget.Button.Small"')
    text = text.replace('parent="@android:attr/buttonStyle"', 'parent="@android:style/Widget.Button"')
    write(path, text)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('decoded', help='apktool-decoded APK directory')
    parser.add_argument('bridge', help='the built arm64 bridge, named lib<name>.so')
    parser.add_argument('out', help='output directory (created or updated)')
    parser.add_argument('--package', help="app package for service-intent pinning (default: from the manifest)")
    parser.add_argument('--pin-intents', nargs='*', default=[], metavar='SMALI',
                        help='smali files (relative to smali/) whose implicit service intents are pinned to the app')
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
    print('ported ->', args.out)


if __name__ == '__main__':
    main()
