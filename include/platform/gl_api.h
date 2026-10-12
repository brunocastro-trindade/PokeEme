#ifndef GUARD_PLATFORM_GL_API_H
#define GUARD_PLATFORM_GL_API_H

// Minimal OpenGL ES 3.0 binding. The few types, constants and functions the
// renderer needs are declared here and loaded at runtime through SDL, so the
// same code builds on Linux, Windows and the Switch without GL headers.

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#if defined(_WIN32) && !defined(_WIN64)
#define GL_APIENTRY __stdcall
#else
#define GL_APIENTRY
#endif

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef float GLfloat;
typedef char GLchar;
typedef unsigned char GLubyte;
typedef ptrdiff_t GLintptr;
typedef ptrdiff_t GLsizeiptr;

#define GL_NO_ERROR                 0
#define GL_FALSE                    0
#define GL_TRUE                     1
#define GL_TRIANGLES                0x0004
#define GL_ONE                      1
#define GL_SRC_ALPHA                0x0302
#define GL_ONE_MINUS_SRC_ALPHA      0x0303
#define GL_CULL_FACE                0x0B44
#define GL_DEPTH_TEST               0x0B71
#define GL_BLEND                    0x0BE2
#define GL_SCISSOR_TEST             0x0C11
#define GL_UNPACK_ALIGNMENT         0x0CF5
#define GL_PACK_ALIGNMENT           0x0D05
#define GL_TEXTURE_2D               0x0DE1
#define GL_UNSIGNED_BYTE            0x1401
#define GL_UNSIGNED_SHORT           0x1403
#define GL_FLOAT                    0x1406
#define GL_RED                      0x1903
#define GL_RGBA                     0x1908
#define GL_VENDOR                   0x1F00
#define GL_RENDERER                 0x1F01
#define GL_VERSION                  0x1F02
#define GL_NEAREST                  0x2600
#define GL_LINEAR                   0x2601
#define GL_TEXTURE_MAG_FILTER       0x2800
#define GL_TEXTURE_MIN_FILTER       0x2801
#define GL_TEXTURE_WRAP_S           0x2802
#define GL_TEXTURE_WRAP_T           0x2803
#define GL_COLOR_BUFFER_BIT         0x00004000
#define GL_DEPTH_BUFFER_BIT         0x00000100
#define GL_CLAMP_TO_EDGE            0x812F
#define GL_TEXTURE0                 0x84C0
#define GL_TEXTURE_2D_ARRAY         0x8C1A
#define GL_RGBA8                    0x8058
#define GL_R8UI                     0x8232
#define GL_R16UI                    0x8234
#define GL_RGBA16UI                 0x8D76
#define GL_RED_INTEGER              0x8D94
#define GL_RGBA_INTEGER             0x8D99
#define GL_FRAGMENT_SHADER          0x8B30
#define GL_VERTEX_SHADER            0x8B31
#define GL_COMPILE_STATUS           0x8B81
#define GL_LINK_STATUS              0x8B82
#define GL_INFO_LOG_LENGTH          0x8B84
#define GL_FRAMEBUFFER              0x8D40
#define GL_READ_FRAMEBUFFER         0x8CA8
#define GL_DRAW_FRAMEBUFFER         0x8CA9
#define GL_COLOR_ATTACHMENT0        0x8CE0
#define GL_COLOR_ATTACHMENT1        0x8CE1
#define GL_COLOR                    0x1800
#define GL_R8                       0x8229
#define GL_RGBA8UI                  0x8D7C
#define GL_LESS                     0x0201
#define GL_LEQUAL                   0x0203
#define GL_ARRAY_BUFFER             0x8892
#define GL_STATIC_DRAW              0x88E4
#define GL_DYNAMIC_DRAW             0x88E8
#define GL_RENDERBUFFER             0x8D41
#define GL_DEPTH_COMPONENT24        0x81A6
#define GL_DEPTH_ATTACHMENT         0x8D00
#define GL_FRAMEBUFFER_COMPLETE     0x8CD5
#define GL_NONE                     0
#define GL_UNSIGNED_INT             0x1405
#define GL_DEPTH_COMPONENT          0x1902
#define GL_MAX_TEXTURE_SIZE         0x0D33
#define GL_MAX_RENDERBUFFER_SIZE    0x84E8
#define GL_POLYGON_OFFSET_FILL      0x8037
#define GL_TEXTURE_COMPARE_MODE     0x884C
#define GL_TEXTURE_COMPARE_FUNC     0x884D
#define GL_COMPARE_REF_TO_TEXTURE   0x884E

