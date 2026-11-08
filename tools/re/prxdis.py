#!/usr/bin/env python3
"""Relocate a decrypted PS3 PRX and produce an annotated PPC disassembly."""
import sys, struct, subprocess, re, os

def u32(d, o): return struct.unpack_from('>I', d, o)[0]

class Prx:
    def __init__(self, path):
        self.path = path
        d = bytearray(open(path, 'rb').read())
        phoff = struct.unpack_from('>Q', d, 0x20)[0]; phnum = struct.unpack_from('>H', d, 0x38)[0]
        self.segs = []
        for i in range(phnum):
            t, fl, off, va, pa, fsz, msz, al = struct.unpack_from('>IIQQQQQQ', d, phoff + 56 * i)
            self.segs.append((t, off, va, pa, fsz, msz))
        self.d = d
        load = [s for s in self.segs if s[0] == 1]
        self.base = [s[2] for s in load]
        # relocations
        for t, off, va, pa, fsz, msz in self.segs:
            if t != 0x700000a4: continue
            for i in range(fsz // 24):
                roff, _, iv, ia, rt, ptr = struct.unpack_from('>QHBBIQ', d, off + 24 * i)
                val = (self.base[iv] if iv != 0xff else 0) + ptr
                fo = self.v2o(self.base[ia] + roff)
                if rt == 1: struct.pack_into('>I', d, fo, val & 0xffffffff)
                elif rt == 4: struct.pack_into('>H', d, fo, val & 0xffff)
                elif rt == 5: struct.pack_into('>H', d, fo, (val >> 16) & 0xffff)
                elif rt == 6: struct.pack_into('>H', d, fo, ((val + 0x8000) >> 16) & 0xffff)
                elif rt == 38: struct.pack_into('>Q', d, fo, val)
                elif rt == 57:  # ADDR16_LO_DS: keep the two low (opcode) bits
                    old = struct.unpack_from('>H', d, fo)[0]
                    struct.pack_into('>H', d, fo, (val & 0xfffc) | (old & 3))
                else: print('reloc type', rt, file=sys.stderr)
        mi = load[0][3]
        self.name = bytes(d[mi + 4:mi + 32]).split(b'\0')[0].decode()
        self.toc, es, ee, is_, ie = struct.unpack_from('>5I', d, mi + 32)
        self.imports = {}   # stub slot addr -> (lib, nid)
        o = is_
        while o < ie:
            b = self.v2o(o); size = d[b]
            nfunc, nvar = struct.unpack_from('>HH', d, b + 6)
            lname, fnids, fstubs = struct.unpack_from('>III', d, b + 16)
            ln = self.cstr(lname)
            for k in range(nfunc):
                self.imports[fstubs + 4 * k] = (ln, self.r32(fnids + 4 * k))
            o += size
        self.exports = {}
        o = es
        while o < ee:
            b = self.v2o(o); size = d[b]
            nfunc, nvar = struct.unpack_from('>HH', d, b + 6)
            lname, nids, addrs = struct.unpack_from('>III', d, b + 16)
            ln = self.cstr(lname) if lname else '_sys'
            for k in range(nfunc + nvar):
                self.exports[self.r32(nids + 4 * k)] = (ln, self.r32(addrs + 4 * k), k >= nfunc)
            o += size

    def v2o(self, a):
        for t, off, va, pa, fsz, msz in self.segs:
            if t == 1 and va <= a < va + fsz: return off + a - va
        return None
    def r32(self, a): return u32(self.d, self.v2o(a))
    def cstr(self, a):
        o = self.v2o(a)
        return bytes(self.d[o:self.d.index(b'\0', o)]).decode('latin1')
    def try_str(self, a):
        o = self.v2o(a)
        if o is None: return None
        e = self.d.find(b'\0', o, o + 200)
        if e <= o: return None
        s = bytes(self.d[o:e])
        if len(s) >= 2 and all(32 <= c < 127 or c in (9, 10) for c in s): return s.decode()
        return None
    def try_float(self, a):
        o = self.v2o(a)
        if o is None: return None
        return struct.unpack_from('>f', self.d, o)[0]

def disasm(prx, out):
    tmp = out + '.reloc.elf'
    open(tmp, 'wb').write(prx.d)
    txt = subprocess.run(['llvm-objdump', '-d', '--no-show-raw-insn', '--triple=powerpc64', tmp],
                         capture_output=True, text=True).stdout
    os.unlink(tmp)
    # find import stubs: lis r12,HI ; lwz r12,LO(r12) pattern
    lines = txt.splitlines()
    ins = []
    for l in lines:
        m = re.match(r'\s*([0-9a-f]+):\s+(\S+)\s*(.*)', l)
        if m: ins.append((int(m.group(1), 16), m.group(2), m.group(3)))
    stubs = {}
    for i in range(len(ins) - 3):
        a, op, args = ins[i]
        if op == 'li' and args.startswith('12, 0') and ins[i+1][1] == 'oris' and ins[i+2][1] == 'lwz':
            hi = int(ins[i+1][2].split(',')[2].strip(), 0)
            lo = int(re.match(r'12, (-?\d+)\(12\)', ins[i+2][2]).group(1))
            slot = ((hi << 16) + lo) & 0xffffffff
            if slot in prx.imports: stubs[a] = prx.imports[slot]
    funcs = {v[1]: f'{v[0]}_{k:08x}' for k, v in prx.exports.items() if not v[2]}
    # exported addrs are OPDs: resolve to code addr
    fn_code = {}
    for opd, nm in funcs.items():
        try: fn_code[prx.r32(opd)] = nm
        except Exception: pass
    # Also all OPDs in data segment (pairs code,toc where toc==prx.toc)
    seg1 = prx.base[1]
    o = prx.v2o(seg1); n = [s for s in prx.segs if s[0] == 1][1][4]
    opds = {}
    for k in range(0, n - 8, 4):
        c, t = struct.unpack_from('>II', prx.d, o + k)
        if t == prx.toc and c < seg1 and c % 4 == 0: opds[seg1 + k] = c
    res = []
    regs = {}
    for a, op, args in ins:
        if a in fn_code: res.append(f'\n==== {fn_code[a]}')
        if a in stubs: res.append(f'---- stub {stubs[a][0]}::{stubs[a][1]:08x}')
        note = ''
        m = re.match(r'(\d+), (-?\d+)\((\d+)\)', args)
        ml = re.match(r'(\d+), (-?\d+)$', args)
        if op == 'lis' and ml:
            regs[int(ml.group(1))] = (int(ml.group(2)) << 16) & 0xffffffff
            res.append(f'{a:6x}: {op:8s} {args}'); continue
        ma = re.match(r'(\d+), (\d+), (-?\d+)$', args)
        if op in ('addi', 'addis') and ma and int(ma.group(2)) in regs and int(ma.group(2)) != 2:
            v = (regs[int(ma.group(2))] + (int(ma.group(3)) << (16 if op == 'addis' else 0))) & 0xffffffff
            regs[int(ma.group(1))] = v
            s = prx.try_str(v); f = prx.try_float(v)
            note = f'= {v:#x}' + (f' "{s}"' if s else '') + (f' (opd->{opds[v]:#x})' if v in opds else '')
            res.append(f'{a:6x}: {op:8s} {args}   ; {note}'); continue
        if op in ('bl', 'b') and args:
            try:
                t = int(args.split()[0], 16)
                if t in stubs: note = f'{stubs[t][0]}::{stubs[t][1]:08x}'
                elif t in fn_code: note = fn_code[t]
            except ValueError: pass
        elif m:
            rd, imm, rb = int(m.group(1)), int(m.group(2)), int(m.group(3))
            if rb == 2:
                addr = (prx.toc + imm) & 0xffffffff
                if op in ('lwz', 'ld'):
                    try:
                        v = prx.r32(addr)
                        regs[rd] = v
                        s = prx.try_str(v)
                        note = f'toc[{addr:#x}] -> {v:#x}' + (f' "{s}"' if s else '')
                        if v in opds: note += f' OPD->code {opds[v]:#x}'
                    except Exception: pass
                elif op == 'addi':
                    regs[rd] = addr; note = f'= {addr:#x}'
            elif op in ('lfs', 'lfsu') and rb in regs:
                f = prx.try_float(regs[rb] + imm)
                if f is not None: note = f'[{regs[rb]+imm:#x}] = {f:g}'
            elif op == 'lfd' and rb in regs:
                o2 = prx.v2o(regs[rb] + imm)
                if o2 is not None: note = f'[{regs[rb]+imm:#x}] = {struct.unpack_from(">d", prx.d, o2)[0]:g}'
            elif op in ('lwz', 'lvx') and rb in regs:
                try:
                    v = prx.r32(regs[rb] + imm); s = prx.try_str(v)
                    note = f'[{regs[rb]+imm:#x}] -> {v:#x}' + (f' "{s}"' if s else '')
                    if op == 'lwz': regs[rd] = v
                    else:
                        o2 = prx.v2o(regs[rb] + imm); note = f'[{regs[rb]+imm:#x}] = ' + str([round(x,5) for x in struct.unpack_from('>4f', prx.d, o2)])
                except Exception: pass
                if rd in regs and op == 'lvx': pass
            elif op == 'addi' and rb in regs:
                v = regs[rb] + imm
                s = prx.try_str(v)
                regs[rd] = v
                note = f'= {v:#x}' + (f' "{s}"' if s else '')
            elif op.startswith('l') and rd in regs:
                regs.pop(rd, None)
        else:
            m2 = re.match(r'(\d+),', args)
            if m2 and op not in ('stw', 'std', 'stfs', 'cmpwi', 'cmplwi', 'cmpw') and not op.startswith('st'):
                regs.pop(int(m2.group(1)), None)
        if op in ('blr', 'bctr'): regs = {}
        res.append(f'{a:6x}: {op:8s} {args}' + (f'   ; {note}' if note else ''))
    open(out, 'w').write('\n'.join(res))
    return stubs, opds

if __name__ == '__main__':
    p = Prx(sys.argv[1])
    print(p.name, 'toc', hex(p.toc))
    stubs, opds = disasm(p, sys.argv[2])
    print('stubs', len(stubs), 'opds', len(opds))
    for k, v in p.exports.items(): print('export', hex(k), v[0], hex(v[1]), 'var' if v[2] else 'func')
