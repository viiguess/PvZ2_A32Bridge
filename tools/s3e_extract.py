#!/usr/bin/env python3
"""Extracts the game code of a Marmalade game (assets/*.s3e) into an ELF file a disassembler opens directly.

    python tools/s3e_extract.py game.apk [out.elf]        (or a .s3e file instead of the APK)
    python tools/s3e_extract.py --raw game.apk [out.bin]  (plain image; load it yourself at the printed address)

A .s3e is LZMA-compressed ("alone" header, raw stream; modded builds may use gzip). Unpacked it
starts with the header "XE3U"; word 5 is the file offset of the code+data image and word 13 the address it is linked
for (0x4A000000 in the PvZ builds), so no relocation is needed. The ELF says ARM, little endian, and maps the image at that address: Ghidra,
IDA or objdump pick everything up on their own. Calls into the loader go through a table the loader fills in at run
time, so they show up as indirect calls.
"""
import gzip
import lzma
import os
import struct
import sys
import zipfile


def read_s3e(path):
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as apk:
            names = [n for n in apk.namelist() if n.startswith('assets/') and n.endswith('.s3e')]
            if not names:
                sys.exit('no assets/*.s3e in %s' % path)
            return apk.read(names[0])
    with open(path, 'rb') as f:
        return f.read()


def unpack(data):
    if data[:4] == b'XE3U':  # stored uncompressed
        return data
    if data[:2] == b'\x1f\x8b':  # gzip (some modded builds)
        return gzip.decompress(data)
    # LZMA "alone": 5 property bytes, 64-bit unpacked size, stream. The streams end without an end marker, which the
    # "alone" decoder rejects, so decode the raw stream up to the stated size.
    props, dict_size, size = data[0], struct.unpack_from('<I', data, 1)[0], struct.unpack_from('<Q', data, 5)[0]
    decoder = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=[{
        'id': lzma.FILTER_LZMA1, 'lc': props % 9, 'lp': props // 9 % 5, 'pb': props // 45, 'dict_size': dict_size}])
    return decoder.decompress(data[13:], max_length=size)


def elf(image, base):
    """An ARM ELF32 executable mapping `image` at `base` (one RWX segment, one .text section)."""
    shstrtab = b'\0.text\0.shstrtab\0'
    ehdr_size, phdr_size, shdr_size = 52, 32, 40
    image_offset = 0x1000
    shstrtab_offset = image_offset + len(image)
    shdr_offset = (shstrtab_offset + len(shstrtab) + 3) & ~3
    header = struct.pack('<4sBBBBB7sHHIIIIIHHHHHH', b'\x7fELF', 1, 1, 1, 0, 0, b'\0' * 7,
                         2, 40, 1, base, ehdr_size, shdr_offset,  # ET_EXEC, EM_ARM, entry = image start
                         0x05000000,  # EABI version 5
                         ehdr_size, phdr_size, 1, shdr_size, 3, 2)
    program = struct.pack('<8I', 1, image_offset, base, base, len(image), len(image), 7, 0x1000)  # PT_LOAD, RWX
    sections = (struct.pack('<10I', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) +
                struct.pack('<10I', 1, 1, 7, base, image_offset, len(image), 0, 0, 4, 0) +  # .text: PROGBITS, AWX
                struct.pack('<10I', 7, 3, 0, 0, shstrtab_offset, len(shstrtab), 0, 0, 1, 0))  # .shstrtab
    out = bytearray(header + program)
    out += b'\0' * (image_offset - len(out)) + image + shstrtab
    out += b'\0' * (shdr_offset - len(out)) + sections
    return bytes(out)


def main():
    args = sys.argv[1:]
    raw = '--raw' in args
    args = [a for a in args if a != '--raw']
    if len(args) not in (1, 2):
        sys.exit(__doc__)
    unpacked = unpack(read_s3e(args[0]))
    if unpacked[:4] != b'XE3U':
        sys.exit('not a Marmalade .s3e image (magic %r)' % unpacked[:4])
    header = struct.unpack_from('<16I', unpacked, 0)
    offset, base = header[5], header[13]
    image = unpacked[offset:]
    out = args[1] if len(args) == 2 else os.path.splitext(args[0])[0] + ('.bin' if raw else '.elf')
    with open(out, 'wb') as f:
        f.write(image if raw else elf(image, base))
    print('%s: %d bytes of code and data at 0x%08X' % (out, len(image), base))
    if raw:
        print('load as raw binary: ARM:LE:32:v5t, base address %08X' % base)


if __name__ == '__main__':
    main()
