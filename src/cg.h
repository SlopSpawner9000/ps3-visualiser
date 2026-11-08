// Loader for PS3 CgBinaryProgram files (.vpo / .fpo) and a translator from
// RSX (NV40-class) vertex/fragment microcode to GLSL 3.30.
#pragma once

#include "firmware.h"
#include "gl.h"

#include <array>
#include <string>
#include <vector>

struct CgParam {
    uint32_t type = 0, res = 0, var = 0, dir = 0;
    int32_t resIndex = -1;
    std::string name, semantic;
    bool referenced = false;
    std::vector<uint32_t> embedded; // fragment programs: ucode offsets of the constant
    bool hasDefault = false;
    float def[4] = {};
};

class CgShader {
public:
    bool parse(const Bytes &data, const std::string &label);

    bool isVertex = false;
    std::string label;
    std::vector<CgParam> params;
    std::string glsl;

    // Vertex programs: constant register -> compact uniform slot (-1 = unused).
    std::vector<int> constSlot;
    int slotCount = 0;

    struct Sampler {
        int unit;
        bool is3D;
        std::string name;
    };
    std::vector<Sampler> samplers;   // fragment programs
    std::vector<std::string> uniforms; // fragment programs: embedded constants

private:
    Bytes data_;
    void translateVertex();
    void translateFragment();
};

// A linked GL program made from a translated fragment program and either a
// translated vertex program or a plain GLSL vertex shader.
class CgProgram {
public:
    bool link(const CgShader *vp, const char *customVertexGlsl, const CgShader &fp);
    void destroy();
    void use();

    // Sets a uniform by its Cg name (e.g. "_Line") in both stages.
    void set(const char *name, float x, float y = 0, float z = 0, float w = 0);
    // Row-major 4x4 matrix (rows go to consecutive constant registers).
    void setMatrix(const char *name, const float rows[16]);
    void setTexture(const char *name, GLenum target, GLuint tex);
    // Mirror the v coordinate of every lookup on this sampler.
    void setFlipY(const char *name, bool flip);
    void upload();

    GLuint id() const { return prog_; }

private:
    GLuint prog_ = 0;
    const CgShader *vp_ = nullptr;
    const CgShader *fp_ = nullptr;
    std::vector<std::array<float, 4>> vc_;
    GLint vcLoc_ = -1;
    struct Uni {
        std::string name;
        GLint loc;
    };
    std::vector<Uni> fpUni_;
    struct Tex {
        std::string name;
        int unit;
        GLint flipLoc;
        GLenum target = 0;
        GLuint tex = 0;
        bool flip = false;
    };
    std::vector<Tex> tex_;
};

GLuint compileProgram(const char *vs, const char *fs, const char *label);
