#include "gl.h"

#include <SDL3/SDL.h>

#define PS3VIS_GL_DEFINE(ret, name, ...) PFN_##name name = nullptr;
PS3VIS_GL_FUNCS(PS3VIS_GL_DEFINE)
#undef PS3VIS_GL_DEFINE

namespace gl {

bool load()
{
    bool ok = true;
#define PS3VIS_GL_LOAD(ret, name, ...)                                              \
    name = reinterpret_cast<PFN_##name>(SDL_GL_GetProcAddress(#name));             \
    if (!name) {                                                                    \
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "missing GL function " #name);  \
        ok = false;                                                                 \
    }
    PS3VIS_GL_FUNCS(PS3VIS_GL_LOAD)
#undef PS3VIS_GL_LOAD
    return ok;
}

} // namespace gl
