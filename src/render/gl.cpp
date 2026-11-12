#include "gl.h"

#include <SDL.h>

namespace plumnin::gl {

#define PLUMNIN_GL_DEFINE(ret, name, args) ret(PLUMNIN_GLAPI* name) args = nullptr;
PLUMNIN_GL_FUNCTIONS(PLUMNIN_GL_DEFINE)
#undef PLUMNIN_GL_DEFINE

const char* load() {
#define PLUMNIN_GL_LOAD(ret, name, args)                                              \
    name = reinterpret_cast<ret(PLUMNIN_GLAPI*) args>(SDL_GL_GetProcAddress("gl" #name)); \
    if (!name) return "gl" #name;
    PLUMNIN_GL_FUNCTIONS(PLUMNIN_GL_LOAD)
#undef PLUMNIN_GL_LOAD
    return nullptr;
}

namespace {

GLuint compile(GLenum type, const char* source) {
    GLuint shader = CreateShader(type);
    ShaderSource(shader, 1, &source, nullptr);
    CompileShader(shader);
    GLint ok = 0;
    GetShaderiv(shader, COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        GetShaderInfoLog(shader, sizeof log, nullptr, log);
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "shader: %s", log);
        DeleteShader(shader);
        return 0;
    }
    return shader;
}

}  // namespace

GLuint buildProgram(const char* vertexSource, const char* fragmentSource) {
    const GLuint vs = compile(VERTEX_SHADER, vertexSource);
    const GLuint fs = compile(FRAGMENT_SHADER, fragmentSource);
    GLuint program = 0;
    if (vs && fs) {
        program = CreateProgram();
        AttachShader(program, vs);
        AttachShader(program, fs);
        LinkProgram(program);
        GLint ok = 0;
        GetProgramiv(program, LINK_STATUS, &ok);
        if (!ok) {
            char log[1024];
            GetProgramInfoLog(program, sizeof log, nullptr, log);
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "program: %s", log);
            DeleteProgram(program);
            program = 0;
        }
    }
    if (vs) DeleteShader(vs);
    if (fs) DeleteShader(fs);
    return program;
}

}  // namespace plumnin::gl
