#!/usr/bin/env python3
"""One command from a game APK to a ready APK.

    python tools/build_apk.py game.apk [-o out.apk] [--server 192.168.1.10] [--install]

It looks at the APK and picks the route:

  bridge   32-bit ARM game, not packed (Lenovo 1.7.x). The bridge is built (or taken from the cache), the APK is
           decoded, ported, rebuilt, aligned and signed.
  jiagu    32-bit ARM game packed with 360 Jiagu (Lenovo 2.x). The packer library is run on a Linux host (WSL on
           Windows) to get its dex files, a game bridge and a jiagu bridge are built, the APK is ported with
           --jiagu-bridge/--jiagu-dex, rebuilt, finished, aligned and signed.
  native   the APK already carries 64-bit libraries (4.x). Nothing is ported; only the server patch applies.

--server then points the result at a private server with the server repo's patch_apk.py (addresses in the native
libraries and SERVERCONFIG, plain-http allowance, signature with the server's key). --install pushes the result
with adb install -r. --route overrides the choice.

Bridges are built with the Android NDK (ANDROID_NDK / ANDROID_NDK_HOME / under ANDROID_HOME/ndk), cmake and ninja
and cached in ~/.a32bridge/; a cached bridge is rebuilt when the sources under src/ are newer or --rebuild-bridges
is given. The library name is the game's largest 32-bit library (--lib-name overrides). The bridge answers the
game's signature checks with the bridge default; --sig-hash sets another value. Defaults match the builds made so far:
free shop and bridge log on, x86_64 bridge included, and for jiagu games --unhollow and --dex-in-lib on; each has a
--no-... switch.

Tools (PATH, or the variables named): java + keytool (PATH or JAVA_HOME), apktool (PATH or APKTOOL=<jar>),
zipalign + apksigner (PATH or ANDROID_HOME build-tools), adb for --install, WSL for the jiagu route on Windows.
Signing uses ~/.a32bridge/release.jks (alias dbg, password android) when that file exists, the key the published ports
are signed with; otherwise a debug keystore is generated in ~/.a32bridge/ (alias a32bridge, password android).
"""
import argparse
import datetime
import glob
import hashlib
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

REPO = pathlib.Path(__file__).resolve().parent.parent
TOOLS = REPO / 'tools'
HOME = pathlib.Path.home() / '.a32bridge'
IS_WINDOWS = os.name == 'nt'
KEY_ALIAS, KEY_PASSWORD = 'a32bridge', 'android'
RELEASE_ALIAS = 'dbg'  # alias inside ~/.a32bridge/release.jks, the key the published ports are signed with
signing = {'alias': KEY_ALIAS}
SKIP_LIBS = {'libBugly', 'libcasdkjni', 'libcmcc_haze', 'libcmcc_rusteze', 'libDexHelper_mmb', 'libegamepay_dr2',
             'libentryexstd', 'libidentifyapp', 'libmegjb', 'libme_unipay', 'libumc-auth', 'libjiagu', 'libTextUtil',
             'libcrypt_sign', 'libdexmmb', 'libmg20pbase', 'libsmsprotocol', 'libunipaychsec', 'libfuck'}


def fail(message):
    sys.exit('[!] ' + message)


def step(text):
    print('\n== ' + text)


def run(command, **kwargs):
    print('  $ ' + ' '.join(str(c) for c in command))
    result = subprocess.run([str(c) for c in command], **kwargs)
    if result.returncode != 0:
        fail('%s failed (exit %d)' % (pathlib.Path(str(command[0])).name, result.returncode))
    return result


# --- tools -------------------------------------------------------------------

def sdk_roots():
    return [pathlib.Path(os.environ[v]) for v in ('ANDROID_HOME', 'ANDROID_SDK_ROOT') if os.environ.get(v)]


def find_tool(name, sdk_globs=(), java_home=False):
    found = shutil.which(name)
    if found:
        return found
    if java_home and os.environ.get('JAVA_HOME'):
        for ext in ('', '.exe'):
            candidate = pathlib.Path(os.environ['JAVA_HOME']) / 'bin' / (name + ext)
            if candidate.is_file():
                return str(candidate)
    for root in sdk_roots():
        for pattern in sdk_globs:
            hits = sorted(glob.glob(str(root / pattern)), reverse=True)
            if hits:
                return hits[0]
    return None


