#!/usr/bin/env python3
"""Disassembler for PS3 CgBinaryProgram (.vpo/.fpo) RSX microcode."""
import sys, struct

def u32(d, o): return struct.unpack_from('>I', d, o)[0]
def bits(v, lo, n): return (v >> lo) & ((1 << n) - 1)
def cstr(d, o): return d[o:d.index(b'\0', o)].decode('latin1') if o else ''

SW = 'xyzw'

def params(d):
    cnt, arr = u32(d, 12), u32(d, 16)
    out = []
    for i in range(cnt):
        b = arr + 48 * i
        typ, res, var, resIndex, name, dflt, emb, sem, dirn, pno, ref, shared = struct.unpack_from('>IIIiIIIIIiII', d, b)
        p = dict(type=typ, res=res, var=var, resIndex=resIndex, name=cstr(d, name), sem=cstr(d, sem),
                 dir=dirn, ref=ref, emb=[], default=None)
        if emb:
            n = u32(d, emb)
            p['emb'] = [u32(d, emb + 4 + 4 * k) for k in range(n)]
        if dflt:
            p['default'] = struct.unpack_from('>4f', d, dflt)
        out.append(p)
    return out

# ---------------------------------------------------------------- vertex
VEC = {0:'NOP',1:'MOV',2:'MUL',3:'ADD',4:'MAD',5:'DP3',6:'DPH',7:'DP4',8:'DST',9:'MIN',10:'MAX',11:'SLT',12:'SGE',
       13:'ARL',14:'FRC',15:'FLR',16:'SEQ',17:'SFL',18:'SGT',19:'SLE',20:'SNE',21:'STR',22:'SSG',25:'TXL'}
SCA = {0:'NOP',1:'MOV',2:'RCP',3:'RCC',4:'RSQ',5:'EXP',6:'LOG',7:'LIT',8:'BRA',9:'BRI',10:'CAL',11:'CLI',12:'RET',
       13:'LG2',14:'EX2',15:'SIN',16:'COS',17:'BRB',18:'CLB',19:'PSH',20:'POP'}
VIN = ['in_pos','in_weight','in_normal','in_col0','in_col1','in_fog','in_psize','in_7',
       'in_tc0','in_tc1','in_tc2','in_tc3','in_tc4','in_tc5','in_tc6','in_tc7']
VOUT = {0:'o_hpos',1:'o_col0',2:'o_col1',3:'o_bfc0',4:'o_bfc1',5:'o_fog',6:'o_psize',7:'o_tc0',8:'o_tc1',9:'o_tc2',
        10:'o_tc3',11:'o_tc4',12:'o_tc5',13:'o_tc6',14:'o_tc7',15:'o_tc8',16:'o_tc9'}

