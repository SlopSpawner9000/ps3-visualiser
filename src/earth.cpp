#include "earth.h"
#include "cg.h"
#include "dither.h"

#include "stb_image.h" // implementation lives in firmware.cpp (JPEG)

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <random>
#include <sstream>

namespace {

constexpr float kPi = 3.14159265f;

// --- small matrix helpers (column-major, like GL) ---
struct Mat4 {
    float m[16];
};
Mat4 multiply(const Mat4 &a, const Mat4 &b)
{
    Mat4 r{};
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++)
            for (int k = 0; k < 4; k++)
                r.m[c * 4 + rr] += a.m[k * 4 + rr] * b.m[c * 4 + k];
    return r;
}
Mat4 perspective(float fovyDeg, float aspect, float zn, float zf)
{
    const float f = 1.0f / std::tan(fovyDeg * kPi / 360.0f);
    Mat4 r{};
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = (zf + zn) / (zn - zf);
    r.m[11] = -1;
    r.m[14] = 2 * zf * zn / (zn - zf);
    return r;
}
void normalize3(float v[3])
{
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0)
        for (int i = 0; i < 3; i++)
            v[i] /= l;
}
Mat4 lookAt(const float eye[3], const float center[3], const float up[3])
{
    float f[3] = {center[0] - eye[0], center[1] - eye[1], center[2] - eye[2]};
    normalize3(f);
    float s[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]};
    normalize3(s);
    const float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
    Mat4 r{};
    r.m[0] = s[0], r.m[4] = s[1], r.m[8] = s[2];
    r.m[1] = u[0], r.m[5] = u[1], r.m[9] = u[2];
    r.m[2] = -f[0], r.m[6] = -f[1], r.m[10] = -f[2];
    r.m[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
    r.m[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    r.m[14] = f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2];
    r.m[15] = 1;
    return r;
}

// Cube-face lookup shared by the shaders (devnotes/EARTH.md). Face images are stored rotated by 180°,
// so the upright u,v (-1..1, v down) map to texture coords s = (1-u)/2, t = (1-v)/2.
const char *kFaceLookup = R"(
vec3 faceCoord(vec3 d) {
    vec3 a = abs(d);
    float u, v, f;
    if (a.y >= max(a.x, a.z)) {
        float y = a.y;
        if (d.y > 0.0) { f = 5.0; u = -d.x / y; v = -d.z / y; }
        else           { f = 4.0; u = -d.x / y; v =  d.z / y; }
    } else if (a.z >= a.x) {
        if (d.z > 0.0) { f = 0.0; u =  d.x / a.z; v = -d.y / a.z; }
        else           { f = 2.0; u = -d.x / a.z; v = -d.y / a.z; }
    } else {
        if (d.x > 0.0) { f = 1.0; u = -d.z / a.x; v = -d.y / a.x; }
        else           { f = 3.0; u =  d.z / a.x; v = -d.y / a.x; }
    }
    return vec3((1.0 - u) * 0.5, (1.0 - v) * 0.5, f);
}
// Texture coords of direction d projected onto face f (also for d outside that face): used for the
// gradients, so mip selection doesn't jump where neighbouring pixels fall on different faces.
vec2 faceST(vec3 d, float f) {
    float u, v;
    if (f > 4.5)      { u = -d.x / d.y;  v = -d.z / d.y; }
    else if (f > 3.5) { u = d.x / d.y;   v = -d.z / d.y; }
    else if (f > 2.5) { u = -d.z / d.x;  v = d.y / d.x; }
    else if (f > 1.5) { u = d.x / d.z;   v = d.y / d.z; }
    else if (f > 0.5) { u = -d.z / d.x;  v = -d.y / d.x; }
    else              { u = d.x / d.z;   v = -d.y / d.z; }
    return vec2((1.0 - u) * 0.5, (1.0 - v) * 0.5);
}
// Sample a cube-face array at direction d with gradients taken within d's own face (no seams).
vec4 cubeSample(sampler2DArray tex, vec3 d) {
    vec3 c = faceCoord(d);
    vec3 dx = dFdx(d), dy = dFdy(d);
    return textureGrad(tex, c, faceST(d + dx, c.z) - c.xy, faceST(d + dy, c.z) - c.xy);
}
)";

const char *kSphereVs = R"(#version 330 core
layout(location = 0) in vec3 pos;
uniform mat4 viewProj;
uniform float radius;
out vec3 worldPos;
void main() {
    worldPos = pos * radius;
    gl_Position = viewProj * vec4(worldPos, 1.0);
}
)";

// The planet: sunlit ground, clouds with soft shadows, ocean glint, a little atmospheric haze.
std::string earthFs()
{
    return std::string(R"(#version 330 core
uniform sampler2DArray ground;  // rgb colour, a specular (water)
uniform sampler2DArray clouds;  // r coverage
uniform vec3 eyePos, sunDir, sunColor;
uniform float cloudAngle, cloudBrightness, cloudBlend, cloudsOn, brightness;
uniform vec3 geoX, geoY, geoZ; // world -> geography (rows): turns the Earth for the real-time option
uniform float glowBoost;       // the music's beat bounce (strong reaction), 0 = none
in vec3 worldPos;
out vec4 fragColor;
)") + kFaceLookup + R"(
vec3 rotY(vec3 p, float a) { float c = cos(a), s = sin(a); return vec3(c * p.x + s * p.z, p.y, -s * p.x + c * p.z); }
// World -> texture frame. qgl_gaia_app puts longitude L at world azimuth L - 45 deg (setSunLongitude: 0xf86c),
// and the cube faces are centred on -135, -45, 45, 135 (faces 0-3), so face 2 (45 E) faces +z: a half turn.
vec3 texFrame(vec3 p) { p = vec3(dot(geoX, p), dot(geoY, p), dot(geoZ, p)); return vec3(-p.x, p.y, -p.z); }
void main() {
    vec3 n = normalize(worldPos);
    vec3 v = normalize(eyePos - worldPos);
    float ndl = dot(n, sunDir);
    vec3 nt = texFrame(n);
    float day = smoothstep(-0.08, 0.25, ndl);
    vec4 g = cubeSample(ground, nt);
    vec3 albedo = pow(g.rgb, vec3(2.2));
    float cov = 0.0, shadow = 0.0;
    if (cloudsOn > 0.5) {
        vec3 nc = rotY(nt, cloudAngle);
        cov = cubeSample(clouds, nc).r;
        // the cloud above the point on the way to the sun casts the shadow
        vec3 ns = rotY(texFrame(normalize(n + (sunDir - n * dot(sunDir, n)) * 0.006)), cloudAngle);
        shadow = cubeSample(clouds, ns).r;
    }
    vec3 lit = albedo * max(ndl, 0.0) * (1.0 - 0.6 * shadow) * sunColor * brightness;
    vec3 r = reflect(-sunDir, n);
    float rv = max(dot(r, v), 0.0);
    // ocean glint (specular map = water): broad and soft like the PS3's
    lit += g.a * pow(rv, 24.0) * 0.3 * day * sunColor * brightness;
    vec3 cloudCol = vec3(pow(cov, 0.8) * cloudBrightness * 3.6) * sunColor * brightness * max(ndl + 0.08, 0.0);
    vec3 col = mix(lit, cloudCol, clamp(pow(cov, 0.75) * cloudBlend * 2.6, 0.0, 1.0)); // (thin wisps show too)
    // Aerial perspective, the PS3's look: the air between the eye and the ground dims the ground and scatters
    // teal sunlight towards the eye, more towards the limb (longer path) -> hazy disk, bright cyan-white edge.
    float mu = clamp(dot(n, v), 0.0, 1.0); // (rounding can exceed 1 straight below the eye: pow(1 - mu) = NaN)
    float tau = 0.11 / (mu + 0.07);
    float air = smoothstep(-0.12, 0.3, ndl); // sunlit air
    vec3 skyCol = mix(vec3(0.9, 0.55, 0.35), vec3(0.06, 0.46, 0.85), smoothstep(-0.1, 0.06, ndl));
    // looking towards the sun through the air (a backlit crescent) it scatters forward: bright cyan-white
    float fwd = pow(max(dot(-v, sunDir), 0.0), 4.0);
    col = col * exp(-tau * vec3(1.6, 0.6, 0.25))
        + (skyCol + vec3(0.3, 0.62, 0.95) * fwd * 2.5) * (1.0 - exp(-tau)) * air * brightness * 0.62;
    // the thin bright rim right at the edge of the disk
    col += vec3(0.6, 0.82, 1.0) * pow(1.0 - mu, 14.0 - 6.0 * glowBoost) * air * brightness * 1.1 * (1.0 + glowBoost);
    // forward scattering of the sun's reflection: a wide warm sheen around the glint, over clouds too
    col += vec3(1.0, 0.82, 0.78) * pow(rv, 5.0) * day * brightness * 0.16;
    // (the night side stays black, as on the PS3)
    fragColor = vec4(col, 1.0);
}
)";
}