def apktool_command():
    jar = os.environ.get('APKTOOL')
    if jar:
        return ['java', '-jar', jar], pathlib.Path(jar)
    found = shutil.which('apktool')
    if not found:
        fail('apktool is not on PATH; install it or set APKTOOL to apktool.jar')
    jar_next_to = pathlib.Path(found).with_name('apktool.jar')
    return [found], (jar_next_to if jar_next_to.is_file() else None)


def find_ndk():
    for variable in ('ANDROID_NDK', 'ANDROID_NDK_HOME', 'ANDROID_NDK_ROOT'):
        if os.environ.get(variable) and (pathlib.Path(os.environ[variable]) / 'build' / 'cmake').is_dir():
            return pathlib.Path(os.environ[variable])
    for root in sdk_roots():
        hits = sorted(glob.glob(str(root / 'ndk' / '*' / 'build' / 'cmake')), reverse=True)
        if hits:
            return pathlib.Path(hits[0]).parent.parent
    fail('Android NDK not found: set ANDROID_NDK (or ANDROID_HOME with an ndk folder)')


class Tools:
    def __init__(self):
        self.java = find_tool('java', java_home=True) or fail('java not found (PATH or JAVA_HOME)')
        self.keytool = find_tool('keytool', java_home=True) or fail('keytool not found (PATH or JAVA_HOME)')
        self.apktool, self.apktool_jar = apktool_command()
        self.zipalign = find_tool('zipalign', ('build-tools/*/zipalign', 'build-tools/*/zipalign.exe')) \
            or fail('zipalign not found (PATH or ANDROID_HOME build-tools)')
        self.apksigner = find_tool('apksigner', ('build-tools/*/apksigner', 'build-tools/*/apksigner.bat')) \
            or fail('apksigner not found (PATH or ANDROID_HOME build-tools)')
        self.shell_signer = IS_WINDOWS and self.apksigner.lower().endswith('.bat')


# --- APK facts ---------------------------------------------------------------

def analyze(apk):
    with zipfile.ZipFile(apk) as z:
        names = z.namelist()
        sizes = {n: z.getinfo(n).file_size for n in names}
    abis = sorted({n.split('/')[1] for n in names if n.startswith('lib/') and n.endswith('.so')})
    packed = any('libjiagu' in n for n in names)
    arm32 = [n for n in names if re.match(r'lib/armeabi(-v7a)?/[^/]+\.so$', n)]
    native64 = [n for n in names if re.match(r'lib/(arm64-v8a|x86_64)/[^/]+\.so$', n)]
    candidates = sorted(((sizes[n], n) for n in arm32 if pathlib.Path(n).stem not in SKIP_LIBS), reverse=True)
    lib_name = pathlib.Path(candidates[0][1]).stem[3:] if candidates else None
    has_64bit_game = lib_name is not None and any(
        re.match(r'lib/(arm64-v8a|x86_64)/lib%s\.so$' % re.escape(lib_name), n) and sizes[n] > 1 << 20 for n in names)
    if packed:
        route = 'jiagu'
    elif has_64bit_game or not arm32:
        route = 'native'
    else:
        route = 'bridge'
    return {'route': route, 'abis': abis, 'lib_name': lib_name, 'names': names}


def read_version(decoded):
    text = (decoded / 'apktool.yml').read_text(encoding='utf8', errors='replace')
    code = re.search(r"versionCode:\s*'?(\d+)", text)
    name = re.search(r"versionName:\s*'?([^'\n]+)", text)
    manifest = (decoded / 'AndroidManifest.xml').read_text(encoding='utf8', errors='replace')
    package = re.search(r'package="([^"]+)"', manifest)
    return {'code': int(code.group(1)) if code else 0, 'name': name.group(1).strip() if name else '0',
            'package': package.group(1) if package else ''}


# --- bridges -----------------------------------------------------------------

