#include "cg.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <sstream>

namespace {

uint32_t be32(const Bytes &d, size_t o)
{
    if (o + 4 > d.size())
        return 0;
    return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3];
}

std::string cstr(const Bytes &d, size_t o)
{
    std::string s;
    if (!o)
        return s;
    while (o < d.size() && d[o])
        s += char(d[o++]);
    return s;
}

inline uint32_t bits(uint32_t v, int lo, int n) { return (v >> lo) & ((1u << n) - 1); }

// Fragment ucode words have their 16-bit halves swapped.
inline uint32_t fpSwap(uint32_t w) { return (w << 16) | (w >> 16); }

const char *const kSwz = "xyzw";

std::string floatLit(float f)
{
    if (std::isnan(f))
        return "0.0";
    if (std::isinf(f))
        return f > 0 ? "3.4e38" : "-3.4e38";
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.9g", double(f));
    std::string s = buf;
    if (s.find_first_of(".e") == std::string::npos)
        s += ".0";
    return s;
}

std::string sanitize(const std::string &n)
{
    std::string s;
    for (char c : n)
        s += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
    return s;
}

// CG enums used here.
constexpr uint32_t CG_C = 2178;           // vertex constant register bank
constexpr uint32_t CG_TEXUNIT0 = 2048;
constexpr uint32_t CG_SAMPLER3D = 1067;
constexpr uint32_t CG_PROFILE_VP = 0x1b5b;

const char *vpInputName(int i)
{
    static const char *names[16] = {"v_in0", "v_in1", "v_in2", "v_in3", "v_in4", "v_in5", "v_in6", "v_in7",
                                    "v_in8", "v_in9", "v_in10", "v_in11", "v_in12", "v_in13", "v_in14", "v_in15"};
    return names[i & 15];
}

std::string vpOutputName(int i)
{
    switch (i) {
    case 0: return "gl_Position";
    case 1: return "vo_col0";
    case 2: return "vo_col1";
    case 5: return "vo_fog";
    case 6: return "vo_tc9"; // (TEXCOORD9 shares the point size register; RPCS3 GLVertexProgram.cpp)
    default:
        if (i >= 7 && i <= 15)
            return "vo_tc" + std::to_string(i - 7);
        return "vo_out" + std::to_string(i);
    }
}

std::string fpInputName(int i)
{
    switch (i) {
    case 0: return "WPOS";
    case 1: return "vo_col0";
    case 2: return "vo_col1";
    case 3: return "vo_fog";
    case 14: return "FACE";
    default:
        if (i >= 4 && i <= 13)
            return "vo_tc" + std::to_string(i - 4);
        return "vec4(0.0)";
    }
}

const char *condOp(uint32_t c)
{
    // 0 FL, 1 LT, 2 EQ, 3 LE, 4 GT, 5 NE, 6 GE, 7 TR
    static const char *ops[8] = {nullptr, "<", "==", "<=", ">", "!=", ">=", nullptr};
    return ops[c & 7];
}

} // namespace

bool CgShader::parse(const Bytes &data, const std::string &lbl)
{
    data_ = data;
    label = lbl;
    if (data_.size() < 32)
        return false;
    const uint32_t profile = be32(data_, 0);
    isVertex = profile == CG_PROFILE_VP;
    const uint32_t count = be32(data_, 12), arr = be32(data_, 16);
    for (uint32_t i = 0; i < count; i++) {
        const size_t b = arr + 48 * i;
        if (b + 48 > data_.size())
            return false;
        CgParam p;
        p.type = be32(data_, b);
        p.res = be32(data_, b + 4);
        p.var = be32(data_, b + 8);
        p.resIndex = int32_t(be32(data_, b + 12));
        p.name = cstr(data_, be32(data_, b + 16));
        const uint32_t dflt = be32(data_, b + 20), emb = be32(data_, b + 24);
        p.semantic = cstr(data_, be32(data_, b + 28));
        p.dir = be32(data_, b + 32);
        p.referenced = be32(data_, b + 40) != 0;
        if (emb) {
            const uint32_t n = be32(data_, emb);
            for (uint32_t k = 0; k < n && k < 64; k++)
                p.embedded.push_back(be32(data_, emb + 4 + 4 * k));
        }
        if (dflt && dflt + 16 <= data_.size()) {
            p.hasDefault = true;
            for (int k = 0; k < 4; k++) {
                uint32_t w = be32(data_, dflt + 4 * k);
                std::memcpy(&p.def[k], &w, 4);
            }
        }
        params.push_back(std::move(p));
    }
    if (isVertex)
        translateVertex();
    else
        translateFragment();
    if (SDL_getenv("PS3VIS_DUMP_SHADERS"))
        SDL_Log("---- %s ----\n%s", label.c_str(), glsl.c_str());
    return !glsl.empty();
}