// The atmosphere's glow around the limb (drawn on a slightly larger sphere, back faces, additive).
const char *kAtmFs = R"(#version 330 core
uniform vec3 eyePos, sunDir, sunColor;
uniform float brightness, thickness;
in vec3 worldPos;
out vec4 fragColor;
void main() {
    vec3 rd = normalize(worldPos - eyePos);
    float tca = -dot(eyePos, rd);
    vec3 closest = eyePos + rd * max(tca, 0.0);
    // Height of the ray's closest approach above the ground; 0 where it hits the planet (the planet covers those
    // pixels, except at the mesh's polygon edge, which the glow then fills instead of leaving a dark seam).
    float h = max(length(closest) - 1.0, 0.0);
    // A tight bright core and a wide faint halo, faded out gently well inside the shell (soft, no band edge).
    float glow = (0.8 * exp(-h / (thickness * 0.28)) + 0.2 * exp(-h / (thickness * 0.8)))
               * (1.0 - smoothstep(thickness * 1.5, thickness * 4.0, h));
    float sunSide = dot(normalize(closest), sunDir);
    vec3 col = mix(vec3(1.0, 0.5, 0.25), vec3(0.45, 0.72, 0.95), smoothstep(-0.08, 0.08, sunSide));
    float lit = smoothstep(-0.2, 0.2, sunSide);
    // forward scattering when looking towards the sun through the limb
    float fwd = pow(max(dot(rd, sunDir), 0.0), 8.0);
    fragColor = vec4(col * glow * lit * brightness * (0.5 + 2.0 * fwd) * sunColor, 1.0);
}
)";

const char *kStarVs = R"(#version 330 core
layout(location = 0) in vec4 star; // direction, brightness
uniform mat4 viewProjRot;           // rotation-only view: stars at infinity
uniform float pointScale;
out float bright;
void main() {
    vec4 p = viewProjRot * vec4(star.xyz, 1.0);
    gl_Position = p.xyww;            // on the far plane
    bright = star.w;
    gl_PointSize = pointScale * (0.8 + 1.4 * star.w);
}
)";
const char *kStarFs = R"(#version 330 core
uniform float strength;
in float bright;
out vec4 fragColor;
void main() {
    vec2 c = gl_PointCoord - 0.5;
    float a = exp(-dot(c, c) * 18.0);
    fragColor = vec4(vec3(0.9, 0.93, 1.0) * bright * a * strength, 1.0);
}
)";

const char *kQuadVs = R"(#version 330 core
out vec2 uv;
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// The sun: core, glow and spikes around its screen position (additive).
const char *kSunFs = R"(#version 330 core
uniform vec2 sunPos;        // in uv
uniform float aspect, visible, strength, spikes, size;
uniform vec3 sunColor;
in vec2 uv;
out vec4 fragColor;
void main() {
    vec2 d = (uv - sunPos) * vec2(aspect, 1.0);
    float r = length(d);
    float core = exp(-r * r / (size * size)) * 40.0;
    float glow = exp(-r / (size * 6.0)) * 1.2 + exp(-r / (size * 30.0)) * 0.15;
    float ang = atan(d.y, d.x + 1e-7); // (atan(0, 0) is NaN at the sun's own pixel: bloom made it a black square)
    float spike = pow(abs(cos(ang * spikes * 0.5)), 60.0) * exp(-r / (size * 22.0)) * 0.9;
    fragColor = vec4(sunColor * (core + glow + spike) * visible * strength, 1.0);
}
)";

// Bright pass, downsampling to quarter size: 4 bilinear taps = the full 4x4 block of scene pixels (one tap would
// skip most of them: flickery, blocky glow).
const char *kBrightFs = R"(#version 330 core
uniform sampler2D scene;
uniform float threshold;
in vec2 uv;
out vec4 fragColor;
vec3 bright(vec2 p) {
    vec3 c = texture(scene, p).rgb;
    if (any(isnan(c)) || any(isinf(c))) c = vec3(0.0); // (one bad pixel would bloom into a black square)
    float l = max(c.r, max(c.g, c.b));
    return c * max(l - threshold, 0.0) / max(l, 1e-4);
}
void main() {
    vec2 t = 1.0 / vec2(textureSize(scene, 0));
    fragColor = vec4((bright(uv + t * vec2(-1.0, -1.0)) + bright(uv + t * vec2(1.0, -1.0)) +
                      bright(uv + t * vec2(-1.0, 1.0)) + bright(uv + t * vec2(1.0, 1.0))) * 0.25, 1.0);
}
)";
const char *kBlurFs = R"(#version 330 core
uniform sampler2D tex;
uniform vec2 dir;
in vec2 uv;
out vec4 fragColor;
void main() {
    vec3 s = texture(tex, uv).rgb * 0.227;
    s += (texture(tex, uv + dir * 1.385).rgb + texture(tex, uv - dir * 1.385).rgb) * 0.316;
    s += (texture(tex, uv + dir * 3.231).rgb + texture(tex, uv - dir * 3.231).rgb) * 0.070;
    fragColor = vec4(s, 1.0);
}
)";
// Exposure, bloom and tone mapping (SDR: a filmic curve; HDR: linear, highlights above 1).
const char *kToneFs = R"(#version 330 core
uniform sampler2D scene, bloom;
uniform float exposure, glare, white, hdr, fade;
in vec2 uv;
out vec4 fragColor;
// The quarter-size bloom scaled up smoothly (cubic B-spline from 4 bilinear taps): plain bilinear shows its texels
// as soft blocks in the glow.
vec3 bloomAt(vec2 p) {
    vec2 size = vec2(textureSize(bloom, 0)), q = p * size - 0.5, f = fract(q), i = q - f;
    vec2 w0 = (1.0 - f) * (1.0 - f) * (1.0 - f) / 6.0, w1 = (4.0 - 6.0 * f * f + 3.0 * f * f * f) / 6.0;
    vec2 w3 = f * f * f / 6.0, w2 = 1.0 - w0 - w1 - w3;
    vec2 s0 = w0 + w1, s1 = w2 + w3;
    vec2 c0 = (i - 0.5 + w1 / s0) / size, c1 = (i + 1.5 + w3 / s1) / size;
    return (texture(bloom, vec2(c0.x, c0.y)).rgb * s0.x + texture(bloom, vec2(c1.x, c0.y)).rgb * s1.x) * s0.y +
           (texture(bloom, vec2(c0.x, c1.y)).rgb * s0.x + texture(bloom, vec2(c1.x, c1.y)).rgb * s1.x) * s1.y;
}
void main() {
    vec3 c = (texture(scene, uv).rgb + bloomAt(uv) * glare) * exposure * fade;
    if (hdr > 0.5) {
        fragColor = vec4(min(c, vec3(white * 4.0)), 1.0);
    } else {
        c = c * (1.0 + c / (white * white)) / (1.0 + c); // extended Reinhard
        fragColor = vec4(pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2)), 1.0); // (dithering: Misc > Dithering, dither.cpp)
    }
}
)";

// Firmware path: our stand-in for the atmosphere table the PS3 computes on an SPU each frame (_AtmEarthIv, read by
// CubeEarthLayer.vpo/fpo). Texel (x, y) is the ground point at azimuth x*pi around the eye axis (from the sun's side)
// and angle y*polarMax from it, in the sun's frame (sun = +z). rgb = light scattered towards the eye on the way
// (the same model as the lookalike shader), a = optical depth (the firmware dims the ground by 1 - a * extinction).
const char *kIvFs = R"(#version 330 core
uniform vec3 setupZ, setupX;
uniform float eyeDist, polarMax, scale, glowBoost;
in vec2 uv;
out vec4 fragColor;
void main() {
    float az = uv.x * 3.14159265, pol = uv.y * polarMax;
    vec3 Y = normalize(cross(setupZ, setupX));
    vec3 n = cos(pol) * setupZ + sin(pol) * (cos(az) * setupX + sin(az) * Y);
    vec3 v = normalize(setupZ * eyeDist - n);
    float mu = clamp(dot(n, v), 0.0, 1.0), ndl = n.z; // (pow(1 - mu): NaN if mu > 1)
    float tau = 0.11 / (mu + 0.07);
    float air = smoothstep(-0.12, 0.3, ndl);
    vec3 skyCol = mix(vec3(0.9, 0.55, 0.35), vec3(0.06, 0.46, 0.85), smoothstep(-0.1, 0.06, ndl));
    float fwd = pow(max(-v.z, 0.0), 4.0);
    vec3 col = (skyCol + vec3(0.3, 0.62, 0.95) * fwd * 2.5) * (1.0 - exp(-tau)) * air * 0.62
             + vec3(0.6, 0.82, 1.0) * pow(1.0 - mu, 14.0 - 6.0 * glowBoost) * air * 1.1 * (1.0 + glowBoost);
    fragColor = vec4(col * scale, tau);
}
)";

