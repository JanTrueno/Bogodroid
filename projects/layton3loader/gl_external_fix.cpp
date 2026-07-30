// gl_external_fix.cpp -- make the engine's SurfaceTexture movie path work
// against an ordinary GL_TEXTURE_2D.
//
// On Android the cutscene texture comes from a SurfaceTexture, so the engine
// addresses it as GL_TEXTURE_EXTERNAL_OES and its movie shader samples a
// samplerExternalOES. There is no SurfaceTexture here -- movie.cpp uploads
// decoded frames into a plain 2D texture -- so those calls have to be
// redirected, or the driver rejects them (GL_INVALID_OPERATION on the upload,
// and a fragment shader that will not compile) and the movie draws black.
//
// These hooks are registered ahead of symtable_gles2 in so_dynamic_libraries,
// so the game resolves them instead of the real entry points.
// Ported from reference/layton_nx-main/source/imports.c.

#include <string.h>

#include "glad.h"
#include "so_util.h"

#define GL_TEXTURE_EXTERNAL_OES_ENUM 0x8D65

static void glBindTexture_hook(GLenum target, GLuint tex)
{
    if (target == GL_TEXTURE_EXTERNAL_OES_ENUM)
        target = GL_TEXTURE_2D;
    glBindTexture(target, tex);
}

static void glTexParameteri_hook(GLenum target, GLenum pname, GLint param)
{
    if (target == GL_TEXTURE_EXTERNAL_OES_ENUM)
        target = GL_TEXTURE_2D;
    glTexParameteri(target, pname, param);
}

// GL_TEXTURE_EXTERNAL_OES is not a valid capability; enabling it would just
// raise GL_INVALID_ENUM
static void glEnable_hook(GLenum cap)
{
    if (cap != GL_TEXTURE_EXTERNAL_OES_ENUM)
        glEnable(cap);
}

static void glDisable_hook(GLenum cap)
{
    if (cap != GL_TEXTURE_EXTERNAL_OES_ENUM)
        glDisable(cap);
}

// The uniform names are kept identical, so the engine's glGetUniformLocation
// lookups still resolve against the patched program.
static const char *patched_frag =
    "precision highp float;"
    "uniform sampler2D texture;uniform vec4 color;varying vec2 vary_uv;"
    "void main(){ gl_FragColor = texture2D(texture, vary_uv) * color; }";

static void glShaderSource_hook(GLuint shader, GLsizei count,
                                const GLchar *const *string, const GLint *length)
{
    if (count >= 1 && string && string[0] && strstr(string[0], "samplerExternalOES"))
        glShaderSource(shader, 1, &patched_frag, NULL);
    else
        glShaderSource(shader, count, string, length);
}

DynLibFunction symtable_movie_gl[] = {
    {"glBindTexture", (uintptr_t)&glBindTexture_hook},
    {"glTexParameteri", (uintptr_t)&glTexParameteri_hook},
    {"glEnable", (uintptr_t)&glEnable_hook},
    {"glDisable", (uintptr_t)&glDisable_hook},
    {"glShaderSource", (uintptr_t)&glShaderSource_hook},
    {NULL, (uintptr_t)NULL},
};
