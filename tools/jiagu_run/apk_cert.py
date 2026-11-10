#!/usr/bin/env python3
"""Prints the DER of the first signing certificate of an APK as lowercase hex (v1 signature block, needs openssl).

jiagu_run uses it to answer PackageManager.getPackageInfo(pkg, GET_SIGNATURES): newer packers hash the certificate
and show an error dialog when it does not match the original app's.

    apk_cert.py <apk> [--subject]
"""
import base64
import os
import re
import subprocess
import sys
import zipfile


def main():
    if os.environ.get('JR_CERT_FILE'):  # a DER file instead of the APK's own certificate (re-signed APKs)
        der = open(os.environ['JR_CERT_FILE'], 'rb').read()
        return emit(der)
    with zipfile.ZipFile(sys.argv[1]) as apk:
        blocks = [n for n in apk.namelist() if re.fullmatch(r'META-INF/[^/]+\.(RSA|DSA|EC)', n, re.I)]
        if not blocks:
            sys.exit('no signature block in the APK')
        pkcs7 = apk.read(blocks[0])
    pem = subprocess.run(['openssl', 'pkcs7', '-inform', 'DER', '-print_certs'], input=pkcs7,
                         capture_output=True, check=True).stdout.decode()
    match = re.search(r'-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----', pem, re.S)
    if not match:
        sys.exit('no certificate found')
    emit(base64.b64decode(match.group(1)))


def emit(der):
    if len(sys.argv) > 2 and sys.argv[2] == '--subject':  # Java's X509Certificate.getSubjectDN().getName() form
        out = subprocess.run(['openssl', 'x509', '-inform', 'DER', '-noout', '-subject', '-nameopt',
                              'sname,sep_comma_plus_space'], input=der, capture_output=True, check=True).stdout.decode()
        print(out.strip().split('subject=', 1)[-1].strip())
    else:
        print(der.hex())


if __name__ == '__main__':
    main()