// Linear interpolation of a preset path with Catmull-Rom smoothing.
float catmull(float p0, float p1, float p2, float p3, float t)
{
    return 0.5f * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t + (-p0 + 3 * p1 - 3 * p2 + p3) * t * t * t);
}

// Where the sun is overhead right now, as qgl_gaia_app's REALISTIC sun method computes it (gaia.s 0x1ca7c,
// 0xdd2c): latitude = -23.45 cos(0.0172142 (day of the year + 10)), longitude = frac(UTC day) x 360 - 180; and the
// world direction of a latitude / longitude (0xf86c: Y = 90 - lat, Z = lon - 45, then the SUN ANG formula).
// PS3VIS_EARTH_UTC="YYYY-MM-DD HH:MM" fixes the time (testing).
void subsolarNow(float &lat, float &lon)
{
    std::time_t now = std::time(nullptr);
    std::tm t{};
    int Y, M, D, h, m;
    if (const char *fix = SDL_getenv("PS3VIS_EARTH_UTC"); fix && std::sscanf(fix, "%d-%d-%d %d:%d", &Y, &M, &D, &h, &m) == 5) {
        static const int before[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
        t.tm_yday = before[std::clamp(M, 1, 12) - 1] + D - 1 + (M > 2 && Y % 4 == 0 ? 1 : 0);
        t.tm_hour = h, t.tm_min = m;
    } else {
#ifdef _WIN32
        gmtime_s(&t, &now);
#else
        gmtime_r(&now, &t);
#endif
    }
    lat = -23.45f * std::cos(0.0172142f * float(t.tm_yday + 10));
    lon = (float(t.tm_hour) + float(t.tm_min) / 60.0f + float(t.tm_sec) / 3600.0f) / 24.0f * 360.0f - 180.0f;
}

void geoDirection(float lat, float lon, float d[3])
{
    const float y = (90.0f - lat) * kPi / 180.0f, z = (lon - 45.0f) * kPi / 180.0f;
    d[0] = std::sin(y) * std::sin(z), d[1] = std::cos(y), d[2] = std::sin(y) * std::cos(z);
}

} // namespace