def newest_source():
    files = list((REPO / 'src').rglob('*')) + [REPO / 'CMakeLists.txt']
    return max(f.stat().st_mtime for f in files if f.is_file())


def build_bridge(name, abi, options, rebuild):
    flags = []
    if options.get('log'):
        flags.append('log')
    if options.get('free_shop'):
        flags.append('shop')
    if options.get('sig_hash'):
        flags.append('sig' + str(options['sig_hash']))
    trace = bool(os.environ.get('A32_TRACE_BUILD'))
    if trace:
        flags.append('trace')
    key = '-'.join([name, abi] + flags)
    build_dir = HOME / 'build' / key
    product = build_dir / ('lib%s.so' % name)
    if product.is_file() and not rebuild and product.stat().st_mtime >= newest_source():
        print('  bridge %s: cached' % key)
        return product

    ndk = find_ndk()
    print('  building bridge %s' % key)
    command = ['cmake', '-S', REPO, '-B', build_dir, '-G', 'Ninja',
               '-DCMAKE_TOOLCHAIN_FILE=' + str(ndk / 'build' / 'cmake' / 'android.toolchain.cmake'),
               '-DANDROID_ABI=' + abi, '-DANDROID_PLATFORM=android-24', '-DCMAKE_BUILD_TYPE=Release',
               '-DLIBNAME=' + name]
    ninja = shutil.which('ninja')
    if ninja:
        command.append('-DCMAKE_MAKE_PROGRAM=' + ninja)
    if options.get('sig_hash'):
        command.append('-DORIG_SIG_HASH=%s' % options['sig_hash'])
    command.append('-DA32_LOG=%s' % ('ON' if options.get('log') else 'OFF'))
    command.append('-DA32_FREE_SHOP=%s' % ('ON' if options.get('free_shop') else 'OFF'))
    command.append('-DA32_TRACE=%s' % ('ON' if trace else 'OFF'))
    if trace:
        command.append('-DCMAKE_CXX_FLAGS=-DA32_TRACE_LIMIT=%s' % os.environ.get('A32_TRACE_LIMIT', '1500'))
    run(command, stdout=subprocess.DEVNULL)
    run(['cmake', '--build', build_dir, '-j', str(os.cpu_count() or 4)])
    if not product.is_file():
        fail('the bridge build produced no %s' % product.name)
    return product


def build_stub(abi):
    """The empty library tools/mod_stub.c, placed under the names of the 32-bit SDK libraries."""
    out = HOME / 'stubs' / abi / 'libstub.so'
    source = TOOLS / 'mod_stub.c'
    if out.is_file() and out.stat().st_mtime >= source.stat().st_mtime:
        return out
    triple = {'arm64-v8a': 'aarch64-linux-android', 'x86_64': 'x86_64-linux-android'}[abi]
    bins = sorted(glob.glob(str(find_ndk() / 'toolchains' / 'llvm' / 'prebuilt' / '*' / 'bin')))
    if not bins:
        fail('NDK toolchain not found')
    clang = pathlib.Path(bins[0]) / ('%s24-clang%s' % (triple, '.cmd' if IS_WINDOWS else ''))
    out.parent.mkdir(parents=True, exist_ok=True)
    run([clang, '-shared', '-fPIC', '-O2', '-Wl,-z,max-page-size=16384', '-o', out, source])
    return out


# --- signing -----------------------------------------------------------------

def ensure_keystore(path, tools):
    if path.is_file():
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    print('  creating debug keystore ' + str(path))
    run([tools.keytool, '-genkeypair', '-keystore', path, '-alias', KEY_ALIAS, '-keyalg', 'RSA', '-keysize', '2048',
         '-validity', '10000', '-storepass', KEY_PASSWORD, '-keypass', KEY_PASSWORD, '-dname', 'CN=a32bridge debug'],
        stdout=subprocess.DEVNULL)


