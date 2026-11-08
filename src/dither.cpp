#include "dither.h"
#include "cg.h"

namespace dither {
namespace {

const char *kVs = R"(#version 330 core
out vec2 uv;
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Triangular noise (sum of two uniform hashes, -1..1) per channel: the least visible noise that removes banding.
// Fixed per pixel (no shimmer between frames).
const char *kFs = R"(#version 330 core
uniform sampler2D src;
uniform float amount, linearOut, sameSize;
in vec2 uv;
out vec4 fragColor;
float hash(vec2 p) {
    vec3 q = fract(vec3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}
void main() {
    vec4 c = sameSize > 0.5 ? texelFetch(src, ivec2(gl_FragCoord.xy), 0) : texture(src, uv);
    vec2 p = gl_FragCoord.xy;
    vec3 n = vec3(hash(p) + hash(p + 17.13), hash(p + 41.7) + hash(p + 63.9), hash(p + 89.1) + hash(p + 7.77)) - 1.0;
    // (No grain on pure black, e.g. space, or on clipped white: nothing to band there.)
    if (linearOut > 0.5) { // HDR: linear light, quantized to ~10 bits later
        vec3 e = pow(max(c.rgb, 0.0), vec3(1.0 / 2.2));
        e += n * amount / 1023.0 * clamp(e * 1023.0 / 1.5, 0.0, 1.0);
        c.rgb = pow(max(e, 0.0), vec3(2.2));
    } else {
        c.rgb += n * amount / 255.0 * clamp(c.rgb * 255.0 / 1.5, 0.0, 1.0) * clamp((1.0 - c.rgb) * 255.0 / 1.5, 0.0, 1.0);
    }
    fragColor = c;
}
)";

GLuint prog = 0, vao = 0;
bool failed = false;

} // namespace

void copy(GLuint srcTex, int srcW, int srcH, GLuint targetFbo, int dstW, int dstH, float amount, bool linear)
{
    if (!prog && !failed) {
        prog = compileProgram(kVs, kFs, "dither");
        failed = !prog;
        glGenVertexArrays(1, &vao);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, targetFbo);
    glViewport(0, 0, dstW, dstH);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    if (!prog) { // (no shader: plain copy)
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "src"), 0);
    glUniform1f(glGetUniformLocation(prog, "amount"), amount);
    glUniform1f(glGetUniformLocation(prog, "linearOut"), linear ? 1.0f : 0.0f);
    glUniform1f(glGetUniformLocation(prog, "sameSize"), srcW == dstW && srcH == dstH ? 1.0f : 0.0f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, srcTex);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

void shutdown()
{
    if (prog)
        glDeleteProgram(prog);
    if (vao)
        glDeleteVertexArrays(1, &vao);
    prog = vao = 0;
}

} // namespace dither
