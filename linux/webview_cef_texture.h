#ifndef WEBVIEW_CEF_TEXTURE_H_
#define WEBVIEW_CEF_TEXTURE_H_

#include <flutter_linux/flutter_linux.h>

#include <mutex>

#ifndef WEBVIEW_CEF_GPU_TEXTURE
#include <gtk/gtk.h>
#endif

#ifdef WEBVIEW_CEF_GPU_TEXTURE
#include "include/cef_render_handler.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <cstdint>
#include <unistd.h>
#include <vector>

struct WebviewCefTexture {
    FlTextureGL parent_instance;
    std::mutex *mutex = nullptr;
    int32_t width = 0;
    int32_t height = 0;
    uint64_t frame_serial = 0;
    uint64_t uploaded_serial = 0;
    GLuint texture_id = 0;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    int plane_count = 0;
    int fds[kAcceleratedPaintMaxPlanes] = {-1, -1, -1, -1};
    uint32_t strides[kAcceleratedPaintMaxPlanes] = {};
    uint64_t offsets[kAcceleratedPaintMaxPlanes] = {};
    uint64_t modifier = 0;
    cef_color_type_t format = CEF_COLOR_TYPE_RGBA_8888;
};

struct WebviewCefTextureClass {
    FlTextureGLClass parent_class;
};

G_DEFINE_TYPE(WebviewCefTexture,
              webview_cef_texture,
              fl_texture_gl_get_type())

#define WEBVIEW_CEF_TEXTURE(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST((obj), webview_cef_texture_get_type(), WebviewCefTexture))

using WebviewGlEglImageTargetTextureProc = void (*)(GLenum, EGLImageKHR);

static void webview_cef_texture_set_parameters() {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static void webview_cef_texture_close_fds(WebviewCefTexture *texture) {
    for (int index = 0; index < texture->plane_count; ++index) {
        if (texture->fds[index] >= 0) {
            close(texture->fds[index]);
            texture->fds[index] = -1;
        }
    }
    texture->plane_count = 0;
}

static void webview_cef_texture_destroy_image(WebviewCefTexture *texture) {
    if (texture->image == EGL_NO_IMAGE_KHR) {
        return;
    }
    auto destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
        eglGetProcAddress("eglDestroyImageKHR"));
    EGLDisplay display = eglGetCurrentDisplay();
    if (destroy_image != nullptr && display != EGL_NO_DISPLAY) {
        destroy_image(display, texture->image);
    }
    texture->image = EGL_NO_IMAGE_KHR;
}

static gboolean webview_cef_texture_populate(FlTextureGL *texture,
                                             uint32_t *target,
                                             uint32_t *name,
                                             uint32_t *width,
                                             uint32_t *height,
                                             GError **error) {
    (void)error;
    WebviewCefTexture *self = WEBVIEW_CEF_TEXTURE(texture);
    std::lock_guard<std::mutex> lock(*self->mutex);

    if (self->texture_id == 0) {
        glGenTextures(1, &self->texture_id);
        glBindTexture(GL_TEXTURE_2D, self->texture_id);
        webview_cef_texture_set_parameters();
        const uint32_t placeholder = 0;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, &placeholder);
        self->width = 1;
        self->height = 1;
    }

    if (self->plane_count != 1 || self->fds[0] < 0 || self->frame_serial == 0) {
        *target = GL_TEXTURE_2D;
        *name = self->texture_id;
        *width = static_cast<uint32_t>(self->width);
        *height = static_cast<uint32_t>(self->height);
        return TRUE;
    }

    if (self->uploaded_serial != self->frame_serial) {
        auto create_image = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
            eglGetProcAddress("eglCreateImageKHR"));
        auto destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
            eglGetProcAddress("eglDestroyImageKHR"));
        auto bind_image = reinterpret_cast<WebviewGlEglImageTargetTextureProc>(
            eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        if (create_image == nullptr || destroy_image == nullptr || bind_image == nullptr) {
            *target = GL_TEXTURE_2D;
            *name = self->texture_id;
            *width = static_cast<uint32_t>(self->width);
            *height = static_cast<uint32_t>(self->height);
            return TRUE;
        }

        const uint32_t drm_format = self->format == CEF_COLOR_TYPE_BGRA_8888
            ? 0x34325241u  // DRM_FORMAT_ARGB8888.
            : 0x34324241u; // DRM_FORMAT_ABGR8888.
        std::vector<EGLint> attributes = {
            EGL_WIDTH, self->width,
            EGL_HEIGHT, self->height,
            EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(drm_format),
            EGL_DMA_BUF_PLANE0_FD_EXT, self->fds[0],
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, static_cast<EGLint>(self->offsets[0]),
            EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(self->strides[0]),
        };
        if (self->modifier != 0) {
            attributes.insert(attributes.end(), {
                EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
                static_cast<EGLint>(self->modifier & 0xffffffffu),
                EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
                static_cast<EGLint>(self->modifier >> 32),
            });
        }
        attributes.push_back(EGL_NONE);

        EGLDisplay display = eglGetCurrentDisplay();
        EGLImageKHR image = create_image(display, EGL_NO_CONTEXT,
                                          EGL_LINUX_DMA_BUF_EXT, nullptr,
                                          attributes.data());
        if (image == EGL_NO_IMAGE_KHR) {
            *target = GL_TEXTURE_2D;
            *name = self->texture_id;
            *width = static_cast<uint32_t>(self->width);
            *height = static_cast<uint32_t>(self->height);
            return TRUE;
        }

        glBindTexture(GL_TEXTURE_2D, self->texture_id);
        webview_cef_texture_set_parameters();
        bind_image(GL_TEXTURE_2D, image);

        if (self->image != EGL_NO_IMAGE_KHR) {
            destroy_image(display, self->image);
        }
        self->image = image;
        self->uploaded_serial = self->frame_serial;
    }

    *target = GL_TEXTURE_2D;
    *name = self->texture_id;
    *width = static_cast<uint32_t>(self->width);
    *height = static_cast<uint32_t>(self->height);
    return TRUE;
}