// X-macro list: return type, name, parameters
#define GL_FUNCTION_LIST(X) \
    X(const GLubyte *, glGetString, (GLenum name)) \
    X(GLenum, glGetError, (void)) \
    X(void, glViewport, (GLint x, GLint y, GLsizei width, GLsizei height)) \
    X(void, glScissor, (GLint x, GLint y, GLsizei width, GLsizei height)) \
    X(void, glClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a)) \
    X(void, glClear, (GLbitfield mask)) \
    X(void, glEnable, (GLenum cap)) \
    X(void, glDisable, (GLenum cap)) \
    X(void, glBlendFunc, (GLenum sfactor, GLenum dfactor)) \
    X(void, glFinish, (void)) \
    X(void, glPixelStorei, (GLenum pname, GLint param)) \
    X(void, glGenTextures, (GLsizei n, GLuint *textures)) \
    X(void, glDeleteTextures, (GLsizei n, const GLuint *textures)) \
    X(void, glBindTexture, (GLenum target, GLuint texture)) \
    X(void, glActiveTexture, (GLenum texture)) \
    X(void, glTexParameteri, (GLenum target, GLenum pname, GLint param)) \
    X(void, glTexImage2D, (GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels)) \
    X(void, glTexSubImage2D, (GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels)) \
    X(void, glTexImage3D, (GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, const void *pixels)) \
    X(void, glTexSubImage3D, (GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void *pixels)) \
    X(GLuint, glCreateShader, (GLenum type)) \
    X(void, glShaderSource, (GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length)) \
    X(void, glCompileShader, (GLuint shader)) \
    X(void, glGetShaderiv, (GLuint shader, GLenum pname, GLint *params)) \
    X(void, glGetShaderInfoLog, (GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog)) \
    X(void, glDeleteShader, (GLuint shader)) \
    X(GLuint, glCreateProgram, (void)) \
    X(void, glAttachShader, (GLuint program, GLuint shader)) \
    X(void, glLinkProgram, (GLuint program)) \
    X(void, glGetProgramiv, (GLuint program, GLenum pname, GLint *params)) \
    X(void, glGetProgramInfoLog, (GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog)) \
    X(void, glUseProgram, (GLuint program)) \
    X(void, glDeleteProgram, (GLuint program)) \
    X(GLint, glGetUniformLocation, (GLuint program, const GLchar *name)) \
    X(void, glUniform1i, (GLint location, GLint v0)) \
    X(void, glUniform2f, (GLint location, GLfloat v0, GLfloat v1)) \
    X(void, glGenVertexArrays, (GLsizei n, GLuint *arrays)) \
    X(void, glBindVertexArray, (GLuint array)) \
    X(void, glDeleteVertexArrays, (GLsizei n, const GLuint *arrays)) \
    X(void, glDrawArrays, (GLenum mode, GLint first, GLsizei count)) \
    X(void, glGenFramebuffers, (GLsizei n, GLuint *framebuffers)) \
    X(void, glDeleteFramebuffers, (GLsizei n, const GLuint *framebuffers)) \
    X(void, glBindFramebuffer, (GLenum target, GLuint framebuffer)) \
    X(void, glFramebufferTexture2D, (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum target)) \
    X(void, glReadPixels, (GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels)) \
    X(void, glDrawBuffers, (GLsizei n, const GLenum *bufs)) \
    X(void, glClearBufferuiv, (GLenum buffer, GLint drawbuffer, const GLuint *value)) \
    X(void, glClearBufferfv, (GLenum buffer, GLint drawbuffer, const GLfloat *value)) \
    X(void, glUniform1f, (GLint location, GLfloat v0)) \
    X(void, glUniform4f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3)) \
    X(void, glUniform2fv, (GLint location, GLsizei count, const GLfloat *value)) \
    X(void, glUniform3f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2)) \
    X(void, glUniformMatrix4fv, (GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)) \
    X(void, glGenBuffers, (GLsizei n, GLuint *buffers)) \
    X(void, glDeleteBuffers, (GLsizei n, const GLuint *buffers)) \
    X(void, glBindBuffer, (GLenum target, GLuint buffer)) \
    X(void, glBufferData, (GLenum target, GLsizeiptr size, const void *data, GLenum usage)) \
    X(void, glBufferSubData, (GLenum target, GLintptr offset, GLsizeiptr size, const void *data)) \
    X(void, glVertexAttribPointer, (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer)) \
    X(void, glVertexAttribIPointer, (GLuint index, GLint size, GLenum type, GLsizei stride, const void *pointer)) \
    X(void, glEnableVertexAttribArray, (GLuint index)) \
    X(void, glDepthFunc, (GLenum func)) \
    X(void, glDepthMask, (GLboolean flag)) \
    X(void, glClearDepthf, (GLfloat d)) \
    X(void, glGenRenderbuffers, (GLsizei n, GLuint *renderbuffers)) \
    X(void, glDeleteRenderbuffers, (GLsizei n, const GLuint *renderbuffers)) \
    X(void, glBindRenderbuffer, (GLenum target, GLuint renderbuffer)) \
    X(void, glRenderbufferStorage, (GLenum target, GLenum internalformat, GLsizei width, GLsizei height)) \
    X(void, glFramebufferRenderbuffer, (GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer)) \
    X(void, glPolygonOffset, (GLfloat factor, GLfloat units)) \
    X(void, glGetIntegerv, (GLenum pname, GLint *data)) \
    X(void, glReadBuffer, (GLenum src))