void CgShader::translateVertex()
{
    const uint32_t usize = be32(data_, 24), uc = be32(data_, 28);
    std::ostringstream body;
    std::set<int> temps, inputs, outputs;
    std::set<int> consts;
    bool relative = false;

    struct Insn {
        uint32_t d0, d1, d2, d3;
    };
    std::vector<Insn> code;
    for (uint32_t i = 0; i + 1 <= usize / 16; i++) {
        Insn in{be32(data_, uc + 16 * i), be32(data_, uc + 16 * i + 4), be32(data_, uc + 16 * i + 8),
                be32(data_, uc + 16 * i + 12)};
        code.push_back(in);
        if (bits(in.d3, 1, 1))
            relative = true;
        if (in.d3 & 1)
            break;
    }
    // Pass 1: find used constants so they can be packed into a small array.
    for (auto &in : code) {
        const uint32_t src[3] = {(bits(in.d1, 0, 8) << 9) | bits(in.d2, 23, 9), bits(in.d2, 6, 17),
                                 (bits(in.d2, 0, 6) << 11) | bits(in.d3, 21, 11)};
        for (auto s : src)
            if (bits(s, 0, 2) == 3)
                consts.insert(int(bits(in.d1, 12, 10)));
    }
    constSlot.assign(512, -1);
    if (relative) {
        for (int i = 0; i < 468; i++)
            constSlot[i] = i;
        slotCount = 468;
    } else {
        for (int c : consts)
            constSlot[c] = slotCount++;
    }

    for (auto &in : code) {
        const uint32_t d0 = in.d0, d1 = in.d1, d2 = in.d2, d3 = in.d3;
        const uint32_t src[3] = {(bits(d1, 0, 8) << 9) | bits(d2, 23, 9), bits(d2, 6, 17),
                                 (bits(d2, 0, 6) << 11) | bits(d3, 21, 11)};
        const int inputIdx = int(bits(d1, 8, 4));
        const int constIdx = int(bits(d1, 12, 10));
        const uint32_t vop = bits(d1, 22, 5), sop = bits(d1, 27, 5);
        const bool absb[3] = {bits(d0, 21, 1) != 0, bits(d0, 22, 1) != 0, bits(d0, 23, 1) != 0};

        auto S = [&](int n) {
            const uint32_t s = src[n];
            std::string r;
            switch (bits(s, 0, 2)) {
            case 1:
                r = "R" + std::to_string(bits(s, 2, 6));
                temps.insert(int(bits(s, 2, 6)));
                break;
            case 2:
                r = vpInputName(inputIdx);
                inputs.insert(inputIdx);
                break;
            case 3:
                if (relative)
                    r = "vc[clamp(" + std::to_string(constIdx) + " + A0." + kSwz[bits(d0, 0, 2)] + ", 0, 467)]";
                else
                    r = "vc[" + std::to_string(constSlot[constIdx]) + "]";
                break;
            default: r = "vec4(0.0)";
            }
            r += '.';
            for (int k = 0; k < 4; k++)
                r += kSwz[bits(s, 14 - 2 * k, 2)];
            if (absb[n])
                r = "abs(" + r + ")";
            if (bits(s, 16, 1))
                r = "(-" + r + ")";
            return "(" + r + ")";
        };

        std::string vmask, smask;
        if (bits(d3, 16, 1)) vmask += 'x';
        if (bits(d3, 15, 1)) vmask += 'y';
        if (bits(d3, 14, 1)) vmask += 'z';
        if (bits(d3, 13, 1)) vmask += 'w';
        if (bits(d3, 20, 1)) smask += 'x';
        if (bits(d3, 19, 1)) smask += 'y';
        if (bits(d3, 18, 1)) smask += 'z';
        if (bits(d3, 17, 1)) smask += 'w';
        const bool sat = bits(d0, 26, 1);
        const bool condTest = bits(d0, 13, 1);
        const uint32_t cond = bits(d0, 10, 3);
        char csw[4];
        for (int k = 0; k < 4; k++)
            csw[k] = kSwz[bits(d0, 8 - 2 * k, 2)];
        const bool update = bits(d0, 14, 1) || bits(d0, 29, 1);
        const int dstTmp = int(bits(d0, 15, 6));
        const bool vecResult = bits(d0, 30, 1);
        const int dst = int(bits(d3, 2, 5));
        const int scaTmp = int(bits(d3, 7, 6));

        std::string vexpr, sexpr;
        if (vop) {
            const std::string a = S(0), b = S(1), c = S(2);
            switch (vop) {
            case 1: vexpr = a; break;
            case 2: vexpr = a + " * " + b; break;
            case 3: vexpr = a + " + " + c; break;
            case 4: vexpr = a + " * " + b + " + " + c; break;
            case 5: vexpr = "vec4(dot(" + a + ".xyz, " + b + ".xyz))"; break;
            case 6: vexpr = "vec4(dot(vec4(" + a + ".xyz, 1.0), " + b + "))"; break;
            case 7: vexpr = "vec4(dot(" + a + ", " + b + "))"; break;
            case 8: vexpr = "vec4(1.0, " + a + ".y * " + b + ".y, " + a + ".z, " + b + ".w)"; break;
            case 9: vexpr = "min(" + a + ", " + b + ")"; break;
            case 10: vexpr = "max(" + a + ", " + b + ")"; break;
            case 11: vexpr = "vec4(lessThan(" + a + ", " + b + "))"; break;
            case 12: vexpr = "vec4(greaterThanEqual(" + a + ", " + b + "))"; break;
            case 13: vexpr = "ARL:" + a; break;
            case 14: vexpr = "fract(" + a + ")"; break;
            case 15: vexpr = "floor(" + a + ")"; break;
            case 16: vexpr = "vec4(equal(" + a + ", " + b + "))"; break;
            case 17: vexpr = "vec4(0.0)"; break;
            case 18: vexpr = "vec4(greaterThan(" + a + ", " + b + "))"; break;
            case 19: vexpr = "vec4(lessThanEqual(" + a + ", " + b + "))"; break;
            case 20: vexpr = "vec4(notEqual(" + a + ", " + b + "))"; break;
            case 21: vexpr = "vec4(1.0)"; break;
            case 22: vexpr = "sign(" + a + ")"; break;
            default:
                body << "    // unsupported vector opcode " << vop << "\n";
            }
        }
        if (sop) {
            const std::string c = S(2);
            switch (sop) {
            case 1: sexpr = c; break;
            case 2: sexpr = "(1.0 / " + c + ")"; break;
            case 3: sexpr = "clamp(1.0 / " + c + ", 5.42101e-20, 1.884467e19)"; break;
            case 4: sexpr = "inversesqrt(abs(" + c + "))"; break;
            case 5: case 14: sexpr = "exp2(" + c + ")"; break;
            case 6: case 13: sexpr = "log2(" + c + ")"; break;
            case 7:
                sexpr = "vec4(1.0, max(" + c + ".x, 0.0), (" + c + ".x > 0.0) ? exp2(" + c + ".w * log2(max(" + c +
                        ".y, 1e-30))) : 0.0, 1.0)";
                break;
            case 15: sexpr = "sin(" + c + ")"; break;
            case 16: sexpr = "cos(" + c + ")"; break;
            default:
                body << "    // unsupported scalar opcode " << sop << "\n";
            }
        }

        auto writeMasked = [&](const std::string &tmp, const std::string &mask, const std::string &target) {
            if (condTest && cond != 7) {
                const char *op = condOp(cond);
                for (char ch : mask) {
                    const char ci = csw[std::strchr(kSwz, ch) - kSwz];
                    if (op)
                        body << "    if (CC." << ci << " " << op << " 0.0) " << target << "." << ch << " = " << tmp
                             << "." << ch << ";\n";
                }
            } else {
                body << "    " << target << "." << mask << " = " << tmp << "." << mask << ";\n";
            }
        };

        // Evaluate both results before writing either: both units read the
        // register file as it was at the start of the instruction.
        const bool isArl = vexpr.rfind("ARL:", 0) == 0;
        if (!vexpr.empty() && !isArl)
            body << "    tv = " << (sat ? "clamp(" + vexpr + ", 0.0, 1.0)" : vexpr) << ";\n";
        if (!sexpr.empty())
            body << "    ts = " << (sat ? "clamp(" + sexpr + ", 0.0, 1.0)" : sexpr) << ";\n";
        if (isArl) {
            const std::string m = vmask.empty() ? "xyzw" : vmask;
            body << "    A0." << m << " = ivec4(floor(" << vexpr.substr(4) << "))." << m << ";\n";
        } else if (!vexpr.empty()) {
            const std::string m = vmask.empty() ? "xyzw" : vmask;
            if (dstTmp != 0x3f) {
                temps.insert(dstTmp);
                writeMasked("tv", m, "R" + std::to_string(dstTmp));
            }
            if (vecResult && dst != 0x1f) {
                outputs.insert(dst);
                writeMasked("tv", m, dst == 0 ? "opos" : vpOutputName(dst));
            }
            if (update)
                body << "    CC." << m << " = tv." << m << ";\n";
        }
        if (!sexpr.empty()) {
            const std::string m = smask.empty() ? "xyzw" : smask;
            if (scaTmp != 0x3f) {
                temps.insert(scaTmp);
                writeMasked("ts", m, "R" + std::to_string(scaTmp));
            } else if (dst != 0x1f) {
                outputs.insert(dst);
                writeMasked("ts", m, dst == 0 ? "opos" : vpOutputName(dst));
            }
            if (update && vexpr.empty())
                body << "    CC." << m << " = ts." << m << ";\n";
        }
    }

    std::ostringstream o;
    o << "#version 330 core\n// translated from " << label << "\n";
    o << "uniform vec4 vc[" << std::max(slotCount, 1) << "];\n";
    for (int i : inputs)
        o << "layout(location = " << i << ") in vec4 " << vpInputName(i) << ";\n";
    for (int i : outputs)
        if (i != 0)
            o << "out vec4 " << vpOutputName(i) << ";\n";
    o << "void main() {\n    vec4 tv, ts; vec4 CC = vec4(0.0); ivec4 A0 = ivec4(0);\n";
    o << "    vec4 opos = vec4(0.0, 0.0, 0.0, 1.0);\n";
    for (int r : temps)
        o << "    vec4 R" << r << " = vec4(0.0);\n";
    for (int i : outputs)
        if (i != 0)
            o << "    " << vpOutputName(i) << " = vec4(0.0);\n";
    o << body.str();
    o << "    gl_Position = opos;\n}\n";
    glsl = o.str();
}