def vp_dis(d, ps):
    cnames = {}
    vin = {}
    for p in ps:
        if p['var'] == 4101 or p['emb'] == [] and p['res'] >= 3000:  # uniform-ish
            pass
        if p['dir'] == 4097 and p['var'] == 4101 and p['ref']:  # CG_IN, CG_VARYING? just record
            pass
    for p in ps:
        if p['resIndex'] >= 0 and p['res'] == 2178:  # CG_C
            cnames[p['resIndex']] = p['name']
    prog = u32(d, 20); usz = u32(d, 24); uc = u32(d, 28)
    lines = []
    for i in range(usz // 16):
        d0, d1, d2, d3 = struct.unpack_from('>4I', d, uc + 16 * i)
        src0 = (bits(d1, 0, 8) << 9) | bits(d2, 23, 9)
        src1 = bits(d2, 6, 17)
        src2 = (bits(d2, 0, 6) << 11) | bits(d3, 21, 11)
        input_src = bits(d1, 8, 4); const_src = bits(d1, 12, 10)
        vop = bits(d1, 22, 5); sop = bits(d1, 27, 5)
        absb = [bits(d0, 21, 1), bits(d0, 22, 1), bits(d0, 23, 1)]
        idx_const = bits(d3, 1, 1)
        def src(s, n):
            rt = bits(s, 0, 2); tmp = bits(s, 2, 6)
            sw = ''.join(SW[bits(s, 14 - 2 * k, 2)] for k in range(4))
            neg = bits(s, 16, 1)
            if rt == 1: r = f'R{tmp}'
            elif rt == 2: r = VIN[input_src]
            elif rt == 3:
                r = f'c[{const_src}{"+A0" if idx_const else ""}]'
                if const_src in cnames: r += '{' + cnames[const_src] + '}'
            else: r = f'?{rt}'
            if sw != 'xyzw': r += '.' + (sw[0] if len(set(sw)) == 1 else sw)
            if absb[n]: r = f'|{r}|'
            if neg: r = '-' + r
            return r
        S = [src(src0, 0), src(src1, 1), src(src2, 2)]
        out = []
        vmask = ''.join(c for c, b in zip(SW, [bits(d3, 16, 1), bits(d3, 15, 1), bits(d3, 14, 1), bits(d3, 13, 1)]) if b)
        smask = ''.join(c for c, b in zip(SW, [bits(d3, 20, 1), bits(d3, 19, 1), bits(d3, 18, 1), bits(d3, 17, 1)]) if b)
        dst_tmp = bits(d0, 15, 6); vec_result = bits(d0, 30, 1); dst = bits(d3, 2, 5); sca_tmp = bits(d3, 7, 6)
        sat = '_SAT' if bits(d0, 26, 1) else ''
        cond = ''
        if bits(d0, 13, 1):
            cond = f' (cc {["FL","LT","EQ","LE","GT","NE","GE","TR"][bits(d0,10,3)]})'
        if vop:
            dsts = []
            if dst_tmp != 0x3f: dsts.append(f'R{dst_tmp}')
            if vec_result and dst != 0x1f: dsts.append(VOUT.get(dst, f'o{dst}'))
            if not dsts: dsts = ['CC'] if bits(d0, 14, 1) else ['_']
            dd = ','.join(x + '.' + vmask for x in dsts)
            n = {1: 1, 13: 1, 14: 1, 15: 1, 22: 1, 4: 3}.get(vop, 2)
            if vop == 3: S = [S[0], S[2], S[2]]
            if vop == 25: n = 1
            out.append(f'{VEC.get(vop, vop)}{sat} {dd}, ' + ', '.join(S[:n]) + cond)
        if sop:
            if sca_tmp != 0x3f: dd = f'R{sca_tmp}'
            elif dst != 0x1f: dd = VOUT.get(dst, f'o{dst}')
            else: dd = '_'
            if sop in (8, 9, 10, 11, 12, 17, 18):
                out.append(f'{SCA[sop]} {d2 & 0x3f}<<3|{bits(d3,29,3)} ; raw')
            else:
                out.append(f'{SCA.get(sop, sop)}{sat} {dd}.{smask}, {S[2]}' + cond)
        if not out: out = ['NOP']
        lines.append(f'{i:3d}: ' + ' ; '.join(out) + (' END' if d3 & 1 else ''))
    return lines

# ---------------------------------------------------------------- fragment
FOP = {0x00:'NOP',0x01:'MOV',0x02:'MUL',0x03:'ADD',0x04:'MAD',0x05:'DP3',0x06:'DP4',0x07:'DST',0x08:'MIN',0x09:'MAX',
       0x0A:'SLT',0x0B:'SGE',0x0C:'SLE',0x0D:'SGT',0x0E:'SNE',0x0F:'SEQ',0x10:'FRC',0x11:'FLR',0x12:'KIL',0x13:'PK4',
       0x14:'UP4',0x15:'DDX',0x16:'DDY',0x17:'TEX',0x18:'TXP',0x19:'TXD',0x1A:'RCP',0x1B:'RSQ',0x1C:'EX2',0x1D:'LG2',
       0x1E:'LIT',0x1F:'LRP',0x20:'STR',0x21:'SFL',0x22:'COS',0x23:'SIN',0x24:'PK2',0x25:'UP2',0x26:'POW',0x27:'PKB',
       0x28:'UPB',0x29:'PK16',0x2A:'UP16',0x2B:'BEM',0x2C:'PKG',0x2D:'UPG',0x2E:'DP2A',0x2F:'TXL',0x31:'TXB',
       0x33:'TEXBEM',0x34:'TXPBEM',0x35:'BEMLUM',0x36:'REFL',0x37:'TIMESWTEX',0x38:'DP2',0x39:'NRM',0x3A:'DIV',
       0x3B:'DIVSQ',0x3C:'LIF',0x3D:'FENCT',0x3E:'FENCB',0x40:'BRK',0x41:'CAL',0x42:'IFE',0x43:'LOOP',0x44:'REP',0x45:'RET'}
NSRC = {0x01:1,0x02:2,0x03:2,0x04:3,0x05:2,0x06:2,0x07:2,0x08:2,0x09:2,0x0A:2,0x0B:2,0x0C:2,0x0D:2,0x0E:2,0x0F:2,
        0x10:1,0x11:1,0x12:0,0x13:1,0x14:1,0x15:1,0x16:1,0x17:1,0x18:1,0x19:3,0x1A:1,0x1B:1,0x1C:1,0x1D:1,0x1E:1,
        0x1F:3,0x22:1,0x23:1,0x24:1,0x25:1,0x26:2,0x2E:3,0x2F:2,0x31:2,0x36:2,0x38:2,0x39:1,0x3A:2,0x3B:2,0x3C:1}
FIN = ['WPOS','COL0','COL1','FOGC','TC0','TC1','TC2','TC3','TC4','TC5','TC6','TC7','TC8','TC9','SSA','IN15']

def fp_swap(w): return ((w << 16) | (w >> 16)) & 0xffffffff

def fp_dis(d, ps):
    usz = u32(d, 24); uc = u32(d, 28)
    texnames = {}
    for p in ps:
        if 2048 <= p['res'] <= 2063:  # CG_TEXUNIT0..15
            texnames[p['res'] - 2048] = p['name']
    embmap = {}
    for p in ps:
        for off in p['emb']:
            embmap[off] = p['name']
    lines = []
    i = 0; pc = 0
    while i < usz:
        w = [fp_swap(u32(d, uc + i + 4 * k)) for k in range(4)]
        dst, s0, s1, s2 = w
        opc = bits(dst, 24, 6) | (bits(s1, 31, 1) << 6)
        has_const = False
        def src(s, n):
            nonlocal has_const
            rt = bits(s, 0, 2); idx = bits(s, 2, 6); h = bits(s, 8, 1)
            sw = ''.join(SW[bits(s, 9 + 2 * k, 2)] for k in range(4))
            neg = bits(s, 17, 1)
            ab = bits(s, 29, 1) if n == 0 else bits(s, 18, 1)
            if rt == 0: r = f'{"H" if h else "R"}{idx}'
            elif rt == 1:
                r = FIN[bits(dst, 13, 4)]
                if n == 2 and bits(s2, 30, 1): r += '[aL]'
            elif rt == 2:
                has_const = True
                r = 'CONST'
            else: r = f'?{rt}'
            if sw != 'xyzw': r += '.' + (sw[0] if len(set(sw)) == 1 else sw)
            if ab: r = f'|{r}|'
            if neg: r = '-' + r
            return r
        S = [src(s0, 0), src(s1, 1), src(s2, 2)]
        name = FOP.get(opc, f'op{opc:x}')
        mask = ''.join(c for c, b in zip(SW, [bits(dst, 9, 1), bits(dst, 10, 1), bits(dst, 11, 1), bits(dst, 12, 1)]) if b)
        prec = ['', '_H', '_X', '_?'][bits(dst, 22, 2)]
        sat = '_SAT' if bits(dst, 31, 1) else ''
        scale = {0:'',1:'_x2',2:'_x4',3:'_x8',5:'_d2',6:'_d4',7:'_d8'}.get(bits(s1, 28, 3), '')
        no_dest = bits(dst, 30, 1)
        dreg = ('H' if bits(dst, 7, 1) else 'R') + str(bits(dst, 1, 6))
        dd = ('CC' if no_dest else dreg) + ('.' + mask if mask != 'xyzw' else '')
        cc = ''
        lt, eq, gr = bits(s0, 18, 1), bits(s0, 19, 1), bits(s0, 20, 1)
        if not (lt and eq and gr):
            csw = ''.join(SW[bits(s0, 21 + 2 * k, 2)] for k in range(4))
            c = {(0,0,0):'FL',(1,0,0):'LT',(0,1,0):'EQ',(1,1,0):'LE',(0,0,1):'GT',(1,0,1):'NE',(0,1,1):'GE'}[(lt, eq, gr)]
            cc = f' ({c}{bits(s0,31,1)}.{csw})'
        setcc = '_C' if bits(dst, 8, 1) else ''
        n = NSRC.get(opc, 0)
        args = S[:n]
        if name in ('TEX', 'TXP', 'TXB', 'TXL', 'TXD'):
            t = bits(dst, 17, 4)
            args = args + [f'tex{t}{{{texnames.get(t, "?")}}}']
        if opc >= 0x40:
            txt = f'{name} raw={w[0]:08x} {w[1]:08x} {w[2]:08x} {w[3]:08x}'
        elif opc == 0:
            txt = 'NOP'
        else:
            txt = f'{name}{prec}{setcc}{sat}{scale} {dd}, ' + ', '.join(args) + cc
        lines.append(f'{pc:3d}: {txt}' + (' END' if bits(dst, 0, 1) else ''))
        i += 16; pc += 1
        if has_const:
            cv = struct.unpack('<4f', struct.pack('<4I', *[fp_swap(u32(d, uc + i + 4 * k)) for k in range(4)]))
            nm = embmap.get(i, None)
            lines.append(f'      CONST = {nm}' if nm else '      CONST = (' + ', '.join(f'{x:.7g}' for x in cv) + ')')
            i += 16
        if bits(dst, 0, 1): break
    return lines

def main(path):
    d = open(path, 'rb').read()
    prof = u32(d, 0)
    ps = params(d)
    print(f'# {path} profile={prof:#x}')
    for p in ps:
        if not p['ref']: continue
        extra = f' emb@{p["emb"]}' if p['emb'] else ''
        dv = f' default={p["default"]}' if p['default'] else ''
        print(f'#  param {p["name"]:<28} res={p["res"]} idx={p["resIndex"]} sem={p["sem"]} dir={p["dir"]} var={p["var"]}{extra}{dv}')
    if prof == 0x1b5b:
        print('\n'.join(vp_dis(d, ps)))
    else:
        print('\n'.join(fp_dis(d, ps)))

if __name__ == '__main__':
    for p in sys.argv[1:]:
        main(p); print()
