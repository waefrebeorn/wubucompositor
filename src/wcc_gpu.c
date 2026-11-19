/*
 * wcc_gpu.c -- GPU present (GLX): upload the client's buffer to a texture,
 * draw a fullscreen quad. Step 2 of the monolith split; the first host-
 * bound leaf. A macOS backend replaces this concern with Metal without
 * touching the protocol logic.
 *
 * Rules (from the compositor skill): client bytes are B,G,R,00 — upload
 * GL_BGRA + GL_UNSIGNED_BYTE, blending OFF; GL_UNPACK_ROW_LENGTH for
 * non-tight strides; the GLX visual must be chosen BEFORE XCreateWindow
 * (that choice lives in the init path, not here).
 */
#include "wcc_internal.h"

#include <stdlib.h>
#include <string.h>

void wcc_gpu_destroy(struct wcc_server *s)
{
    if (!s->gpu.ok) return;
    glXMakeCurrent(s->xd, None, NULL);
    if (s->gpu.ctx) glXDestroyContext(s->xd, s->gpu.ctx);
    if (s->gpu.tex) glDeleteTextures(1, &s->gpu.tex);
    free(s->gpu.last);
    if (s->gpu.vi) XFree(s->gpu.vi);
    memset(&s->gpu, 0, sizeof s->gpu);
}

void wcc_gpu_setup(struct wcc_server *s)
{
    const int w = WC_OUTPUT_W, h = WC_OUTPUT_H;
    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, w, h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);

    glGenTextures(1, &s->gpu.tex);
    glBindTexture(GL_TEXTURE_2D, s->gpu.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA,
                 GL_UNSIGNED_BYTE, NULL);

    s->gpu.last = calloc((size_t)w * h * 4, 1);
    if (!s->gpu.last) { wcc_gpu_destroy(s); return; }
    s->gpu.ok = true;
}

void wcc_gpu_present(struct wcc_server *s, const void *data,
                        int w, int h, int stride)
{
    if (!s->gpu.ok) return;
    glBindTexture(GL_TEXTURE_2D, s->gpu.tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA,
                    GL_UNSIGNED_BYTE, data);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    glEnable(GL_TEXTURE_2D);
    glColor4f(1.f, 1.f, 1.f, 1.f);
    glBegin(GL_QUADS);
        glTexCoord2f(0.f, 0.f); glVertex2f(0.f, 0.f);
        glTexCoord2f(1.f, 0.f); glVertex2f((GLfloat)w, 0.f);
        glTexCoord2f(1.f, 1.f); glVertex2f((GLfloat)w, (GLfloat)h);
        glTexCoord2f(0.f, 1.f); glVertex2f(0.f, (GLfloat)h);
    glEnd();
    glDisable(GL_TEXTURE_2D);
    glXSwapBuffers(s->xd, s->xw);
}