// The pointers are prefixed so they never clash with a GL library that is
// linked statically (the Switch links libglapi); the macros below keep the
// usual names at call sites.
#define GL_DECLARE_POINTER(ret, name, params) extern ret (GL_APIENTRY *pe_##name) params;
GL_FUNCTION_LIST(GL_DECLARE_POINTER)
#undef GL_DECLARE_POINTER

#define glGetString pe_glGetString
#define glGetError pe_glGetError
#define glViewport pe_glViewport
#define glScissor pe_glScissor
#define glClearColor pe_glClearColor
#define glClear pe_glClear
#define glEnable pe_glEnable
#define glDisable pe_glDisable
#define glBlendFunc pe_glBlendFunc
#define glFinish pe_glFinish
#define glPixelStorei pe_glPixelStorei
#define glGenTextures pe_glGenTextures
#define glDeleteTextures pe_glDeleteTextures
#define glBindTexture pe_glBindTexture
#define glActiveTexture pe_glActiveTexture
#define glTexParameteri pe_glTexParameteri
#define glTexImage2D pe_glTexImage2D
#define glTexSubImage2D pe_glTexSubImage2D
#define glTexImage3D pe_glTexImage3D
#define glTexSubImage3D pe_glTexSubImage3D
#define glCreateShader pe_glCreateShader
#define glShaderSource pe_glShaderSource
#define glCompileShader pe_glCompileShader
#define glGetShaderiv pe_glGetShaderiv
#define glGetShaderInfoLog pe_glGetShaderInfoLog
#define glDeleteShader pe_glDeleteShader
#define glCreateProgram pe_glCreateProgram
#define glAttachShader pe_glAttachShader
#define glLinkProgram pe_glLinkProgram
#define glGetProgramiv pe_glGetProgramiv
#define glGetProgramInfoLog pe_glGetProgramInfoLog
#define glUseProgram pe_glUseProgram
#define glDeleteProgram pe_glDeleteProgram
#define glGetUniformLocation pe_glGetUniformLocation
#define glUniform1i pe_glUniform1i
#define glUniform2f pe_glUniform2f
#define glGenVertexArrays pe_glGenVertexArrays
#define glBindVertexArray pe_glBindVertexArray
#define glDeleteVertexArrays pe_glDeleteVertexArrays
#define glDrawArrays pe_glDrawArrays
#define glGenFramebuffers pe_glGenFramebuffers
#define glDeleteFramebuffers pe_glDeleteFramebuffers
#define glBindFramebuffer pe_glBindFramebuffer
#define glFramebufferTexture2D pe_glFramebufferTexture2D
#define glCheckFramebufferStatus pe_glCheckFramebufferStatus
#define glReadPixels pe_glReadPixels
#define glDrawBuffers pe_glDrawBuffers
#define glClearBufferuiv pe_glClearBufferuiv
#define glClearBufferfv pe_glClearBufferfv
#define glUniform1f pe_glUniform1f
#define glUniform4f pe_glUniform4f
#define glUniform2fv pe_glUniform2fv
#define glUniform3f pe_glUniform3f
#define glUniformMatrix4fv pe_glUniformMatrix4fv
#define glGenBuffers pe_glGenBuffers
#define glDeleteBuffers pe_glDeleteBuffers
#define glBindBuffer pe_glBindBuffer
#define glBufferData pe_glBufferData
#define glBufferSubData pe_glBufferSubData
#define glVertexAttribPointer pe_glVertexAttribPointer
#define glVertexAttribIPointer pe_glVertexAttribIPointer
#define glEnableVertexAttribArray pe_glEnableVertexAttribArray
#define glDepthFunc pe_glDepthFunc
#define glDepthMask pe_glDepthMask
#define glClearDepthf pe_glClearDepthf
#define glGenRenderbuffers pe_glGenRenderbuffers
#define glDeleteRenderbuffers pe_glDeleteRenderbuffers
#define glBindRenderbuffer pe_glBindRenderbuffer
#define glRenderbufferStorage pe_glRenderbufferStorage
#define glFramebufferRenderbuffer pe_glFramebufferRenderbuffer
#define glPolygonOffset pe_glPolygonOffset
#define glGetIntegerv pe_glGetIntegerv
#define glReadBuffer pe_glReadBuffer

// Loads every function with getProc (e.g. SDL_GL_GetProcAddress).
// Returns false and names the first missing function on failure.
bool GlApi_Load(void *(*getProc)(const char *name), const char **missing);

#endif // GUARD_PLATFORM_GL_API_H