static void webview_cef_texture_finalize(GObject *object) {
    WebviewCefTexture *self = WEBVIEW_CEF_TEXTURE(object);
    {
        std::lock_guard<std::mutex> lock(*self->mutex);
        webview_cef_texture_close_fds(self);
        webview_cef_texture_destroy_image(self);
    }
    delete self->mutex;
    self->mutex = nullptr;
    G_OBJECT_CLASS(webview_cef_texture_parent_class)->finalize(object);
}

static WebviewCefTexture *webview_cef_texture_new() {
    return WEBVIEW_CEF_TEXTURE(g_object_new(webview_cef_texture_get_type(), nullptr));
}

static void webview_cef_texture_class_init(WebviewCefTextureClass *klass) {
    FL_TEXTURE_GL_CLASS(klass)->populate = webview_cef_texture_populate;
    G_OBJECT_CLASS(klass)->finalize = webview_cef_texture_finalize;
}

static void webview_cef_texture_init(WebviewCefTexture *self) {
    self->mutex = new std::mutex();
}

#else

struct WebviewCefTexture {
    FlPixelBufferTexture parent_instance;
    uint8_t *buffer = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    std::mutex *mutex = nullptr;
};

struct WebviewCefTextureClass {
    FlPixelBufferTextureClass parent_class;
};

G_DEFINE_TYPE(WebviewCefTexture,
              webview_cef_texture,
              fl_pixel_buffer_texture_get_type())

#define WEBVIEW_CEF_TEXTURE(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST((obj), webview_cef_texture_get_type(), WebviewCefTexture))

static gboolean webview_cef_texture_copy_pixels(FlPixelBufferTexture *texture,
                                                const uint8_t **out_buffer,
                                                uint32_t *width,
                                                uint32_t *height,
                                                GError **error) {
    (void)error;
    WebviewCefTexture *self = WEBVIEW_CEF_TEXTURE(texture);
    std::lock_guard<std::mutex> lock(*self->mutex);
    *out_buffer = self->buffer;
    *width = self->width;
    *height = self->height;
    return TRUE;
}

static void webview_cef_texture_finalize(GObject *object) {
    WebviewCefTexture *self = WEBVIEW_CEF_TEXTURE(object);
    {
        std::lock_guard<std::mutex> lock(*self->mutex);
        delete[] self->buffer;
        self->buffer = nullptr;
    }
    delete self->mutex;
    self->mutex = nullptr;
    G_OBJECT_CLASS(webview_cef_texture_parent_class)->finalize(object);
}

static WebviewCefTexture *webview_cef_texture_new() {
    return WEBVIEW_CEF_TEXTURE(g_object_new(webview_cef_texture_get_type(), nullptr));
}

static void webview_cef_texture_class_init(WebviewCefTextureClass *klass) {
    FL_PIXEL_BUFFER_TEXTURE_CLASS(klass)->copy_pixels = webview_cef_texture_copy_pixels;
    G_OBJECT_CLASS(klass)->finalize = webview_cef_texture_finalize;
}

static void webview_cef_texture_init(WebviewCefTexture *self) {
    self->mutex = new std::mutex();
}

#endif

#endif // WEBVIEW_CEF_TEXTURE_H_