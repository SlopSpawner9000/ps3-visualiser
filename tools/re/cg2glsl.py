#!/usr/bin/env python3
"""Reference translator: PS3 CgBinaryProgram (.vpo/.fpo) -> GLSL 330."""
import sys, struct
sys.path.insert(0, __file__.rsplit('/', 1)[0])
from cgdis import params, u32, bits, fp_swap, cstr

SW = 'xyzw'

def swz(s):
    return ''.join(SW[bits(s, 14 - 2 * k, 2)] for k in range(4))

def vp_glsl(d, ps):
    cnames = {p['resIndex']: p['name'] for p in ps if p['res'] == 2178 and p['resIndex'] >= 0}
    usz = u32(d, 24); uc = u32(d, 28)
    L = []
    outs = set(); ins = set(); temps = set()
    VIN = {0: 'v_pos', 2: 'v_nrm', 3: 'v_col0', 4: 'v_col1', 8: 'v_tc0', 9: 'v_tc1', 10: 'v_tc2', 11: 'v_tc3'}
    def out_name(i):
        return {0: 'gl_Position', 1: 'vo_col0', 2: 'vo_col1', 5: 'vo_fog'}.get(i, f'vo_tc{i-7}' if 7 <= i <= 16 else f'vo_{i}')
    for i in range(usz // 16):
        d0, d1, d2, d3 = struct.unpack_from('>4I', d, uc + 16 * i)
        src = [(bits(d1, 0, 8) << 9) | bits(d2, 23, 9), bits(d2, 6, 17), (bits(d2, 0, 6) << 11) | bits(d3, 21, 11)]
        inp = bits(d1, 8, 4); cs = bits(d1, 12, 10)
        vop = bits(d1, 22, 5); sop = bits(d1, 27, 5)
        absb = [bits(d0, 21, 1), bits(d0, 22, 1), bits(d0, 23, 1)]
        def S(n):
            s = src[n]; rt = bits(s, 0, 2)
            if rt == 1: r = f'R{bits(s,2,6)}'; temps.add(bits(s, 2, 6))
            elif rt == 2: r = VIN.get(inp, f'v_in{inp}'); ins.add(inp)
            elif rt == 3:
                r = f'vc[{cs}{" + A0.x" if bits(d3,1,1) else ""}]'
            else: r = 'vec4(0)'
            r = f'{r}.{swz(s)}'
            if absb[n]: r = f'abs({r})'
            if bits(s, 16, 1): r = f'(-{r})'
            return r
        vmask = ''.join(c for c, b in zip(SW, [bits(d3, 16, 1), bits(d3, 15, 1), bits(d3, 14, 1), bits(d3, 13, 1)]) if b)
        smask = ''.join(c for c, b in zip(SW, [bits(d3, 20, 1), bits(d3, 19, 1), bits(d3, 18, 1), bits(d3, 17, 1)]) if b)
        sat = bits(d0, 26, 1)
        cond_test = bits(d0, 13, 1)
        cond = bits(d0, 10, 3)
        csw = ''.join(SW[bits(d0, 8 - 2 * k, 2)] for k in range(4))
        upd = bits(d0, 14, 1) or bits(d0, 29, 1)
        dst_tmp = bits(d0, 15, 6); vec_res = bits(d0, 30, 1); dst = bits(d3, 2, 5); sca_tmp = bits(d3, 7, 6)
        def emit(expr, mask, targets):
            if sat: expr = f'clamp({expr}, 0.0, 1.0)'
            L.append(f'    t = {expr};')
            for tg in targets:
                if cond_test and cond != 7:
                    op = {0: 'false', 1: '<', 2: '==', 3: '<=', 4: '>', 5: '!=', 6: '>='}[cond]
                    for c in mask:
                        ci = csw['xyzw'.index(c)]
                        L.append(f'    if (CC.{ci} {op} 0.0) {tg}.{c} = t.{c};')
                else:
                    L.append(f'    {tg}.{mask} = t.{mask};')
            if upd:
                L.append(f'    CC.{mask} = t.{mask};')
        if vop:
            a, b, c = S(0), S(1), S(2)
            ex = {1: f'{a}', 2: f'{a} * {b}', 3: f'{a} + {c}', 4: f'{a} * {b} + {c}',
                  5: f'vec4(dot({a}.xyz, {b}.xyz))', 6: f'vec4(dot(vec4({a}.xyz, 1.0), {b}))', 7: f'vec4(dot({a}, {b}))',
                  8: f'vec4(1.0, {a}.y * {b}.y, {a}.z, {b}.w)', 9: f'min({a}, {b})', 10: f'max({a}, {b})',
                  11: f'vec4(lessThan({a}, {b}))', 12: f'vec4(greaterThanEqual({a}, {b}))',
                  14: f'fract({a})', 15: f'floor({a})', 16: f'vec4(equal({a}, {b}))', 17: 'vec4(0.0)',
                  18: f'vec4(greaterThan({a}, {b}))', 19: f'vec4(lessThanEqual({a}, {b}))',
                  20: f'vec4(notEqual({a}, {b}))', 21: 'vec4(1.0)', 22: f'sign({a})'}.get(vop)
            if vop == 13:
                L.append(f'    A0.{vmask} = ivec4(floor({a})).{vmask};')
            elif ex is None:
                L.append(f'    // unsupported vec op {vop}')
            else:
                tg = []
                if dst_tmp != 0x3f: tg.append(f'R{dst_tmp}'); temps.add(dst_tmp)
                if vec_res and dst != 0x1f: tg.append(out_name(dst)); outs.add(dst)
                if not tg and upd: tg = []
                emit(ex, vmask or 'xyzw', tg)
        if sop:
            c = S(2)
            ex = {1: c, 2: f'1.0 / {c}', 3: f'clamp(1.0 / {c}, 5.42101e-20, 1.884467e19)', 4: f'inversesqrt(abs({c}))',
                  5: f'exp2({c})', 6: f'log2({c})', 13: f'log2({c})', 14: f'exp2({c})', 15: f'sin({c})', 16: f'cos({c})',
                  7: f'vec4(1.0, max({c}.x, 0.0), ({c}.x > 0.0) ? exp2({c}.w * log2(max({c}.y, 1e-30))) : 0.0, 1.0)'}.get(sop)
            if ex is None:
                L.append(f'    // unsupported sca op {sop}')
            else:
                tg = []
                if sca_tmp != 0x3f: tg.append(f'R{sca_tmp}'); temps.add(sca_tmp)
                elif dst != 0x1f: tg.append(out_name(dst)); outs.add(dst)
                emit(ex, smask or 'xyzw', tg)
    H = ['#version 330 core', 'uniform vec4 vc[468];']
    for k, n in sorted(VIN.items()):
        if k in ins: H.append(f'layout(location = {k}) in vec4 {n};')
    for o in sorted(outs):
        if o != 0: H.append(f'out vec4 {out_name(o)};')
    H.append('void main() {')
    H.append('    vec4 t; vec4 CC = vec4(0.0); ivec4 A0 = ivec4(0);')
    for r in sorted(temps): H.append(f'    vec4 R{r} = vec4(0.0);')
    for o in sorted(outs):
        if o != 0: H.append(f'    {out_name(o)} = vec4(0.0);')
    return '\n'.join(H + L + ['}'])

FIN = ['gl_FragCoord', 'vo_col0', 'vo_col1', 'vo_fog'] + [f'vo_tc{i}' for i in range(10)] + ['vec4(gl_FrontFacing ? 1.0 : -1.0)']

def fp_glsl(d, ps):
    usz = u32(d, 24); uc = u32(d, 28); prog = u32(d, 20)
    outH0 = d[prog + 28]
    embmap = {off: p['name'] for p in ps for off in p['emb']}
    texs = {p['res'] - 2048: p['name'] for p in ps if 2048 <= p['res'] <= 2063}
    uniforms = []
    L = []; ins = set(); regs = set()
    i = 0
    while i < usz:
        w = [fp_swap(u32(d, uc + i + 4 * k)) for k in range(4)]
        dst, s0, s1, s2 = w
        opc = bits(dst, 24, 6) | (bits(s1, 31, 1) << 6)
        end = bits(dst, 0, 1)
        has_c = any(bits(s, 0, 2) == 2 for s in (s0, s1, s2))
        cexpr = None
        if has_c:
            co = i + 16
            nm = embmap.get(co)
            if nm:
                if nm not in uniforms: uniforms.append(nm)
                cexpr = nm
            else:
                cv = struct.unpack('<4f', struct.pack('<4I', *[fp_swap(u32(d, uc + co + 4 * k)) for k in range(4)]))
                cexpr = 'vec4(' + ', '.join(f'{x!r}' for x in cv) + ')'
        def S(s, n):
            rt = bits(s, 0, 2)
            if rt == 0:
                r = f'{"H" if bits(s,8,1) else "R"}{bits(s,2,6)}'; regs.add(r)
            elif rt == 1:
                a = bits(dst, 13, 4); ins.add(a); r = FIN[a]
            elif rt == 2: r = cexpr
            else: r = 'vec4(0.0)'
            sw = ''.join(SW[bits(s, 9 + 2 * k, 2)] for k in range(4))
            r = f'{r}.{sw}'
            if (bits(s, 29, 1) if n == 0 else bits(s, 18, 1)): r = f'abs({r})'
            if bits(s, 17, 1): r = f'(-{r})'
            return r
        a, b, c = S(s0, 0), S(s1, 1), S(s2, 2)
        tex = bits(dst, 17, 4)
        ex = {0x01: a, 0x02: f'{a} * {b}', 0x03: f'{a} + {b}', 0x04: f'{a} * {b} + {c}',
              0x05: f'vec4(dot({a}.xyz, {b}.xyz))', 0x06: f'vec4(dot({a}, {b}))',
              0x07: f'vec4(1.0, {a}.y * {b}.y, {a}.z, {b}.w)', 0x08: f'min({a}, {b})', 0x09: f'max({a}, {b})',
              0x0A: f'vec4(lessThan({a}, {b}))', 0x0B: f'vec4(greaterThanEqual({a}, {b}))',
              0x0C: f'vec4(lessThanEqual({a}, {b}))', 0x0D: f'vec4(greaterThan({a}, {b}))',
              0x0E: f'vec4(notEqual({a}, {b}))', 0x0F: f'vec4(equal({a}, {b}))',
              0x10: f'fract({a})', 0x11: f'floor({a})', 0x15: f'dFdx({a})', 0x16: f'dFdy({a})',
              0x17: f'texture(tex{tex}, {a}.xy)', 0x18: f'textureProj(tex{tex}, {a}.xyw)',
              0x2F: f'textureLod(tex{tex}, {a}.xy, {a}.w)', 0x31: f'texture(tex{tex}, {a}.xy, {a}.w)',
              0x19: f'textureGrad(tex{tex}, {a}.xy, {b}.xy, {c}.xy)',
              0x1A: f'vec4(1.0 / {a}.x)', 0x1B: f'vec4(inversesqrt(abs({a}.x)))', 0x1C: f'vec4(exp2({a}.x))',
              0x1D: f'vec4(log2({a}.x))', 0x1F: f'mix({c}, {b}, {a})', 0x20: 'vec4(1.0)', 0x21: 'vec4(0.0)',
              0x22: f'vec4(cos({a}.x))', 0x23: f'vec4(sin({a}.x))', 0x26: f'vec4(pow({a}.x, {b}.x))',
              0x38: f'vec4(dot({a}.xy, {b}.xy))', 0x39: f'vec4(normalize({a}.xyz), 0.0)',
              0x3A: f'{a} / {b}.x', 0x3B: f'{a} * inversesqrt({b}.x)', 0x2E: f'vec4(dot({a}.xy, {b}.xy) + {c}.z)',
              0x1E: f'vec4(1.0, max({a}.x, 0.0), ({a}.x > 0.0) ? pow(max({a}.y, 0.0), {a}.w) : 0.0, 1.0)'}.get(opc)
        mask = ''.join(ch for ch, bb in zip(SW, [bits(dst, 9, 1), bits(dst, 10, 1), bits(dst, 11, 1), bits(dst, 12, 1)]) if bb)
        lt, eq, gr = bits(s0, 18, 1), bits(s0, 19, 1), bits(s0, 20, 1)
        csw = ''.join(SW[bits(s0, 21 + 2 * k, 2)] for k in range(4))
        ccr = f'CC{bits(s0,31,1)}'
        condop = None if (lt and eq and gr) else {(0,0,0):'false',(1,0,0):'<',(0,1,0):'==',(1,1,0):'<=',(0,0,1):'>',(1,0,1):'!=',(0,1,1):'>='}[(lt, eq, gr)]
        if opc == 0x12:  # KIL
            if condop is None: L.append('    discard;')
            else:
                L.append(f'    if (any(bvec4(' + ', '.join(f'{ccr}.{csw[k]} {condop} 0.0' for k in range(4)) + '))) discard;')
        elif opc in (0x00, 0x3D, 0x3E):
            pass
        elif ex is None:
            L.append(f'    // unsupported fp op {opc:#x}')
        else:
            scale = {0: '', 1: ' * 2.0', 2: ' * 4.0', 3: ' * 8.0', 5: ' * 0.5', 6: ' * 0.25', 7: ' * 0.125'}.get(bits(s1, 28, 3), '')
            e = f'({ex}){scale}' if scale else ex
            if bits(dst, 31, 1): e = f'clamp({e}, 0.0, 1.0)'
            L.append(f'    t = {e};')
            no_dest = bits(dst, 30, 1)
            reg = f'{"H" if bits(dst,7,1) else "R"}{bits(dst,1,6)}'
            if not no_dest:
                regs.add(reg)
                if condop:
                    for ch in mask:
                        L.append(f'    if ({ccr}.{csw["xyzw".index(ch)]} {condop} 0.0) {reg}.{ch} = t.{ch};')
                else:
                    L.append(f'    {reg}.{mask} = t.{mask};')
            if bits(dst, 8, 1):
                L.append(f'    CC{bits(s1,30,1) if False else 0}.{mask} = t.{mask};')
        i += 16 + (16 if has_c else 0)
        if end: break
    H = ['#version 330 core']
    for k, n in sorted(texs.items()): H.append(f'uniform sampler2D tex{k}; // {n}')
    for u in uniforms: H.append(f'uniform vec4 {u};')
    for a in sorted(ins):
        if 1 <= a <= 13: H.append(f'in vec4 {FIN[a]};')
    H.append('out vec4 fragColor;')
    H.append('void main() {')
    H.append('    vec4 t; vec4 CC0 = vec4(0.0), CC1 = vec4(0.0);')
    for r in sorted(regs | {'R0', 'H0'}): H.append(f'    vec4 {r} = vec4(0.0);')
    L.append(f'    fragColor = {"H0" if outH0 else "R0"};')
    return '\n'.join(H + L + ['}'])

def main(p):
    d = open(p, 'rb').read()
    ps = params(d)
    print(f'// {p}')
    if u32(d, 0) == 0x1b5b:
        for x in ps:
            if x['res'] == 2178 and x['resIndex'] >= 0 and x['ref']:
                print(f'// vc[{x["resIndex"]}] = {x["name"]}' + (f' = {x["default"]}' if x['default'] else ''))
        print(vp_glsl(d, ps))
    else:
        print(fp_glsl(d, ps))

if __name__ == '__main__':
    for p in sys.argv[1:]: main(p); print()