void CgShader::translateFragment()
{
    const uint32_t usize = be32(data_, 24), uc = be32(data_, 28), prog = be32(data_, 20);
    const bool outputFromH0 = prog + 28 < data_.size() && data_[prog + 28] != 0;

    std::map<uint32_t, std::string> embedded;
    for (auto &p : params)
        for (uint32_t off : p.embedded)
            embedded[off] = sanitize(p.name);
    for (auto &p : params)
        if (p.res >= CG_TEXUNIT0 && p.res < CG_TEXUNIT0 + 16)
            samplers.push_back({int(p.res - CG_TEXUNIT0), p.type == CG_SAMPLER3D, p.name});

    auto samplerIs3D = [&](int unit) {
        for (auto &s : samplers)
            if (s.unit == unit)
                return s.is3D;
        return false;
    };
    std::set<int> usedUnits;

    std::ostringstream body;
    std::set<std::string> regs{"R0", "H0"};
    std::set<int> inputs;
    std::set<std::string> uniformSet;

    uint32_t i = 0;
    while (i + 16 <= usize) {
        const uint32_t dst = fpSwap(be32(data_, uc + i)), s0 = fpSwap(be32(data_, uc + i + 4)),
                       s1 = fpSwap(be32(data_, uc + i + 8)), s2 = fpSwap(be32(data_, uc + i + 12));
        const uint32_t opc = bits(dst, 24, 6) | (bits(s1, 31, 1) << 6);
        const bool end = bits(dst, 0, 1);
        const bool hasConst = bits(s0, 0, 2) == 2 || bits(s1, 0, 2) == 2 || bits(s2, 0, 2) == 2;
        std::string constExpr;
        if (hasConst) {
            const uint32_t co = i + 16;
            auto it = embedded.find(co);
            if (it != embedded.end()) {
                constExpr = it->second;
                if (uniformSet.insert(constExpr).second)
                    uniforms.push_back(constExpr);
            } else {
                float v[4];
                for (int k = 0; k < 4; k++) {
                    const uint32_t w = fpSwap(be32(data_, uc + co + 4 * k));
                    std::memcpy(&v[k], &w, 4);
                }
                constExpr = "vec4(" + floatLit(v[0]) + ", " + floatLit(v[1]) + ", " + floatLit(v[2]) + ", " +
                            floatLit(v[3]) + ")";
            }
        }
        auto S = [&](uint32_t s, int n) {
            std::string r;
            switch (bits(s, 0, 2)) {
            case 0:
                r = std::string(bits(s, 8, 1) ? "H" : "R") + std::to_string(bits(s, 2, 6));
                regs.insert(r);
                break;
            case 1: {
                const int a = int(bits(dst, 13, 4));
                inputs.insert(a);
                r = fpInputName(a);
                break;
            }
            case 2: r = constExpr; break;
            default: r = "vec4(0.0)";
            }
            r += '.';
            for (int k = 0; k < 4; k++)
                r += kSwz[bits(s, 9 + 2 * k, 2)];
            if (n == 0 ? bits(s, 29, 1) : bits(s, 18, 1))
                r = "abs(" + r + ")";
            if (bits(s, 17, 1))
                r = "(-" + r + ")";
            return "(" + r + ")";
        };
        const std::string a = S(s0, 0), b = S(s1, 1), c = S(s2, 2);
        const int unit = int(bits(dst, 17, 4));
        auto texCoord = [&](const std::string &coord) {
            usedUnits.insert(unit);
            return "tc" + std::to_string(unit) + "(" + coord + ")";
        };
        auto sample = [&](const std::string &coord) {
            return "texture(tex" + std::to_string(unit) + ", " + texCoord(coord) + ")";
        };

        std::string ex;
        switch (opc) {
        case 0x01: ex = a; break;
        case 0x02: ex = a + " * " + b; break;
        case 0x03: ex = a + " + " + b; break;
        case 0x04: ex = a + " * " + b + " + " + c; break;
        case 0x05: ex = "vec4(dot(" + a + ".xyz, " + b + ".xyz))"; break;
        case 0x06: ex = "vec4(dot(" + a + ", " + b + "))"; break;
        case 0x07: ex = "vec4(1.0, " + a + ".y * " + b + ".y, " + a + ".z, " + b + ".w)"; break;
        case 0x08: ex = "min(" + a + ", " + b + ")"; break;
        case 0x09: ex = "max(" + a + ", " + b + ")"; break;
        case 0x0A: ex = "vec4(lessThan(" + a + ", " + b + "))"; break;
        case 0x0B: ex = "vec4(greaterThanEqual(" + a + ", " + b + "))"; break;
        case 0x0C: ex = "vec4(lessThanEqual(" + a + ", " + b + "))"; break;
        case 0x0D: ex = "vec4(greaterThan(" + a + ", " + b + "))"; break;
        case 0x0E: ex = "vec4(notEqual(" + a + ", " + b + "))"; break;
        case 0x0F: ex = "vec4(equal(" + a + ", " + b + "))"; break;
        case 0x10: ex = "fract(" + a + ")"; break;
        case 0x11: ex = "floor(" + a + ")"; break;
        case 0x15: ex = "dFdx(" + a + ")"; break;
        case 0x16: ex = "dFdy(" + a + ")"; break;
        case 0x17: ex = sample(a); break;
        case 0x18: ex = sample("vec4(" + a + ".xyz / " + a + ".w, 1.0)"); break;
        case 0x19:
            ex = "textureGrad(tex" + std::to_string(unit) + ", " + texCoord(a) + ", " +
                 (samplerIs3D(unit) ? b + ".xyz, " + c + ".xyz)" : b + ".xy, " + c + ".xy)");
            break;
        case 0x1A: ex = "vec4(1.0 / " + a + ".x)"; break;
        case 0x1B: ex = "vec4(inversesqrt(abs(" + a + ".x)))"; break;
        case 0x1C: ex = "vec4(exp2(" + a + ".x))"; break;
        case 0x1D: ex = "vec4(log2(" + a + ".x))"; break;
        case 0x1E:
            ex = "vec4(1.0, max(" + a + ".x, 0.0), (" + a + ".x > 0.0) ? pow(max(" + a + ".y, 0.0), " + a +
                 ".w) : 0.0, 1.0)";
            break;
        case 0x1F: ex = "mix(" + c + ", " + b + ", " + a + ")"; break;
        case 0x20: ex = "vec4(1.0)"; break;
        case 0x21: ex = "vec4(0.0)"; break;
        case 0x22: ex = "vec4(cos(" + a + ".x))"; break;
        case 0x23: ex = "vec4(sin(" + a + ".x))"; break;
        case 0x26: ex = "vec4(pow(" + a + ".x, " + b + ".x))"; break;
        case 0x2E: ex = "vec4(dot(" + a + ".xy, " + b + ".xy) + " + c + ".z)"; break;
        case 0x2F:
            ex = "textureLod(tex" + std::to_string(unit) + ", " + texCoord(a) + ", " + a + ".w)";
            break;
        case 0x31:
            ex = "texture(tex" + std::to_string(unit) + ", " + texCoord(a) + ", " + a + ".w)";
            break;
        case 0x38: ex = "vec4(dot(" + a + ".xy, " + b + ".xy))"; break;
        case 0x39: ex = "vec4(normalize(" + a + ".xyz), 0.0)"; break;
        case 0x3A: ex = a + " / " + b + ".x"; break;
        case 0x3B: ex = a + " * inversesqrt(" + b + ".x)"; break;
        default: break;
        }

        const uint32_t lt = bits(s0, 18, 1), eq = bits(s0, 19, 1), gr = bits(s0, 20, 1);
        const int condCode = int(lt | (eq << 1) | (gr << 2)); // NV: bit0 lt, bit1 eq, bit2 gt
        const char *op = nullptr;
        bool never = false;
        switch (condCode) {
        case 0: never = true; break;
        case 1: op = "<"; break;
        case 2: op = "=="; break;
        case 3: op = "<="; break;
        case 4: op = ">"; break;
        case 5: op = "!="; break;
        case 6: op = ">="; break;
        default: break;
        }
        char csw[4];
        for (int k = 0; k < 4; k++)
            csw[k] = kSwz[bits(s0, 21 + 2 * k, 2)];
        const std::string ccRead = bits(s0, 31, 1) ? "CC1" : "CC0";
        const std::string ccWrite = bits(s0, 30, 1) ? "CC1" : "CC0";

        std::string mask;
        if (bits(dst, 9, 1)) mask += 'x';
        if (bits(dst, 10, 1)) mask += 'y';
        if (bits(dst, 11, 1)) mask += 'z';
        if (bits(dst, 12, 1)) mask += 'w';

        if (opc == 0x12) { // KIL
            if (!never) {
                if (!op) {
                    body << "    discard;\n";
                } else {
                    body << "    if (";
                    for (int k = 0; k < 4; k++)
                        body << (k ? " || " : "") << ccRead << "." << csw[k] << " " << op << " 0.0";
                    body << ") discard;\n";
                }
            }
        } else if (opc == 0x00 || opc == 0x3D || opc == 0x3E) {
            // NOP / texture fences
        } else if (ex.empty()) {
            body << "    // unsupported fragment opcode 0x" << std::hex << opc << std::dec << "\n";
        } else if (!never && !mask.empty()) {
            static const char *scales[8] = {"", " * 2.0", " * 4.0", " * 8.0", "", " * 0.5", " * 0.25", " * 0.125"};
            const char *scale = scales[bits(s1, 28, 3)];
            std::string e = *scale ? "(" + ex + ")" + scale : ex;
            if (bits(dst, 31, 1))
                e = "clamp(" + e + ", 0.0, 1.0)";
            body << "    t = " << e << ";\n";
            const std::string reg = std::string(bits(dst, 7, 1) ? "H" : "R") + std::to_string(bits(dst, 1, 6));
            if (!bits(dst, 30, 1)) {
                regs.insert(reg);
                if (op) {
                    for (char ch : mask)
                        body << "    if (" << ccRead << "." << csw[std::strchr(kSwz, ch) - kSwz] << " " << op
                             << " 0.0) " << reg << "." << ch << " = t." << ch << ";\n";
                } else {
                    body << "    " << reg << "." << mask << " = t." << mask << ";\n";
                }
            }
            if (bits(dst, 8, 1))
                body << "    " << ccWrite << "." << mask << " = t." << mask << ";\n";
        }
        i += 16 + (hasConst ? 16 : 0);
        if (end)
            break;
    }

    std::ostringstream o;
    o << "#version 330 core\n// translated from " << label << "\n";
    for (auto &s : samplers) {
        o << "uniform " << (s.is3D ? "sampler3D" : "sampler2D") << " tex" << s.unit << "; // " << s.name << "\n";
        o << "uniform bool flip" << s.unit << ";\n";
        if (s.is3D)
            o << "vec3 tc" << s.unit << "(vec4 c) { return vec3(c.x, flip" << s.unit << " ? 1.0 - c.y : c.y, c.z); }\n";
        else
            o << "vec2 tc" << s.unit << "(vec4 c) { return vec2(c.x, flip" << s.unit << " ? 1.0 - c.y : c.y); }\n";
    }
    for (auto &u : uniforms)
        o << "uniform vec4 " << u << ";\n";
    for (int a : inputs)
        if (a >= 1 && a <= 13)
            o << "in vec4 " << fpInputName(a) << ";\n";
    o << "out vec4 fragColor;\nvoid main() {\n    vec4 t; vec4 CC0 = vec4(0.0), CC1 = vec4(0.0);\n";
    o << "    vec4 WPOS = gl_FragCoord;\n    vec4 FACE = vec4(gl_FrontFacing ? 1.0 : -1.0);\n";
    for (auto &r : regs)
        o << "    vec4 " << r << " = vec4(0.0);\n";
    o << body.str();
    // RSX render targets store NaN as 0 (fixed-point/fp16 conversion); float
    // targets on PC would keep it and spread it through later passes.
    o << "    vec4 outc = " << (outputFromH0 ? "H0" : "R0") << ";\n";
    o << "    fragColor = mix(outc, vec4(0.0), bvec4(isnan(outc.x), isnan(outc.y), isnan(outc.z), isnan(outc.w)));\n}\n";
    glsl = o.str();
}