def align_and_sign(unsigned, output, keystore, tools, work):
    aligned = work / 'aligned.apk'
    run([tools.zipalign, '-f', '-p', '4', unsigned, aligned])
    output.parent.mkdir(parents=True, exist_ok=True)
    run([tools.apksigner, 'sign', '--v4-signing-enabled', 'false', '--ks', keystore, '--ks-key-alias', signing['alias'], '--ks-pass', 'pass:' + KEY_PASSWORD,
         '--key-pass', 'pass:' + KEY_PASSWORD, '--out', output, aligned], shell=tools.shell_signer)
    run([tools.apksigner, 'verify', output], shell=tools.shell_signer)


# --- the jiagu unpacker (Linux host, WSL on Windows) ---------------------------

def wsl_path(path):
    return subprocess.run(['wsl', '-e', 'wslpath', '-u', str(path)], capture_output=True, text=True).stdout.strip()


def unpack_jiagu(apk, info, version, work):
    """Runs the packer library on a Linux host and returns the decrypted dex files it hands over."""
    out = work / 'jiagu'
    out.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(apk) as z:
        entry = next((n for n in info['names'] if n.endswith('libjiagu.so') and n.startswith(('assets/', 'lib/armeabi'))), None)
        if entry is None:
            fail('no libjiagu.so in the APK')
        (out / 'libjiagu.so').write_bytes(z.read(entry))

    package, vname, vcode = version['package'], version['name'], version['code']
    if IS_WINDOWS:
        if not shutil.which('wsl'):
            fail('the jiagu route needs WSL (or a Linux host) to run the packer library')
        lib, apkp, dest = wsl_path(out / 'libjiagu.so'), wsl_path(apk), wsl_path(out)
        runner = wsl_path(TOOLS / 'jiagu_run' / 'jr.sh')
        launch = ['wsl', '-e', 'bash', '-c']
    else:
        lib, apkp, dest = str(out / 'libjiagu.so'), str(apk), str(out)
        runner = str(TOOLS / 'jiagu_run' / 'jr.sh')
        launch = ['bash', '-c']

    script = (
        'set -e; W=/data/data/%(pkg)s; rm -rf "$W" 2>/dev/null || sudo -n rm -rf "$W"; '
        'mkdir -p "$W/files" 2>/dev/null || sudo -n mkdir -p "$W/files"; sudo -n chmod -R 777 /data 2>/dev/null || true; '
        'tr -d "\\r" < "%(jr)s" | sed "s|^here=.*|here=$(dirname %(jr)s)|" > "$HOME/.a32_jr.sh"; '
        'bash "$HOME/.a32_jr.sh" run "%(lib)s" "%(apk)s" "$W" -- --package %(pkg)s --version-name %(vname)s '
        '--version-code %(vcode)s || true; cp "$W"/classes_unpacked*.dex "%(dest)s"/'
    ) % {'pkg': package, 'jr': runner, 'lib': lib, 'apk': apkp, 'dest': dest, 'vname': vname, 'vcode': vcode}
    run(launch + [script])
    dexes = sorted(out.glob('classes_unpacked*.dex'))
    if not dexes:
        fail('the packer handed over no dex files (see the jiagu_run output above and JIAGU_HANDOFF.md)')
    print('  unpacked dex files: ' + ', '.join('%s (%d bytes)' % (d.name, d.stat().st_size) for d in dexes))
    return dexes


# --- steps shared by the bridge and jiagu routes ---------------------------------

def decode(apk, tools, work):
    decoded = work / 'decoded'
    run(tools.apktool + ['d', '-f', '-o', decoded, apk])
    return decoded


def rebuild(ported, tools, work):
    build = ported / 'build'
    if build.exists():
        shutil.rmtree(build, ignore_errors=True)
    unsigned = work / 'unsigned.apk'
    run(tools.apktool + ['b', '-api', '28', ported, '-o', unsigned])
    return unsigned


def port_command(decoded, bridge_arm, ported, args, extra):
    command = [sys.executable, TOOLS / 'port_apk.py', decoded, bridge_arm, ported]
    command += ['--stub', build_stub('arm64-v8a')]
    if args.x86_64:
        command += ['--bridge', args.bridges_x86[0], '--stub', build_stub('x86_64')]
    if args.arm64_only:
        command.append('--arm64-only')
    if args.pin_intents:
        command += ['--pin-intents'] + args.pin_intents
    return command + extra


