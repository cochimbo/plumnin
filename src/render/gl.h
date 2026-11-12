#pragma once

#include <cstddef>
#include <cstdint>

// The handful of OpenGL 3.3 core functions plumnin uses, loaded at run time through
// SDL so nothing has to link against opengl32 or libGL. Call them as gl::Clear(...).

#ifdef _WIN32
#define PLUMNIN_GLAPI __stdcall
#else
#define PLUMNIN_GLAPI
#endif

namespace plumnin::gl {

using GLenum = unsigned int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLint = int;
using GLuint = unsigned int;
using GLsizei = int;
using GLfloat = float;
using GLchar = char;
using GLsizeiptr = std::ptrdiff_t;

constexpr GLboolean FALSE_ = 0;
constexpr GLboolean TRUE_ = 1;
constexpr GLenum TRIANGLES = 0x0004;
constexpr GLenum TRIANGLE_STRIP = 0x0005;
constexpr GLenum DEPTH_BUFFER_BIT = 0x00000100;
constexpr GLenum COLOR_BUFFER_BIT = 0x00004000;
constexpr GLenum LEQUAL = 0x0203;
constexpr GLenum SRC_ALPHA = 0x0302;
constexpr GLenum ONE_MINUS_SRC_ALPHA = 0x0303;
constexpr GLenum CULL_FACE = 0x0B44;
constexpr GLenum DEPTH_TEST = 0x0B71;
constexpr GLenum BLEND = 0x0BE2;
constexpr GLenum SCISSOR_TEST = 0x0C11;
constexpr GLenum TEXTURE_2D = 0x0DE1;
constexpr GLenum UNSIGNED_BYTE = 0x1401;
constexpr GLenum UNSIGNED_INT = 0x1405;
constexpr GLenum FLOAT = 0x1406;
constexpr GLenum HALF_FLOAT = 0x140B;
constexpr GLenum DEPTH_COMPONENT = 0x1902;
constexpr GLenum RGBA = 0x1908;
constexpr GLenum VERSION = 0x1F02;
constexpr GLenum NEAREST = 0x2600;
constexpr GLenum LINEAR = 0x2601;
constexpr GLenum LINEAR_MIPMAP_LINEAR = 0x2703;
constexpr GLenum TEXTURE_MAG_FILTER = 0x2800;
constexpr GLenum TEXTURE_MIN_FILTER = 0x2801;
constexpr GLenum TEXTURE_WRAP_S = 0x2802;
constexpr GLenum TEXTURE_WRAP_T = 0x2803;
constexpr GLenum RGBA8 = 0x8058;
constexpr GLenum BGRA = 0x80E1;
constexpr GLenum CLAMP_TO_EDGE = 0x812F;
constexpr GLenum DEPTH_COMPONENT24 = 0x81A6;
constexpr GLenum UNSIGNED_INT_8_8_8_8_REV = 0x8367;
constexpr GLenum TEXTURE0 = 0x84C0;
constexpr GLenum RGBA16F = 0x881A;
constexpr GLenum ARRAY_BUFFER = 0x8892;
constexpr GLenum STATIC_DRAW = 0x88E4;
constexpr GLenum FRAGMENT_SHADER = 0x8B30;
constexpr GLenum VERTEX_SHADER = 0x8B31;
constexpr GLenum COMPILE_STATUS = 0x8B81;
constexpr GLenum LINK_STATUS = 0x8B82;
constexpr GLenum FRAMEBUFFER_COMPLETE = 0x8CD5;
constexpr GLenum COLOR_ATTACHMENT0 = 0x8CE0;
constexpr GLenum DEPTH_ATTACHMENT = 0x8D00;
constexpr GLenum FRAMEBUFFER = 0x8D40;
constexpr GLenum RENDERBUFFER = 0x8D41;
constexpr GLenum READ_FRAMEBUFFER = 0x8CA8;
constexpr GLenum DRAW_FRAMEBUFFER = 0x8CA9;

#define PLUMNIN_GL_FUNCTIONS(X)                                                                         \
    X(void, ActiveTexture, (GLenum))                                                                   \
    X(void, GenerateMipmap, (GLenum))                                                                  \
    X(void, AttachShader, (GLuint, GLuint))                                                            \
    X(void, BindBuffer, (GLenum, GLuint))                                                              \
    X(void, BindFramebuffer, (GLenum, GLuint))                                                         \
    X(void, BindRenderbuffer, (GLenum, GLuint))                                                        \
    X(void, BlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(void, DeleteRenderbuffers, (GLsizei, const GLuint*))                                             \
    X(void, FramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint))                                 \
    X(void, GenRenderbuffers, (GLsizei, GLuint*))                                                      \
    X(void, RenderbufferStorageMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei))               \
    X(void, BindTexture, (GLenum, GLuint))                                                             \
    X(void, BindVertexArray, (GLuint))                                                                 \
    X(void, BlendFunc, (GLenum, GLenum))                                                               \
    X(void, BufferData, (GLenum, GLsizeiptr, const void*, GLenum))                                     \
    X(GLenum, CheckFramebufferStatus, (GLenum))                                                        \
    X(void, Clear, (GLbitfield))                                                                       \
    X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                          \
    X(void, CompileShader, (GLuint))                                                                   \
    X(GLuint, CreateProgram, ())                                                                       \
    X(GLuint, CreateShader, (GLenum))                                                                  \
    X(void, DeleteBuffers, (GLsizei, const GLuint*))                                                   \
    X(void, DeleteFramebuffers, (GLsizei, const GLuint*))                                              \
    X(void, DeleteProgram, (GLuint))                                                                   \
    X(void, DeleteShader, (GLuint))                                                                    \
    X(void, DeleteTextures, (GLsizei, const GLuint*))                                                  \
    X(void, DeleteVertexArrays, (GLsizei, const GLuint*))                                              \
    X(void, DepthFunc, (GLenum))                                                                       \
    X(void, DepthMask, (GLboolean))                                                                    \
    X(void, Disable, (GLenum))                                                                         \
    X(void, DrawArrays, (GLenum, GLint, GLsizei))                                                      \
    X(void, DrawArraysInstanced, (GLenum, GLint, GLsizei, GLsizei))                                    \
    X(void, Enable, (GLenum))                                                                          \
    X(void, EnableVertexAttribArray, (GLuint))                                                         \
    X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))                             \
    X(void, GenBuffers, (GLsizei, GLuint*))                                                            \
    X(void, GenFramebuffers, (GLsizei, GLuint*))                                                       \
    X(void, GenTextures, (GLsizei, GLuint*))                                                           \
    X(void, GenVertexArrays, (GLsizei, GLuint*))                                                       \
    X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                   \
    X(void, GetProgramiv, (GLuint, GLenum, GLint*))                                                    \
    X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                    \
    X(void, GetShaderiv, (GLuint, GLenum, GLint*))                                                     \
    X(const unsigned char*, GetString, (GLenum))                                                       \
    X(GLint, GetUniformLocation, (GLuint, const GLchar*))                                              \
    X(void, LinkProgram, (GLuint))                                                                     \
    X(void, Scissor, (GLint, GLint, GLsizei, GLsizei))                                                 \
    X(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*))                         \
    X(void, ShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*))                       \
    X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*)) \
    X(void, TexParameteri, (GLenum, GLenum, GLint))                                                    \
    X(void, TexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*)) \
    X(void, Uniform1f, (GLint, GLfloat))                                                               \
    X(void, Uniform1i, (GLint, GLint))                                                                 \
    X(void, Uniform2f, (GLint, GLfloat, GLfloat))                                                      \
    X(void, Uniform2i, (GLint, GLint, GLint))                                                          \
    X(void, Uniform3f, (GLint, GLfloat, GLfloat, GLfloat))                                             \
    X(void, Uniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))                                    \
    X(void, UniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat*))                             \
    X(void, UseProgram, (GLuint))                                                                      \
    X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*))             \
    X(void, Viewport, (GLint, GLint, GLsizei, GLsizei))

#define PLUMNIN_GL_DECLARE(ret, name, args) extern ret(PLUMNIN_GLAPI* name) args;
PLUMNIN_GL_FUNCTIONS(PLUMNIN_GL_DECLARE)
#undef PLUMNIN_GL_DECLARE

// Loads every function above from the current context. Returns the name of the first
// one missing, or nullptr when all were found.
const char* load();

// Builds a program from GLSL 3.30 sources. On failure returns 0 and logs the error.
GLuint buildProgram(const char* vertexSource, const char* fragmentSource);

}  // namespace plumnin::gl