static GLuint compileStage(GLenum type, const char *src, const char *label)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "shader compile failed (%s):\n%s\n%s", label, log, src);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint compileProgram(const char *vs, const char *fs, const char *label)
{
    GLuint v = compileStage(GL_VERTEX_SHADER, vs, label);
    GLuint f = compileStage(GL_FRAGMENT_SHADER, fs, label);
    if (!v || !f)
        return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindFragDataLocation(p, 0, "fragColor");
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "program link failed (%s):\n%s", label, log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

bool CgProgram::link(const CgShader *vp, const char *customVertexGlsl, const CgShader &fp)
{
    vp_ = vp;
    fp_ = &fp;
    const std::string label = (vp ? vp->label : std::string("custom")) + " + " + fp.label;
    prog_ = compileProgram(vp ? vp->glsl.c_str() : customVertexGlsl, fp.glsl.c_str(), label.c_str());
    if (!prog_)
        return false;
    if (vp) {
        vc_.assign(std::max(vp->slotCount, 1), {0, 0, 0, 0});
        vcLoc_ = glGetUniformLocation(prog_, "vc");
        for (auto &p : vp->params)
            if (p.res == CG_C && p.hasDefault && p.resIndex >= 0 && vp->constSlot[p.resIndex] >= 0)
                std::memcpy(vc_[vp->constSlot[p.resIndex]].data(), p.def, sizeof p.def);
    }
    for (auto &u : fp.uniforms)
        fpUni_.push_back({u, glGetUniformLocation(prog_, u.c_str())});
    glUseProgram(prog_);
    for (auto &s : fp.samplers) {
        GLint loc = glGetUniformLocation(prog_, ("tex" + std::to_string(s.unit)).c_str());
        if (loc >= 0)
            glUniform1i(loc, s.unit);
        tex_.push_back({s.name, s.unit, glGetUniformLocation(prog_, ("flip" + std::to_string(s.unit)).c_str())});
    }
    return true;
}

void CgProgram::destroy()
{
    if (prog_)
        glDeleteProgram(prog_);
    prog_ = 0;
}

void CgProgram::use() { glUseProgram(prog_); }

void CgProgram::set(const char *name, float x, float y, float z, float w)
{
    if (vp_) {
        for (auto &p : vp_->params) {
            if (p.res == CG_C && p.resIndex >= 0 && p.name == name) {
                const int slot = vp_->constSlot[p.resIndex];
                if (slot >= 0)
                    vc_[slot] = {x, y, z, w};
                break; // (a name can be in both stages, e.g. gaia's _SunDirection)
            }
        }
    }
    for (auto &u : fpUni_) {
        if (u.name == name) {
            glUniform4f(u.loc, x, y, z, w);
            return;
        }
    }
}

void CgProgram::setMatrix(const char *name, const float rows[16])
{
    if (!vp_)
        return;
    for (auto &p : vp_->params) {
        if (p.res == CG_C && p.resIndex >= 0 && p.name == name) {
            for (int r = 0; r < 4; r++) {
                const int slot = vp_->constSlot[p.resIndex + r];
                if (slot >= 0)
                    vc_[slot] = {rows[4 * r], rows[4 * r + 1], rows[4 * r + 2], rows[4 * r + 3]};
            }
            return;
        }
    }
}

void CgProgram::setTexture(const char *name, GLenum target, GLuint tex)
{
    for (auto &t : tex_) {
        if (t.name == name) {
            glActiveTexture(GL_TEXTURE0 + t.unit);
            glBindTexture(target, tex);
            glActiveTexture(GL_TEXTURE0);
            return;
        }
    }
}

void CgProgram::setFlipY(const char *name, bool flip)
{
    for (auto &t : tex_)
        if (t.name == name && t.flipLoc >= 0)
            glUniform1i(t.flipLoc, flip ? 1 : 0);
}

void CgProgram::upload()
{
    if (vcLoc_ >= 0 && !vc_.empty())
        glUniform4fv(vcLoc_, GLsizei(vc_.size()), vc_[0].data());
}