TALKYUN_HOSTS = ('cert', 'filead', 'adapi', 'adh5', 'ablog', 'popcap', 'pvz2androidtga', 'twuser')


def redirect_channel_sdk(ported, server):
    """Point the channel SDK's vendor hosts (real-name check, minor limits, ad and log calls) at the server."""
    from urllib.parse import urlsplit
    url = server if '://' in server else 'http://' + server
    parts = urlsplit(url)
    base = '%s://%s' % (parts.scheme, parts.netloc)
    pattern = re.compile(r'"https?://(?:%s)\.talkyun\.com\.cn' % '|'.join(TALKYUN_HOSTS))
    hits = 0
    for smali_dir in sorted(ported.glob('smali*')):
        for path in smali_dir.rglob('*.smali'):
            text = path.read_text(encoding='utf8', errors='surrogateescape')
            if 'talkyun.com.cn' not in text:
                continue
            new, count = pattern.subn('"' + base, text)
            if count:
                path.write_text(new, encoding='utf8', errors='surrogateescape')
                hits += count
    print('  channel SDK hosts -> %s (%d literal(s))' % (base, hits))


HTTP_REDIRECT_REGEX = r'^https?://[^/]*(popcap\.com\.cn|talkyun\.com\.cn|ditwan\.cn)(:[0-9]+)?'


def redirect_http_requests(ported, server):
    """Make the game's HTTP transaction send every vendor-host request to the server, whatever the host length."""
    from urllib.parse import urlsplit
    parts = urlsplit(server if '://' in server else 'http://' + server)
    base = '%s://%s' % (parts.scheme, parts.netloc)
    gap = '\n\n'
    anchor = ('    new-instance v1, Ljava/net/URL;' + gap
              + '    invoke-direct {v1, p4}, Ljava/net/URL;-><init>(Ljava/lang/String;)V')
    regex = HTTP_REDIRECT_REGEX.replace('\\', '\\\\')
    patch = ('    const-string v1, "a32.logrelay"' + gap
             + '    const-string v2, "%s"' % base + gap
             + '    invoke-static {v1, v2}, Ljava/lang/System;->setProperty('
             + 'Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;' + gap
             + '    const-string v1, "%s"' % regex + gap
             + '    const-string v2, "%s"' % base + gap
             + '    invoke-virtual {p4, v1, v2}, Ljava/lang/String;->replaceFirst('
             + 'Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;' + gap
             + '    move-result-object p4' + gap)
    for smali_dir in sorted(ported.glob('smali*')):
        path = smali_dir / 'com' / 'popcap' / 'SexyAppFramework' / 'AndroidHttpTransaction.smali'
        if path.is_file():
            text = path.read_text(encoding='utf8')
            if anchor not in text:
                fail('AndroidHttpTransaction constructor layout changed')
            path.write_text(text.replace(anchor, patch + anchor, 1), encoding='utf8')
            print('  HTTP requests to vendor hosts -> %s' % base)
            return
    print('  AndroidHttpTransaction not found; HTTP redirect skipped')


def server_patch(apk, output, args):
    tools_dir = args.server_tools or os.environ.get('PVZ2C_SERVER_TOOLS')
    candidates = [pathlib.Path(tools_dir)] if tools_dir else []
    candidates += [REPO.parent / 'PvZ2C_Private_Server' / 'Server' / 'tools']
    patch = next((c / 'patch_apk.py' for c in candidates if (c / 'patch_apk.py').is_file()), None)
    if patch is None:
        fail('--server needs the server repo tools: give --server-tools <dir with patch_apk.py> or set PVZ2C_SERVER_TOOLS')
    command = [sys.executable, patch, '--server', args.server, '--apk', apk, '-o', output]
    if args.bypass:
        command += ['--bypass', args.bypass]
    if args.danger_room:
        command.append('--danger-room')
    run(command)