bool Earth::init(const fs::path &devFlash, std::string &error)
{
    if (ready_)
        return true;
    QrcArchive qrc;
    if (!qrc.load(devFlash / "vsh" / "resource" / "qgl" / "earth.qrc")) {
        error = "earth.qrc not found";
        return false;
    }
    // Presets: every folder with a camera flight (camera.path).
    for (const std::string &file : qrc.list("earth/presets/")) {
        const std::string suffix = "/earth/camera.path";
        if (file.size() <= suffix.size() || file.compare(file.size() - suffix.size(), suffix.size(), suffix) != 0)
            continue;
        const std::string dir = file.substr(0, file.size() - suffix.size()); // earth/presets/<name>
        Preset p;
        p.name = dir.substr(dir.rfind('/') + 1);
        const auto text = qrc.get(file); // (one copy: begin and end must come from the same buffer)
        if (!text)
            continue;
        std::istringstream lines(std::string(text->begin(), text->end()));
        for (std::string l; std::getline(lines, l);) {
            Key k{};
            if (std::sscanf(l.c_str(), "time=%f eye %f %f %f center %f %f %f up %f %f %f fovy=%f", &k.time, &k.eye[0],
                            &k.eye[1], &k.eye[2], &k.center[0], &k.center[1], &k.center[2], &k.up[0], &k.up[1], &k.up[2],
                            &k.fovy) == 11)
                p.path.push_back(k);
        }
        if (p.path.size() < 2)
            continue;
        auto mnu = [&](const char *name) {
            auto b = qrc.get(dir + "/earth/" + name);
            return b ? parseMnu(*b) : MnuValues{};
        };
        auto pick = [](const MnuValues &m, const char *key, float def) {
            auto it = m.find(key);
            return it == m.end() ? def : it->second;
        };
        const MnuValues sun = mnu("SUN.mnu"), hdr = mnu("HDR.mnu"), earth = mnu("EARTH.mnu"), clouds = mnu("CLOUDS.mnu"),
                        atm = mnu("ATM.mnu");
        p.sunY = pick(sun, "SUN ANG Y", 90), p.sunZ = pick(sun, "SUN ANG Z", 0), p.brightness = pick(sun, "BRIGHTNESS", 3.4f);
        p.exposure = pick(hdr, "EXPOSURE", 0.8f), p.white = pick(hdr, "WHITE LEVEL", 3.4f);
        p.glareLevel = pick(hdr, "GLARE LEVEL", 1.0f);
        p.animSpeed = pick(earth, "ANIM SPEED", 1.0f);
        p.cloudBrightness = pick(clouds, "BRIGHTNESS", 0.3f), p.cloudBlend = pick(clouds, "BLEND", 0.35f);
        p.seaAlbedo = pick(earth, "SEA ALBEDO", p.seaAlbedo), p.seaPow = pick(earth, "SEA POW", p.seaPow);
        p.landAlbedo = pick(earth, "LAND ALBEDO", p.landAlbedo), p.landPow = pick(earth, "LAND POW", p.landPow);
        p.parallax = pick(clouds, "PARALLAX", p.parallax), p.parallaxBias = pick(clouds, "PARALLAX BIAS", p.parallaxBias);
        p.shadowBias = pick(clouds, "SHADOW BIAS", p.shadowBias), p.shadowStrength = pick(clouds, "SHADOW STREN", p.shadowStrength);
        p.attnEarth = pick(atm, "ATTN EARTH", p.attnEarth), p.attnAtm = pick(atm, "ATTN ATM.", p.attnAtm);
        presets_.push_back(std::move(p));
    }
    std::sort(presets_.begin(), presets_.end(), [](const Preset &a, const Preset &b) { return a.name < b.name; });
    for (auto &p : presets_)
        names_.push_back(p.name);
    if (presets_.empty()) {
        error = "earth.qrc has no camera flights";
        return false;
    }

    // Textures: 6 faces of 2x2 tiles (512² JPEGs) -> 1024² layers. Ground colour + specular in one RGBA
    // array, cloud coverage in another (and the specular map alone for the firmware shader).
    auto loadArray = [&](GLuint &tex, bool colour, const char *single = "clouds") {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, colour ? GLint(GL_RGBA8) : GLint(GL_R8), 1024, 1024, 6, 0,
                     colour ? GL_RGBA : GL_RED, GL_UNSIGNED_BYTE, nullptr);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        std::vector<uint8_t> layer(size_t(1024) * 1024 * (colour ? 4 : 1));
        for (int f = 0; f < 6; f++) {
            for (int row = 0; row < 2; row++)
                for (int col = 0; col < 2; col++) {
                    char name[96];
                    auto tile = [&](const char *kind, int channels) {
                        std::snprintf(name, sizeof name, "earth/flashrom/%s/%d/1/%03d_%03d.jpg", kind, f, row, col);
                        auto b = qrc.get(name);
                        int w = 0, h = 0, n = 0;
                        unsigned char *px = b ? stbi_load_from_memory(b->data(), int(b->size()), &w, &h, &n, channels)
                                              : nullptr;
                        if (!px || w != 512 || h != 512) {
                            if (px)
                                stbi_image_free(px);
                            return static_cast<unsigned char *>(nullptr);
                        }
                        return px;
                    };
                    if (colour) {
                        unsigned char *rgb = tile("ground_cm", 3), *spec = tile("specular", 1); // (1 byte per pixel)
                        for (int y = 0; y < 512; y++)
                            for (int x = 0; x < 512; x++) {
                                uint8_t *dst = &layer[((size_t(row) * 512 + y) * 1024 + size_t(col) * 512 + x) * 4];
                                const size_t i = size_t(y) * 512 + x;
                                dst[0] = rgb ? rgb[i * 3] : 0;
                                dst[1] = rgb ? rgb[i * 3 + 1] : 0;
                                dst[2] = rgb ? rgb[i * 3 + 2] : 40;
                                dst[3] = spec ? spec[i] : 0;
                            }
                        if (rgb)
                            stbi_image_free(rgb);
                        if (spec)
                            stbi_image_free(spec);
                    } else {
                        unsigned char *c = tile(single, 1);
                        for (int y = 0; y < 512; y++)
                            for (int x = 0; x < 512; x++)
                                layer[(size_t(row) * 512 + y) * 1024 + size_t(col) * 512 + x] = c ? c[size_t(y) * 512 + x] : 0;
                        if (c)
                            stbi_image_free(c);
                    }
                }
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, f, 1024, 1024, 1, colour ? GL_RGBA : GL_RED, GL_UNSIGNED_BYTE,
                            layer.data());
        }
        glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_ANISOTROPY, 8); // (sharp towards the limb; ignored if unsupported)
        glGetError();
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    loadArray(ground_, true);
    loadArray(clouds_, false);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_SWIZZLE_G, GL_RED); // (the firmware shader reads clouds as rgb)
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_SWIZZLE_B, GL_RED);

    // A UV sphere (unit radius) for the planet and the atmosphere shell.
    std::vector<float> verts;
    std::vector<uint32_t> idx;
    const int segs = 160, rings = 80;
    for (int r = 0; r <= rings; r++)
        for (int s = 0; s <= segs; s++) {
            const float th = kPi * float(r) / rings, ph = 2 * kPi * float(s) / segs;
            verts.insert(verts.end(), {std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph)});
        }
    for (int r = 0; r < rings; r++)
        for (int s = 0; s < segs; s++) {
            const uint32_t a = uint32_t(r * (segs + 1) + s), b = a + uint32_t(segs + 1);
            idx.insert(idx.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    sphereIndices_ = int(idx.size());
    glGenVertexArrays(1, &sphereVao_);
    glBindVertexArray(sphereVao_);
    glGenBuffers(1, &sphereVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, sphereVbo_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(verts.size() * sizeof(float)), verts.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glGenBuffers(1, &sphereIbo_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sphereIbo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(idx.size() * sizeof(uint32_t)), idx.data(), GL_STATIC_DRAW);

    // Stars: fixed random directions and brightnesses (most faint, a few bright).
    std::mt19937 rng(1234);
    std::normal_distribution<float> nd;
    std::uniform_real_distribution<float> ud(0, 1);
    std::vector<float> stars;
    starCount_ = 6000;
    for (int i = 0; i < starCount_; i++) {
        float d[3] = {nd(rng), nd(rng), nd(rng)};
        normalize3(d);
        const float b = std::pow(ud(rng), 6.0f);
        stars.insert(stars.end(), {d[0], d[1], d[2], b});
    }
    glGenVertexArrays(1, &starVao_);
    glBindVertexArray(starVao_);
    glGenBuffers(1, &starVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, starVbo_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(stars.size() * sizeof(float)), stars.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
    glGenVertexArrays(1, &quadVao_);
    glBindVertexArray(0);

    earthProg_ = compileProgram(kSphereVs, earthFs().c_str(), "earth");
    atmProg_ = compileProgram(kSphereVs, kAtmFs, "earth atmosphere");
    starProg_ = compileProgram(kStarVs, kStarFs, "earth stars");
    sunProg_ = compileProgram(kQuadVs, kSunFs, "earth sun");
    brightProg_ = compileProgram(kQuadVs, kBrightFs, "earth bright");
    blurProg_ = compileProgram(kQuadVs, kBlurFs, "earth blur");
    toneProg_ = compileProgram(kQuadVs, kToneFs, "earth tone");
    if (!earthProg_ || !atmProg_ || !starProg_ || !sunProg_ || !brightProg_ || !blurProg_ || !toneProg_) {
        error = "earth shaders failed to compile";
        shutdown();
        return false;
    }
    loadArray(specular_, false, "specular");
    fwReady_ = initFirmware(qrc);
    if (!fwReady_)
        SDL_Log("earth: firmware shaders unavailable, the lookalike ones are used");
    ready_ = true;
    SDL_Log("earth: %zu views", presets_.size());
    return true;
}

void Earth::shutdown()
{
    GLuint texs[] = {ground_, clouds_, sceneTex_, bloomTex_[0], bloomTex_[1], finalTex_, specular_, ieTex_, specPowTex_, ivTex_};
    glDeleteTextures(10, texs);
    GLuint fbos[] = {sceneFbo_, bloomFbo_[0], bloomFbo_[1], finalFbo_, ivFbo_, msFbo_};
    glDeleteFramebuffers(6, fbos);
    GLuint rbs[] = {msColorRb_, msDepthRb_};
    glDeleteRenderbuffers(2, rbs);
    if (depthRb_)
        glDeleteRenderbuffers(1, &depthRb_);
    GLuint bufs[] = {sphereVbo_, sphereIbo_, starVbo_, patchVbo_, patchIbo_};
    glDeleteBuffers(5, bufs);
    GLuint vaos[] = {sphereVao_, quadVao_, starVao_, patchVao_};
    glDeleteVertexArrays(4, vaos);
    fwProg_.destroy();
    for (GLuint p : {earthProg_, atmProg_, starProg_, sunProg_, brightProg_, blurProg_, toneProg_, ivProg_})
        if (p)
            glDeleteProgram(p);
    *this = Earth{};
}

// The PS3's own surface shader (lib/gaia/CubeEarthLayer), in a hybrid setup: its inputs that come from the SPU or
// the exposure system are ours (devnotes/EARTH.md "Firmware shaders (hybrid)").
bool Earth::initFirmware(const QrcArchive &qrc)
{
    auto load = [&](CgShader &sh, const char *path) {
        auto b = qrc.get(path);
        return b && sh.parse(*b, path);
    };
    if (!load(fwVp_, "lib/gaia/CubeEarthLayer.vpo") || !load(fwFp_, "lib/gaia/CubeEarthLayer.fpo"))
        return false;
    std::string &g = fwFp_.glsl;
    auto replace = [&g](const std::string &from, const std::string &to) {
        const size_t at = g.find(from);
        if (at == std::string::npos)
            return false;
        g.replace(at, from.size(), to);
        return true;
    };
    // The cube-face textures (earth current/previous, clouds, specular) are our arrays, one layer per face.
    bool ok = replace("#version 330 core\n", "#version 330 core\nuniform float fwLayer;\n");
    for (int u = 0; u < 4; u++) {
        const std::string n = std::to_string(u);
        ok = ok && replace("uniform sampler2D tex" + n + ";", "uniform sampler2DArray tex" + n + ";") &&
             replace("vec2 tc" + n + "(vec4 c) { return vec2(c.x, flip" + n + " ? 1.0 - c.y : c.y); }",
                     "vec3 tc" + n + "(vec4 c) { return vec3(c.x, c.y, fwLayer); }");
    }
    // The end encodes the colour for the PS3's exposure tables (preexpose_*): keep the linear colour instead.
    const size_t enc = g.find("texture(tex13"), out = g.find("    vec4 outc");
    if (!ok || enc == std::string::npos || out == std::string::npos || out < enc) {
        SDL_Log("earth: unexpected CubeEarthLayer translation");
        return false;
    }
    const size_t from = g.rfind('\n', enc) + 1;
    g.replace(from, out - from, "    R0 = vec4(R2.xyz, 1.0);\n    H0 = R0;\n");
    if (!fwProg_.link(&fwVp_, nullptr, fwFp_))
        return false;

    // The PS3's ground light table: 256 RGBA floats (big-endian) by the sun's height (_AtmEarthIeCst).
    auto ie = qrc.get("earth/atm/_ie_table.dump");
    if (!ie || ie->size() < 4096)
        return false;
    std::vector<float> ieRows(1024);
    for (size_t i = 0; i < 1024; i++) {
        const uint8_t *b = ie->data() + i * 4;
        const uint32_t w = uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3];
        std::memcpy(&ieRows[i], &w, 4);
        if (!std::isfinite(ieRows[i]))
            ieRows[i] = 0;
    }
    auto tex2d = [](GLuint &t, GLint fmt, int w, int h, GLenum type, const void *px) {
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, fmt == GL_R32F ? GL_RED : GL_RGBA, type, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    tex2d(ieTex_, GL_RGBA32F, 256, 1, GL_FLOAT, ieRows.data());
    tex2d(specPowTex_, GL_R32F, 256, 16, GL_FLOAT, nullptr);
    tex2d(ivTex_, GL_RGBA16F, 64, 128, GL_HALF_FLOAT, nullptr);
    glGenFramebuffers(1, &ivFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, ivFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ivTex_, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ivProg_ = compileProgram(kQuadVs, kIvFs, "earth atmosphere table");
    if (!ivProg_)
        return false;

    // One patch per cube face: a grid of (x, y) in 0..1, the LOD morph target (z, w) the same point.
    const int n = 48;
    std::vector<float> verts;
    std::vector<uint32_t> idx;
    for (int y = 0; y <= n; y++)
        for (int x = 0; x <= n; x++) {
            const float u = float(x) / n, v = float(y) / n;
            verts.insert(verts.end(), {u, v, u, v});
        }
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            const uint32_t a = uint32_t(y * (n + 1) + x), b = a + uint32_t(n + 1);
            idx.insert(idx.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    patchIndices_ = int(idx.size());
    glGenVertexArrays(1, &patchVao_);
    glBindVertexArray(patchVao_);
    glGenBuffers(1, &patchVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, patchVbo_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(verts.size() * sizeof(float)), verts.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
    glGenBuffers(1, &patchIbo_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, patchIbo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(idx.size() * sizeof(uint32_t)), idx.data(), GL_STATIC_DRAW);
    glBindVertexArray(0);
    return true;
}

// Our scales for the firmware path where the PS3's exposure system would set them (tuned against the recording).
constexpr float kFwCloudScale = 0.035f; // cloud brightness in the shader's units
constexpr float kFwExposure = 0.75f;    // overall (the PS3 adapts its exposure: SimpleAutoExp)
constexpr float kFwGlintScale = 0.25f;  // _SpecPow


void Earth::renderFirmware(const float viewProjRows[16], const float eye[3], const float sun[3], float bright, const float geo[9])
{
    const Preset &p = presets_[size_t(preset_)];
    if (specPowFor_ != preset_) { // _SpecPow: glint by (reflection . sun, specular map) from EARTH.mnu
        std::vector<float> sp(256 * 16);
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 256; x++) {
                const float c = float(x) / 255.0f, w = float(y) / 15.0f;
                sp[size_t(y) * 256 + x] =
                    kFwGlintScale * ((1 - w) * p.landAlbedo * std::pow(c, p.landPow) + w * p.seaAlbedo * std::pow(c, p.seaPow));
            }
        glBindTexture(GL_TEXTURE_2D, specPowTex_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 16, GL_RED, GL_FLOAT, sp.data());
        specPowFor_ = preset_;
    }
    // _FunkyRot: world -> the sun's frame (sun = +z); _Setupz = the eye's direction and _Setupx = the sun's side
    // across it, both in that frame; _Params.x = the angle from the eye axis to the horizon.
    float a[3] = {0, 1, 0};
    if (std::abs(sun[1]) > 0.9f)
        a[0] = 1, a[1] = 0;
    float ax[3] = {a[1] * sun[2] - a[2] * sun[1], a[2] * sun[0] - a[0] * sun[2], a[0] * sun[1] - a[1] * sun[0]};
    normalize3(ax);
    const float ay[3] = {sun[1] * ax[2] - sun[2] * ax[1], sun[2] * ax[0] - sun[0] * ax[2], sun[0] * ax[1] - sun[1] * ax[0]};
    const float dist = std::sqrt(eye[0] * eye[0] + eye[1] * eye[1] + eye[2] * eye[2]);
    float z[3] = {(ax[0] * eye[0] + ax[1] * eye[1] + ax[2] * eye[2]) / dist, (ay[0] * eye[0] + ay[1] * eye[1] + ay[2] * eye[2]) / dist,
                  (sun[0] * eye[0] + sun[1] * eye[1] + sun[2] * eye[2]) / dist};
    float x[3] = {-z[2] * z[0], -z[2] * z[1], 1 - z[2] * z[2]};
    if (x[0] * x[0] + x[1] * x[1] + x[2] * x[2] < 1e-8f)
        x[0] = 1, x[1] = 0, x[2] = 0;
    normalize3(x);
    const float polarMax = std::acos(std::clamp(1.0f / std::max(dist, 1.0001f), 0.0f, 1.0f));

    // Our atmosphere table for this frame.
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glBindFramebuffer(GL_FRAMEBUFFER, ivFbo_);
    glViewport(0, 0, 64, 128);
    glUseProgram(ivProg_);
    glUniform3f(glGetUniformLocation(ivProg_, "setupZ"), z[0], z[1], z[2]);
    glUniform3f(glGetUniformLocation(ivProg_, "setupX"), x[0], x[1], x[2]);
    glUniform1f(glGetUniformLocation(ivProg_, "eyeDist"), dist);
    glUniform1f(glGetUniformLocation(ivProg_, "polarMax"), polarMax);
    glUniform1f(glGetUniformLocation(ivProg_, "scale"), bright / (8.0f * p.attnAtm)); // (the shader: 8 x ATTN ATM x table)
    glUniform1f(glGetUniformLocation(ivProg_, "glowBoost"), glowBoost());
    glBindVertexArray(quadVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindFramebuffer(GL_FRAMEBUFFER, drawFbo_);
    glViewport(0, 0, width_, height_);
    glEnable(GL_DEPTH_TEST);

    fwProg_.use();
    fwProg_.setMatrix("_MVP", viewProjRows);
    fwProg_.setMatrix("_MVPLast", viewProjRows);
    const float funky[16] = {ax[0], ax[1], ax[2], 0, ay[0], ay[1], ay[2], 0, sun[0], sun[1], sun[2], 0, 0, 0, 0, 1};
    fwProg_.setMatrix("_FunkyRot", funky);
    fwProg_.set("_Eye", eye[0], eye[1], eye[2], 1);
    fwProg_.set("_SunDirection", sun[0], sun[1], sun[2], 0);
    fwProg_.set("_LOD", 1e6f, 1e6f + 1); // (no geomorphing: one fixed level)
    fwProg_.set("_Params", polarMax, -0.5f); // y: the light table spans sun heights -0.5..1 (its zero is at index ~86)
    fwProg_.set("_Setupz", z[0], z[1], z[2]);
    fwProg_.set("_Setupx", x[0], x[1], x[2]);
    fwProg_.set("_OffsetAndScaleEarthCurrent", 0, 0, 1, 1);
    fwProg_.set("_OffsetAndScaleEarthPrevious", 0, 0, 1, 1);
    fwProg_.set("_OffsetAndScaleSpecular", 0, 0, 1, 1);
    fwProg_.set("_OffsetAndScaleCloud", 0, 0, 1, 0.1f); // (w: scales the cloud parallax and shadow offsets)
    // Fragment program: clouds blend towards their brightness (our scale; ATTN EARTH can't be it: view 0a has
    // ATTN EARTH 0 and still shows clouds, its ocean colour all from the air); the ground is dimmed by
    // 1 - depth x extinction.
    fwProg_.set("_CloudParams", p.parallax, p.parallaxBias, tweaks.clouds ? 1.0f : 0.0f, p.cloudBrightness * kFwCloudScale);
    fwProg_.set("_CloudShadowAndStretch", p.shadowBias, tweaks.clouds ? p.shadowStrength : 0.0f);
    const float ext[3] = {0.55f, 0.24f, 0.1f};
    fwProg_.set("_EarthStuff_0_", ext[0], 1);
    fwProg_.set("_EarthStuff_1_", ext[1], 1);
    fwProg_.set("_EarthStuff_2_", ext[2], 1);
    fwProg_.set("_Attn", p.attnEarth * bright / 1.6f, p.attnAtm);
    fwProg_.set("_SunDirection", sun[0], sun[1], sun[2], 0);
    fwProg_.setTexture("_EarthCurrentTex", GL_TEXTURE_2D_ARRAY, ground_);
    fwProg_.setTexture("_EarthPreviousTex", GL_TEXTURE_2D_ARRAY, ground_);
    fwProg_.setTexture("_SpecularTex", GL_TEXTURE_2D_ARRAY, specular_);
    fwProg_.setTexture("_CloudTex", GL_TEXTURE_2D_ARRAY, clouds_);
    fwProg_.setTexture("_AtmEarthIv", GL_TEXTURE_2D, ivTex_);
    fwProg_.setTexture("_AtmEarthIeCst", GL_TEXTURE_2D, ieTex_);
    fwProg_.setTexture("_SpecPow", GL_TEXTURE_2D, specPowTex_);
    const GLint layerLoc = glGetUniformLocation(fwProg_.id(), "fwLayer");
    // Each face: its 4 corners (x, y = 0,0 / 1,0 / 1,1 / 0,1) as points of the texture frame (devnotes/EARTH.md: s, t
    // -> u = 1 - 2s, v = 1 - 2t), turned into the world frame (x, z negated, see texFrame()).
    glBindVertexArray(patchVao_);
    for (int f = 0; f < 6; f++) {
        float rows[16] = {};
        const float st[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        for (int c = 0; c < 4; c++) {
            const float u = 1 - 2 * st[c][0], v = 1 - 2 * st[c][1];
            float d[3];
            switch (f) {
            case 0: d[0] = u, d[1] = -v, d[2] = 1; break;
            case 1: d[0] = 1, d[1] = -v, d[2] = -u; break;
            case 2: d[0] = -u, d[1] = -v, d[2] = -1; break;
            case 3: d[0] = -1, d[1] = -v, d[2] = u; break;
            case 4: d[0] = -u, d[1] = -1, d[2] = v; break;
            default: d[0] = -u, d[1] = 1, d[2] = -v; break;
            }
            const float w[3] = {-d[0], d[1], -d[2]}; // (geography; turned into the world by geo's transpose)
            for (int r = 0; r < 3; r++)
                rows[r * 4 + c] = geo[0 * 3 + r] * w[0] + geo[1 * 3 + r] * w[1] + geo[2 * 3 + r] * w[2];
        }
        fwProg_.setMatrix("_Pos", rows);
        glUniform1f(layerLoc, float(f));
        fwProg_.upload();
        glDrawElements(GL_TRIANGLES, patchIndices_, GL_UNSIGNED_INT, nullptr);
    }
    glEnable(GL_CULL_FACE);
}

void Earth::dumpState(std::string &out) const
{
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "[earth]\nready %d, view %s, flight time %.2f (offline: PS3VIS_EARTH_TIME), fade %.2f, cloud drift %.4f rad\n"
                  "shaders %s (firmware path ready %d), real time %d, music %d, pulse %.2f\n",
                  int(ready_), ready_ ? presets_[size_t(preset_)].name.c_str() : "-", double(time_), double(fade_),
                  double(cloudAngle_), tweaks.firmwareShaders ? "firmware (hybrid)" : "lookalike", int(fwReady_),
                  tweaks.realTime, tweaks.music, double(pulse_));
    out += buf;
}

void Earth::setPreset(int index)
{
    if (presets_.empty())
        return;
    index = ((index % int(presets_.size())) + int(presets_.size())) % int(presets_.size());
    if (fade_ <= 0.01f || !ready_) { // nothing on screen yet: switch at once
        preset_ = index;
        time_ = 0;
        pending_ = -1;
        return;
    }
    pending_ = index; // fade out, switch, fade in (update())
    fadeTarget_ = 0;
}

void Earth::cameraAt(float t, float eye[3], float center[3], float up[3], float &fovy) const
{
    const auto &path = presets_[size_t(preset_)].path;
    const int n = int(path.size());
    int i = 0;
    while (i < n - 2 && path[size_t(i + 1)].time <= t)
        i++;
    const Key &k1 = path[size_t(i)], &k2 = path[size_t(i + 1)];
    const Key &k0 = path[size_t(std::max(i - 1, 0))], &k3 = path[size_t(std::min(i + 2, n - 1))];
    const float u = std::clamp((t - k1.time) / std::max(k2.time - k1.time, 1e-3f), 0.0f, 1.0f);
    for (int c = 0; c < 3; c++) {
        eye[c] = catmull(k0.eye[c], k1.eye[c], k2.eye[c], k3.eye[c], u);
        center[c] = catmull(k0.center[c], k1.center[c], k2.center[c], k3.center[c], u);
        up[c] = catmull(k0.up[c], k1.up[c], k2.up[c], k3.up[c], u);
    }
    fovy = k1.fovy + (k2.fovy - k1.fovy) * u;
}

void Earth::update(float dt, float level, const float *columns)
{
    if (!ready_)
        return;
    const Preset &p = presets_[size_t(preset_)];
    time_ += dt * tweaks.flightSpeed;
    const float end = p.path.back().time;
    if (time_ >= end - 1.5f && fadeTarget_ > 0 && pending_ < 0) { // the flight ends: fade, then loop / next view
        pending_ = tweaks.nextAfterFlight ? (preset_ + 1) % int(presets_.size()) : preset_;
        fadeTarget_ = 0;
    }
    fade_ += (fadeTarget_ - fade_) * std::min(1.0f, dt / 0.35f);
    if (fadeTarget_ == 0 && fade_ < 0.02f && pending_ >= 0) {
        preset_ = pending_;
        pending_ = -1;
        time_ = 0;
        fadeTarget_ = 1;
    }
    if (fadeTarget_ > 0 && fade_ < fadeTarget_ && fade_ > 0.98f * fadeTarget_)
        fade_ = fadeTarget_;
    cloudAngle_ += dt * 0.004f * p.animSpeed * tweaks.cloudSpeed;
    // Music reaction (not on the PS3): the low end = the bass columns at both ends of the spectrum (L 0.., R ..127).
    constexpr int kLow = 10;
    float bass = 0;
    for (int i = 0; i < kCols; i++) {
        const bool used = columns && tweaks.music > 0 && (i < kLow || i >= kCols - kLow);
        if (!used) {
            applied_[i] = -1.0f;
            continue;
        }
        const float was = std::max(applied_[i], 0.0f); // (smoothed like the band level below)
        applied_[i] = was + (columns[i] - was) * std::min(1.0f, dt / 0.08f);
        bass += columns[i];
    }
    bass /= 2.0f * kLow;
    // Gentle and strong: bass and level push exposure and glare up a little.
    bass_ += (bass - bass_) * std::min(1.0f, dt / 0.08f);
    const float target = tweaks.music == 0 ? 0.0f : std::clamp(bass_ * 0.6f + level * 0.8f, 0.0f, 1.5f);
    pulse_ += (target - pulse_) * std::min(1.0f, dt / (target > pulse_ ? 0.05f : 0.4f));
    // Strong: beats. A beat = the low end jumping above its recent average (fast vs slow follower); it kicks the
    // atmosphere's glow outwards and a damped spring brings it back, overshooting a little (the bounce).
    if (lowSlow_ <= 0) // (start from the music's level, or the first second is all "beats")
        lowFast_ = lowSlow_ = bass;
    lowFast_ += (bass - lowFast_) * std::min(1.0f, dt / 0.03f);
    lowSlow_ += (bass - lowSlow_) * std::min(1.0f, dt / 0.6f);
    beatWait_ -= dt;
    kick_ *= std::exp(-dt / 0.15f);
    const float onset = lowFast_ - lowSlow_ * 1.12f - 0.015f;
    if (tweaks.music == 2 && onset > 0 && beatWait_ <= 0) {
        kick_ = std::min(1.0f, 0.45f + onset * 4.0f);
        beatWait_ = 0.2f; // (at most 5 beats a second)
    }
    const float push = tweaks.music == 2 ? kick_ : 0.0f;
    for (float left = std::min(dt, 0.25f); left > 0; left -= 1.0f / 240.0f) { // (small steps: stable at any frame rate)
        const float h = std::min(left, 1.0f / 240.0f);
        constexpr float w = 2 * kPi * 2.6f, zeta = 0.32f; // ~2.6 Hz, light damping: one or two small rebounds
        glowVel_ += ((push - glow_) * w * w - 2 * zeta * w * glowVel_) * h;
        glow_ += glowVel_ * h;
    }
    static const bool trace = SDL_getenv("PS3VIS_EARTH_DEBUG") != nullptr;
    if (trace)
        SDL_Log("earth music: t %.3f low %.3f fast %.3f slow %.3f beat %.2f glow %+.3f", double(time_), double(bass),
                double(lowFast_), double(lowSlow_), double(kick_), double(glow_));
}

// The scene's multisampled twin (anti-aliasing): drawn into, then resolved into sceneTex_ for the post passes.
void Earth::ensureMultisample(int samples)
{
    if (samples == msSamples_ && width_ == msW_ && height_ == msH_)
        return;
    msSamples_ = samples, msW_ = width_, msH_ = height_;
    if (samples <= 1)
        return;
    if (!msFbo_) {
        glGenFramebuffers(1, &msFbo_);
        glGenRenderbuffers(1, &msColorRb_);
        glGenRenderbuffers(1, &msDepthRb_);
    }
    glBindRenderbuffer(GL_RENDERBUFFER, msColorRb_);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA16F, width_, height_);
    glBindRenderbuffer(GL_RENDERBUFFER, msDepthRb_);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT24, width_, height_);
    glBindFramebuffer(GL_FRAMEBUFFER, msFbo_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, msColorRb_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, msDepthRb_);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        SDL_Log("earth: %dx anti-aliasing unavailable, drawing without", samples);
        msSamples_ = 1;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Earth::resize(int w, int h)
{
    if (w == width_ && h == height_)
        return;
    width_ = w, height_ = h;
    auto tex = [](GLuint &t, int tw, int th) {
        if (!t)
            glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, tw, th, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    auto fbo = [](GLuint &f, GLuint t, GLuint depth) {
        if (!f)
            glGenFramebuffers(1, &f);
        glBindFramebuffer(GL_FRAMEBUFFER, f);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
        if (depth)
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    };
    tex(sceneTex_, w, h);
    if (!depthRb_)
        glGenRenderbuffers(1, &depthRb_);
    glBindRenderbuffer(GL_RENDERBUFFER, depthRb_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    fbo(sceneFbo_, sceneTex_, depthRb_);
    for (int i = 0; i < 2; i++) {
        tex(bloomTex_[i], std::max(1, w / 4), std::max(1, h / 4));
        fbo(bloomFbo_[i], bloomTex_[i], 0);
    }
    tex(finalTex_, w, h);
    fbo(finalFbo_, finalTex_, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Earth::render(int width, int height)
{
    if (!ready_ || width <= 0 || height <= 0)
        return;
    resize(width, height);
    const Preset &p = presets_[size_t(preset_)];
    float eye[3], center[3], up[3], fovy;
    cameraAt(std::min(time_, p.path.back().time), eye, center, up, fovy);
    if (SDL_getenv("PS3VIS_EARTH_DEBUG"))
        SDL_Log("earth render: view %s t %.2f eye %.3f %.3f %.3f fovy %.2f", p.name.c_str(), double(time_), double(eye[0]),
                double(eye[1]), double(eye[2]), double(fovy));
    const float aspect = float(width) / float(height);
    const Mat4 proj = perspective(fovy, aspect, 0.01f, 100.0f), view = lookAt(eye, center, up);
    const Mat4 viewProj = multiply(proj, view);
    Mat4 viewRot = view;
    viewRot.m[12] = viewRot.m[13] = viewRot.m[14] = 0;
    const Mat4 viewProjRot = multiply(proj, viewRot);
    // Sun direction from the preset's angles, as qgl_gaia_app computes it (0x302b4 -> 0xf7c0 -> 0xdd70 builds
    // (sinY cosZ, sinY sinZ, cosY), then the vperm at 0xf6e0 reorders it to y, z, x):
    const float ay = p.sunY * kPi / 180.0f, az = p.sunZ * kPi / 180.0f;
    float sun[3] = {std::sin(ay) * std::sin(az), std::cos(ay), std::sin(ay) * std::cos(az)};
    normalize3(sun);
    // Real time: the geography (rows: world -> geography) turned so that the point under the sun right now faces the
    // view's sun, or the sun itself moved there.
    float geo[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    if (tweaks.realTime > 0) {
        float lat, lon, g[3];
        subsolarNow(lat, lon);
        geoDirection(lat, lon, g);
        if (tweaks.realTime == 2) {
            std::memcpy(sun, g, sizeof sun);
        } else { // the rotation taking the sun's direction (world) to g (geography): about sun x g
            float k[3] = {sun[1] * g[2] - sun[2] * g[1], sun[2] * g[0] - sun[0] * g[2], sun[0] * g[1] - sun[1] * g[0]};
            const float c = sun[0] * g[0] + sun[1] * g[1] + sun[2] * g[2];
            float sn = std::sqrt(k[0] * k[0] + k[1] * k[1] + k[2] * k[2]);
            if (sn < 1e-5f) // (parallel or opposite: turn about the polar axis)
                k[0] = 0, k[1] = 1, k[2] = 0, sn = 0;
            else
                k[0] /= sn, k[1] /= sn, k[2] /= sn;
            const float s = sn, t = 1 - c;
            const float r[9] = {t * k[0] * k[0] + c,        t * k[0] * k[1] - s * k[2], t * k[0] * k[2] + s * k[1],
                                t * k[0] * k[1] + s * k[2], t * k[1] * k[1] + c,        t * k[1] * k[2] - s * k[0],
                                t * k[0] * k[2] - s * k[1], t * k[1] * k[2] + s * k[0], t * k[2] * k[2] + c};
            std::memcpy(geo, r, sizeof geo);
        }
    }
    const float sunCol[3] = {1.0f, 0.97f, 0.92f};
    const float bright = p.brightness / 3.4f * 1.6f;

    // (PS3VIS_EARTH_NANCHECK: count NaN / Inf pixels in the scene after each pass; the bloom would blow one up into
    // a black square.)
    static const bool nanCheckOn = SDL_getenv("PS3VIS_EARTH_NANCHECK") != nullptr;
    bool readable = true; // (a multisampled target can't be read back: checked after the resolve)
    auto nanCheck = [&](const char *stage) {
        if (!nanCheckOn || !readable)
            return;
        std::vector<float> px(size_t(width) * size_t(height) * 4);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_FLOAT, px.data());
        int bad = 0, bx = -1, by = -1;
        for (size_t i = 0; i < px.size(); i++)
            if (!std::isfinite(px[i]) && bad++ == 0)
                bx = int(i / 4 % size_t(width)), by = int(i / 4 / size_t(width));
        if (bad)
            SDL_Log("earth nan: t %.3f view %s %s: %d bad values, first at %d,%d", double(time_), p.name.c_str(), stage, bad,
                    bx, by);
    };

    // Anti-aliasing: draw into the multisampled twin, resolve before the post passes.
    static GLint maxSamples = -1;
    if (maxSamples < 0) {
        maxSamples = 1;
        glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
    }
    ensureMultisample(std::min(tweaks.antiAlias <= 0 ? 1 : 1 << std::clamp(tweaks.antiAlias, 1, 3), std::max(int(maxSamples), 1)));
    const bool ms = msSamples_ > 1;
    drawFbo_ = ms ? msFbo_ : sceneFbo_;
    readable = !ms;
    glBindFramebuffer(GL_FRAMEBUFFER, drawFbo_);
    glViewport(0, 0, width, height);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT); // (the sphere's triangles wind clockwise seen from outside)

    // The planet.
    if (tweaks.firmwareShaders && fwReady_) {
        float rows[16];
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                rows[r * 4 + c] = viewProj.m[c * 4 + r];
        renderFirmware(rows, eye, sun, bright, geo);
    } else {
        glUseProgram(earthProg_);
        glUniformMatrix4fv(glGetUniformLocation(earthProg_, "viewProj"), 1, GL_FALSE, viewProj.m);
        glUniform1f(glGetUniformLocation(earthProg_, "radius"), 1.0f);
        glUniform3f(glGetUniformLocation(earthProg_, "eyePos"), eye[0], eye[1], eye[2]);
        glUniform3f(glGetUniformLocation(earthProg_, "sunDir"), sun[0], sun[1], sun[2]);
        glUniform3f(glGetUniformLocation(earthProg_, "sunColor"), sunCol[0], sunCol[1], sunCol[2]);
        glUniform1f(glGetUniformLocation(earthProg_, "cloudAngle"), cloudAngle_);
        glUniform1f(glGetUniformLocation(earthProg_, "cloudBrightness"), p.cloudBrightness);
        glUniform1f(glGetUniformLocation(earthProg_, "cloudBlend"), p.cloudBlend);
        glUniform1f(glGetUniformLocation(earthProg_, "cloudsOn"), tweaks.clouds ? 1.0f : 0.0f);
        glUniform1f(glGetUniformLocation(earthProg_, "brightness"), bright);
        glUniform1f(glGetUniformLocation(earthProg_, "glowBoost"), glowBoost());
        glUniform3f(glGetUniformLocation(earthProg_, "geoX"), geo[0], geo[1], geo[2]);
        glUniform3f(glGetUniformLocation(earthProg_, "geoY"), geo[3], geo[4], geo[5]);
        glUniform3f(glGetUniformLocation(earthProg_, "geoZ"), geo[6], geo[7], geo[8]);
        glUniform1i(glGetUniformLocation(earthProg_, "ground"), 0);
        glUniform1i(glGetUniformLocation(earthProg_, "clouds"), 1);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, ground_);
        glActiveTexture(GL_TEXTURE0 + 1);
        glBindTexture(GL_TEXTURE_2D_ARRAY, clouds_);
        glActiveTexture(GL_TEXTURE0);
        glBindVertexArray(sphereVao_);
        glDrawElements(GL_TRIANGLES, sphereIndices_, GL_UNSIGNED_INT, nullptr);
    }

    nanCheck("after planet");

    // Stars (on the far plane, only where the planet isn't).
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glDepthMask(GL_FALSE);
    if (tweaks.stars > 0) {
        glEnable(GL_PROGRAM_POINT_SIZE);
        glUseProgram(starProg_);
        glUniformMatrix4fv(glGetUniformLocation(starProg_, "viewProjRot"), 1, GL_FALSE, viewProjRot.m);
        glUniform1f(glGetUniformLocation(starProg_, "pointScale"), std::max(1.0f, float(height) / 720.0f) * 1.6f);
        glUniform1f(glGetUniformLocation(starProg_, "strength"), 0.5f * tweaks.stars);
        glBindVertexArray(starVao_);
        glDrawArrays(GL_POINTS, 0, starCount_);
        glDisable(GL_PROGRAM_POINT_SIZE);
    }

    nanCheck("after stars");

    // The atmosphere's glow (back faces of a slightly larger sphere).
    glCullFace(GL_BACK);
    glUseProgram(atmProg_);
    glUniformMatrix4fv(glGetUniformLocation(atmProg_, "viewProj"), 1, GL_FALSE, viewProj.m);
    glUniform1f(glGetUniformLocation(atmProg_, "radius"), 1.16f); // (room for the glow to fade out, also when it swells)
    glUniform3f(glGetUniformLocation(atmProg_, "eyePos"), eye[0], eye[1], eye[2]);
    glUniform3f(glGetUniformLocation(atmProg_, "sunDir"), sun[0], sun[1], sun[2]);
    glUniform3f(glGetUniformLocation(atmProg_, "sunColor"), sunCol[0], sunCol[1], sunCol[2]);
    glUniform1f(glGetUniformLocation(atmProg_, "brightness"), bright * 0.6f * (1.0f + 1.2f * glowBoost()));
    glUniform1f(glGetUniformLocation(atmProg_, "thickness"), 0.018f * (1.0f + 0.7f * glowBoost())); // (swells on a beat)
    glBindVertexArray(sphereVao_);
    glDrawElements(GL_TRIANGLES, sphereIndices_, GL_UNSIGNED_INT, nullptr);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);

    nanCheck("after atmosphere");

    // The sun: on screen where its direction projects.
    {
        const float far[4] = {sun[0], sun[1], sun[2], 0};
        float clip[4] = {};
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                clip[r] += viewProjRot.m[c * 4 + r] * (c < 3 ? far[c] : 1.0f);
        if (clip[3] > 0) {
            const float sx = clip[0] / clip[3] * 0.5f + 0.5f, sy = clip[1] / clip[3] * 0.5f + 0.5f;
            const float tca = sun[0] * -eye[0] + sun[1] * -eye[1] + sun[2] * -eye[2]; // towards the centre
            const float e2 = eye[0] * eye[0] + eye[1] * eye[1] + eye[2] * eye[2];
            const float miss = tca > 0 ? std::sqrt(std::max(e2 - tca * tca, 0.0f)) : 10.0f;
            // Hidden by the planet and the air at the limb; and like the PS3's glare (a post-process of the image),
            // a sun off-screen casts no spikes into it.
            const float onScreen = std::clamp((0.5f - std::max(std::abs(sx - 0.5f), std::abs(sy - 0.5f))) / 0.03f + 1.0f, 0.0f, 1.0f);
            const float visible = std::clamp((miss - 1.02f) / 0.03f, 0.0f, 1.0f) * onScreen;
            if (visible > 0) { // (hidden: nothing to draw)
                glUseProgram(sunProg_);
                glUniform2f(glGetUniformLocation(sunProg_, "sunPos"), sx, sy);
                glUniform1f(glGetUniformLocation(sunProg_, "aspect"), aspect);
                glUniform1f(glGetUniformLocation(sunProg_, "visible"), visible);
                glUniform1f(glGetUniformLocation(sunProg_, "strength"), bright * tweaks.glare * (1.0f + pulse_ * 0.5f));
                glUniform1f(glGetUniformLocation(sunProg_, "spikes"), 6.0f);
                glUniform1f(glGetUniformLocation(sunProg_, "size"), 0.012f);
                glUniform3f(glGetUniformLocation(sunProg_, "sunColor"), sunCol[0], sunCol[1], sunCol[2]);
                glBindVertexArray(quadVao_);
                glDrawArrays(GL_TRIANGLES, 0, 3);
            }
        }
    }
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);

    if (ms) { // resolve
        glBindFramebuffer(GL_READ_FRAMEBUFFER, msFbo_);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sceneFbo_);
        glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, sceneFbo_);
        readable = true;
        nanCheck("after resolve");
    } else {
        nanCheck("after sun");
    }

    // Bloom: bright parts at quarter size, blurred twice.
    const int bw = std::max(1, width / 4), bh = std::max(1, height / 4);
    glViewport(0, 0, bw, bh);
    glBindVertexArray(quadVao_);
    glBindFramebuffer(GL_FRAMEBUFFER, bloomFbo_[0]);
    glUseProgram(brightProg_);
    glUniform1i(glGetUniformLocation(brightProg_, "scene"), 0);
    glUniform1f(glGetUniformLocation(brightProg_, "threshold"), 0.8f);
    glBindTexture(GL_TEXTURE_2D, sceneTex_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glUseProgram(blurProg_);
    glUniform1i(glGetUniformLocation(blurProg_, "tex"), 0);
    for (int pass = 0; pass < 2; pass++)
        for (int axis = 0; axis < 2; axis++) {
            glBindFramebuffer(GL_FRAMEBUFFER, bloomFbo_[(axis + 1) & 1]);
            glBindTexture(GL_TEXTURE_2D, bloomTex_[axis & 1]);
            glUniform2f(glGetUniformLocation(blurProg_, "dir"), axis == 0 ? 1.0f / float(bw) : 0.0f,
                        axis == 1 ? 1.0f / float(bh) : 0.0f);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }

    // Exposure, bloom and tone mapping into the final image.
    glViewport(0, 0, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, finalFbo_);
    glUseProgram(toneProg_);
    glUniform1i(glGetUniformLocation(toneProg_, "scene"), 0);
    glUniform1i(glGetUniformLocation(toneProg_, "bloom"), 1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneTex_);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, bloomTex_[0]);
    glActiveTexture(GL_TEXTURE0);
    const float fwExposure = tweaks.firmwareShaders && fwReady_ ? kFwExposure : 1.0f;
    glUniform1f(glGetUniformLocation(toneProg_, "exposure"),
                p.exposure * tweaks.exposure * fwExposure * (1.0f + pulse_ * (tweaks.music == 2 ? 0.35f : 0.12f)));
    glUniform1f(glGetUniformLocation(toneProg_, "glare"), 0.35f * tweaks.glare * (1.0f + pulse_ * (tweaks.music == 2 ? 1.2f : 0.4f)));
    glUniform1f(glGetUniformLocation(toneProg_, "white"), std::max(p.white, 1.0f));
    glUniform1f(glGetUniformLocation(toneProg_, "hdr"), hdrOutput ? 1.0f : 0.0f);
    glUniform1f(glGetUniformLocation(toneProg_, "fade"), fade_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Earth::present(int width, int height, GLuint targetFbo, float dither)
{
    glBindFramebuffer(GL_READ_FRAMEBUFFER, finalFbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, targetFbo);
    if (!ready_ || !finalFbo_) {
        glBindFramebuffer(GL_FRAMEBUFFER, targetFbo);
        glViewport(0, 0, width, height);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }
    if (dither > 0) {
        dither::copy(finalTex_, width_, height_, targetFbo, width, height, dither, hdrOutput);
        return;
    }
    glBlitFramebuffer(0, 0, width_, height_, 0, 0, width, height, GL_COLOR_BUFFER_BIT,
                      width == width_ && height == height_ ? GL_NEAREST : GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, targetFbo);
}

void Earth::sceneColours(float main[3], float accent[3]) const
{
    const float ocean[3] = {0.12f, 0.3f, 0.7f}, sun[3] = {1.0f, 0.82f, 0.5f};
    for (int i = 0; i < 3; i++)
        main[i] = ocean[i], accent[i] = sun[i];
}
