#!/usr/bin/env python3
"""Writes a minimal ARM32 ELF shared object that exports the given symbols (name=hexvalue or name), for packers that
parse /system/lib/libart.so from disk. usage: fake_libart.py out.so name[=hexvalue] ..."""
import struct, sys

def elf_hash(n):
    h = 0
    for c in n.encode():
        h = ((h << 4) + c) & 0xffffffff
        g = h & 0xf0000000
        h ^= g
        h ^= g >> 24
    return h

out, *specs = sys.argv[1:]
syms = []
for i, s in enumerate(specs):
    name, _, val = s.partition('=')
    syms.append((name, int(val, 16) if val else 0x1000 + 4 * i))
strtab = b'\0'
name_off = []
for n, _ in syms:
    name_off.append(len(strtab)); strtab += n.encode() + b'\0'
nsym = len(syms) + 1
dynsym = b'\0' * 16 + b''.join(struct.pack('<IIIBBH', name_off[i], v, 4, 0x12, 0, 1) for i, (n, v) in enumerate(syms))
nb = 67
buckets = [0] * nb; chains = [0] * nsym
for i, (n, _) in enumerate(syms, 1):
    b = elf_hash(n) % nb
    chains[i] = buckets[b]; buckets[b] = i
hashsec = struct.pack('<II', nb, nsym) + struct.pack('<%dI' % nb, *buckets) + struct.pack('<%dI' % nsym, *chains)
shstr = b'\0.hash\0.dynsym\0.dynstr\0.symtab\0.strtab\0.shstrtab\0.dynamic\0'
def so(n): return shstr.index(b'\0' + n.encode() + b'\0') + 1
body = b''
off = 52 + 32 * 2  # ehdr + 2 phdrs
def place(data, align=4):
    global body, off
    pad = (-off) % align
    body += b'\0' * pad; off += pad
    start = off; body += data; off += len(data)
    return start
o_hash = place(hashsec); o_dynsym = place(dynsym); o_dynstr = place(strtab); o_symtab = place(dynsym); o_strtab = place(strtab)
dyn = struct.pack('<II', 4, o_hash) + struct.pack('<II', 5, o_dynstr) + struct.pack('<II', 6, o_dynsym) + \
      struct.pack('<II', 10, len(strtab)) + struct.pack('<II', 11, 16) + struct.pack('<II', 0, 0)
o_dyn = place(dyn); o_shstr = place(shstr, 1)
end_load = off
shoff = off + ((-off) % 4)
body += b'\0' * (shoff - off)
def sh(name, typ, flags, addr, offs, size, link=0, info=0, align=4, ent=0):
    return struct.pack('<IIIIIIIIII', name, typ, flags, addr, offs, size, link, info, align, ent)
shs = [sh(0, 0, 0, 0, 0, 0, align=0),
       sh(so('.hash'), 5, 2, o_hash, o_hash, len(hashsec), 2, ent=4),
       sh(so('.dynsym'), 11, 2, o_dynsym, o_dynsym, len(dynsym), 3, 1, ent=16),
       sh(so('.dynstr'), 3, 2, o_dynstr, o_dynstr, len(strtab), align=1),
       sh(so('.symtab'), 2, 0, 0, o_symtab, len(dynsym), 5, 1, ent=16),
       sh(so('.strtab'), 3, 0, 0, o_strtab, len(strtab), align=1),
       sh(so('.dynamic'), 6, 3, o_dyn, o_dyn, len(dyn), 3, ent=8),
       sh(so('.shstrtab'), 3, 0, 0, o_shstr, len(shstr), align=1)]
ehdr = b'\x7fELF\1\1\1\0' + b'\0' * 8 + struct.pack('<HHIIIIIHHHHHH', 3, 40, 1, 0, 52, shoff, 0x05000000, 52, 32, 2, 40, len(shs), 7)
ph_load = struct.pack('<IIIIIIII', 1, 0, 0, 0, end_load, end_load, 5, 0x1000)
ph_dyn = struct.pack('<IIIIIIII', 2, o_dyn, o_dyn, o_dyn, len(dyn), len(dyn), 6, 4)
open(out, 'wb').write(ehdr + ph_load + ph_dyn + body + b''.join(shs))