def main():
    argv = sys.argv[1:]
    extra = []
    if '--' in argv:
        split = argv.index('--')
        argv, extra = argv[:split], argv[split + 1:]

    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('apk', help='the original game APK')
    parser.add_argument('-o', '--output', help='the APK to write (default: <name>_built.apk next to the input)')
    parser.add_argument('--route', choices=('auto', 'bridge', 'jiagu', 'native'), default='auto')
    parser.add_argument('--server', help='point the result at this server (IP, host or URL) with patch_apk.py')
    parser.add_argument('--server-key', action='store_true',
                        help='keep the signature patch_apk.py writes (the server key) instead of signing with the bridge key again')
    parser.add_argument('--server-tools', help='folder holding the server repo patch_apk.py (or PVZ2C_SERVER_TOOLS)')
    parser.add_argument('--bypass', metavar='APK', help='with --server: an APK carrying a signature bypass')
    parser.add_argument('--danger-room', action=argparse.BooleanOptionalAction, default=True,
                        help='with --server: open the Danger Room (endless) dates of the client event table (default on)')
    parser.add_argument('--install', action='store_true', help='adb install -r the result')
    parser.add_argument('--lib-name', help='the game library name (default: the largest 32-bit library)')
    parser.add_argument('--x86_64', action=argparse.BooleanOptionalAction, default=True,
                        help='also build and ship the x86_64 bridge for x86 emulators (default on; --no-x86_64 for arm64 only)')
    parser.add_argument('--arm64-only', action='store_true', help='drop the 32-bit libraries from the APK')
    parser.add_argument('--sig-hash', type=int, help='ORIG_SIG_HASH for the bridges (default: the bridge default)')
    parser.add_argument('--bridge-log', action=argparse.BooleanOptionalAction, default=True,
                        help='build the bridges with A32_LOG (default on)')
    parser.add_argument('--free-shop', action=argparse.BooleanOptionalAction, default=True,
                        help='build the game bridge with A32_FREE_SHOP: purchases complete without the payment SDK (default on)')
    parser.add_argument('--rebuild-bridges', action='store_true')
    parser.add_argument('--pin-intents', nargs='*', default=[], metavar='SMALI')
    parser.add_argument('--unhollow', action=argparse.BooleanOptionalAction, default=True,
                        help='jiagu: stub the hollowed Activity.onCreate natives (default on)')
    parser.add_argument('--dex-in-lib', action=argparse.BooleanOptionalAction, default=True,
                        help='jiagu: ship the extra dex files as lib<abi>/liba32dex<N>.so (default on; needs Android 8+)')
    parser.add_argument('--baksmali', metavar='JAR', help='jiagu: baksmali jar (default: the one inside apktool.jar)')
    parser.add_argument('--keystore', help='signing keystore (default ~/.a32bridge/release.jks when it exists, else a generated ~/.a32bridge/debug.jks)')
    parser.add_argument('--key-alias', help='alias of the signing key inside --keystore')
    parser.add_argument('--work', help='work folder (default: a temporary one)')
    parser.add_argument('--keep-work', action='store_true')
    parser.add_argument('--date-stamp', action=argparse.BooleanOptionalAction, default=True,
                        help='append the build date (_YYYYMMDD) to the output file name (default on)')
    args = parser.parse_args(argv)

    apk = pathlib.Path(args.apk).resolve()
    if not zipfile.is_zipfile(apk):
        fail('%s is not an APK' % apk)
    info = analyze(apk)
    route = info['route'] if args.route == 'auto' else args.route
    lib_name = args.lib_name or info['lib_name']
    print('APK: %s\n  libraries: %s\n  route: %s%s' % (apk.name, ', '.join(info['abis']) or 'none', route,
                                                       '' if args.route == 'auto' else ' (chosen)'))

    output = pathlib.Path(args.output).resolve() if args.output else apk.with_name(apk.stem + '_built.apk')
    if args.date_stamp:
        output = output.with_name('%s_%s%s' % (output.stem, datetime.date.today().strftime('%Y%m%d'), output.suffix))
    if route == 'native':
        if not args.server:
            fail('this APK already carries 64-bit libraries: there is nothing to port. Give --server to patch it.')
        server_patch(apk, output, args)
        print('\nDone: %s' % output)
        return finish_install(output, args)

    if route in ('bridge', 'jiagu') and not lib_name:
        fail('no 32-bit game library found; give --lib-name')
    tools = Tools()
    release = HOME / 'release.jks'
    if args.keystore:
        keystore = pathlib.Path(args.keystore).resolve()
        signing['alias'] = args.key_alias or KEY_ALIAS
    elif release.is_file():
        keystore = release
        signing['alias'] = args.key_alias or RELEASE_ALIAS
    else:
        keystore = HOME / 'debug.jks'
    if not args.keystore and not release.is_file():
        ensure_keystore(keystore, tools)
    print('  signing key: %s (alias %s)' % (keystore, signing['alias']))
    work = pathlib.Path(args.work).resolve() if args.work else pathlib.Path(tempfile.mkdtemp(prefix='a32build_'))
    work.mkdir(parents=True, exist_ok=True)
    built = work / 'built.apk'
    options = {'log': args.bridge_log, 'free_shop': args.free_shop, 'sig_hash': args.sig_hash}
    try:
        step('bridges')
        bridge_arm = build_bridge(lib_name, 'arm64-v8a', options, args.rebuild_bridges)
        args.bridges_x86 = [build_bridge(lib_name, 'x86_64', options, args.rebuild_bridges)] if args.x86_64 else []

        step('decoding')
        decoded = decode(apk, tools, work)
        version = read_version(decoded)
        print('  %(package)s %(name)s (%(code)d)' % version)

        ported = work / 'ported'
        command_extra = list(extra)
        if route == 'jiagu':
            step('unpacking the packer (Linux host)')
            dexes = unpack_jiagu(apk, info, version, work)
            jiagu_arm = build_bridge('jiagu', 'arm64-v8a', {'log': args.bridge_log, 'sig_hash': args.sig_hash},
                                     args.rebuild_bridges)
            baksmali = args.baksmali or (str(tools.apktool_jar) if tools.apktool_jar else None) \
                or fail('jiagu needs --baksmali <jar> (or apktool.jar next to the apktool command)')
            command_extra += ['--jiagu-bridge', jiagu_arm, '--baksmali', baksmali]
            if args.x86_64:
                command_extra += ['--jiagu-bridge', build_bridge('jiagu', 'x86_64', {'log': args.bridge_log, 'sig_hash': args.sig_hash},
                                                                 args.rebuild_bridges)]
            for dex in dexes:
                command_extra += ['--jiagu-dex', dex]
            if args.unhollow:
                command_extra.append('--unhollow')

        step('porting')
        run(port_command(decoded, bridge_arm, ported, args, command_extra))
        if args.server:
            redirect_channel_sdk(ported, args.server)
            redirect_http_requests(ported, args.server)

        step('building')
        unsigned = rebuild(ported, tools, work)
        if route == 'jiagu':
            finished = work / 'finished.apk'
            run([sys.executable, TOOLS / 'port_apk.py', 'finish', apk, unsigned, finished]
                + (['--dex-in-lib'] if args.dex_in_lib else []))
            unsigned = finished

        step('aligning and signing')
        align_and_sign(unsigned, built, keystore, tools, work)

        if args.server:
            step('pointing at the server')
            if args.server_key:
                server_patch(built, output, args)
            else:
                patched = work / 'patched.apk'
                server_patch(built, patched, args)
                step('signing with the bridge key')
                output.parent.mkdir(parents=True, exist_ok=True)
                align_and_sign(patched, output, keystore, tools, work)
        else:
            output.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(built, output)
    finally:
        if not (args.work or args.keep_work):
            shutil.rmtree(work, ignore_errors=True)

    print('\nDone: %s (%.0f MB)' % (output, output.stat().st_size / 1048576))
    print('Uninstall the original game first: an APK signed by another key cannot update it.')
    return finish_install(output, args)


def finish_install(output, args):
    if args.install:
        adb = shutil.which('adb') or fail('adb is not on PATH')
        run([adb, 'install', '-r', output])
    return 0


if __name__ == '__main__':
    sys.exit(main())
