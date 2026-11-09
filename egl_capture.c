#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "drm.h"
#include "egl_capture.h"
#include "screen_layout.h"

extern struct kmsvnc_data *kmsvnc;

#define MAX_OUTPUTS 16
struct captured_output {
    uint32_t connector_id;
    uint32_t plane_id;
    char name[32];
    int x, y, width, height;
    int native_width, native_height;
    char *pixels;
};

static struct {
    struct gbm_device *gbm;
    int gbm_fd;
    EGLDisplay display;
    EGLContext context;
    GLuint texture;
    GLuint framebuffer;
    PFNEGLCREATEIMAGEKHRPROC create_image;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC bind_image;
    int failed;
    int count;
    int width, height;
    struct captured_output outputs[MAX_OUTPUTS];
    char desktop_name[640];
} capture;

static int extension(const char *extensions, const char *name)
{
    size_t length = strlen(name);
    const char *found = extensions;
    while (found && (found = strstr(found, name))) {
        if ((found == extensions || found[-1] == ' ') &&
            (found[length] == ' ' || found[length] == '\0')) return 1;
        found += length;
    }
    return 0;
}

void egl_capture_cleanup(void)
{
    if (capture.display != EGL_NO_DISPLAY) {
        if (capture.context != EGL_NO_CONTEXT) {
            eglMakeCurrent(capture.display, EGL_NO_SURFACE, EGL_NO_SURFACE, capture.context);
            if (capture.framebuffer) glDeleteFramebuffers(1, &capture.framebuffer);
            if (capture.texture) glDeleteTextures(1, &capture.texture);
            eglMakeCurrent(capture.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroyContext(capture.display, capture.context);
        }
        eglTerminate(capture.display);
    }
    if (capture.gbm) gbm_device_destroy(capture.gbm);
    if (capture.gbm_fd > 0) close(capture.gbm_fd);
    for (int n = 0; n < capture.count; n++) free(capture.outputs[n].pixels);
    memset(&capture, 0, sizeof(capture));
}

/* GETFB2 выдаёт собственные GEM handles: закрываем каждый уникальный handle. */
static void close_handles(drmModeFB2 *fb)
{
    for (int n = 0; n < 4; n++) {
        if (!fb->handles[n]) continue;
        int duplicate = 0;
        for (int k = 0; k < n; k++) if (fb->handles[k] == fb->handles[n]) duplicate = 1;
        if (!duplicate) {
            struct drm_gem_close close_arg = { .handle = fb->handles[n] };
            drmIoctl(kmsvnc->drm->drm_fd, DRM_IOCTL_GEM_CLOSE, &close_arg);
        }
    }
}

/* Ключи атрибутов EGL для каждой плоскости буфера. */
static const EGLint plane_keys[4][5] = {
    { EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE0_PITCH_EXT,
      EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT },
    { EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
      EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT },
    { EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT,
      EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT },
    { EGL_DMA_BUF_PLANE3_FD_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT,
      EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT },
};

/* Причина последней неудачи чтения кадра (для журнала). */
static char frame_error[128];

static int read_frame(drmModeFB2 *fb, char *output)
{
    /*
     * У Intel со сжатием (CCS) у буфера несколько плоскостей, часто с общим
     * GEM handle: каждая плоскость получает свой PRIME fd того же dma-buf.
     */
    int primes[4] = { -1, -1, -1, -1 };
    EGLint attributes[6 + 4 * 10 + 1];
    int count = 0, planes = 0;
    attributes[count++] = EGL_WIDTH; attributes[count++] = fb->width;
    attributes[count++] = EGL_HEIGHT; attributes[count++] = fb->height;
    attributes[count++] = EGL_LINUX_DRM_FOURCC_EXT; attributes[count++] = fb->pixel_format;
    for (; planes < 4 && fb->handles[planes]; planes++) {
        if (drmPrimeHandleToFD(kmsvnc->drm->drm_fd, fb->handles[planes], O_CLOEXEC, &primes[planes])) break;
        attributes[count++] = plane_keys[planes][0]; attributes[count++] = primes[planes];
        attributes[count++] = plane_keys[planes][1]; attributes[count++] = fb->offsets[planes];
        attributes[count++] = plane_keys[planes][2]; attributes[count++] = fb->pitches[planes];
        attributes[count++] = plane_keys[planes][3]; attributes[count++] = (EGLint)(fb->modifier & 0xffffffff);
        attributes[count++] = plane_keys[planes][4]; attributes[count++] = (EGLint)(fb->modifier >> 32);
    }
    attributes[count] = EGL_NONE;
    int exported = planes && (planes == 4 || !fb->handles[planes]);
    EGLImageKHR image = exported ? capture.create_image(capture.display, EGL_NO_CONTEXT,
        EGL_LINUX_DMA_BUF_EXT, NULL, attributes) : EGL_NO_IMAGE_KHR;
    EGLint image_error = image == EGL_NO_IMAGE_KHR ? eglGetError() : EGL_SUCCESS;
    for (int n = 0; n < 4; n++) if (primes[n] >= 0) close(primes[n]);
    if (!exported) {
        snprintf(frame_error, sizeof(frame_error), "PRIME export of plane %d failed: %s", planes, strerror(errno));
        return 1;
    }
    if (image == EGL_NO_IMAGE_KHR) {
        snprintf(frame_error, sizeof(frame_error), "eglCreateImage failed (EGL 0x%x, fourcc %.4s, modifier 0x%llx, planes %d)",
            image_error, (char *)&fb->pixel_format, (unsigned long long)fb->modifier, planes);
        return 1;
    }
    glBindTexture(GL_TEXTURE_2D, capture.texture);
    capture.bind_image(GL_TEXTURE_2D, image);
    glBindFramebuffer(GL_FRAMEBUFFER, capture.framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, capture.texture, 0);
    GLenum gl_error = glGetError();
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    int failed = gl_error != GL_NO_ERROR || status != GL_FRAMEBUFFER_COMPLETE;
    if (failed) {
        snprintf(frame_error, sizeof(frame_error), "framebuffer not usable (GL 0x%x, status 0x%x)", gl_error, status);
    }
    else {
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, fb->width, fb->height, GL_RGBA, GL_UNSIGNED_BYTE, output);
        gl_error = glGetError();
        failed = gl_error != GL_NO_ERROR;
        if (failed) snprintf(frame_error, sizeof(frame_error), "glReadPixels failed (GL 0x%x)", gl_error);
    }
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    capture.destroy_image(capture.display, image);
    return failed;
}

static const char *connector_type(uint32_t type)
{
    switch (type) {
        case DRM_MODE_CONNECTOR_HDMIA: return "HDMI-A";
        case DRM_MODE_CONNECTOR_HDMIB: return "HDMI-B";
        case DRM_MODE_CONNECTOR_DisplayPort: return "DP";
        case DRM_MODE_CONNECTOR_eDP: return "eDP";
        case DRM_MODE_CONNECTOR_DVID: return "DVI-D";
        case DRM_MODE_CONNECTOR_DVII: return "DVI-I";
        case DRM_MODE_CONNECTOR_DVIA: return "DVI-A";
        case DRM_MODE_CONNECTOR_VGA: return "VGA";
        default: return "Unknown";
    }
}

static int primary_plane(drmModePlane *plane)
{
    int primary = 0;
    drmModeObjectProperties *properties = drmModeObjectGetProperties(
        kmsvnc->drm->drm_fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
    if (!properties) return 0;
    for (int n = 0; n < properties->count_props; n++) {
        drmModePropertyRes *property = drmModeGetProperty(kmsvnc->drm->drm_fd, properties->props[n]);
        if (property) {
            if (!strcmp(property->name, "type") && properties->prop_values[n] == DRM_PLANE_TYPE_PRIMARY) primary = 1;
            drmModeFreeProperty(property);
        }
    }
    drmModeFreeObjectProperties(properties);
    return primary;
}

/* KMS хранит локальную геометрию scanout; расположение мониторов задаёт композитор. */
static int compositor_layout(void)
{
    struct kmsvnc_output_layout outputs[MAX_OUTPUTS];
    int count = screen_layout_read(outputs, MAX_OUTPUTS);
    int matched[MAX_OUTPUTS] = {0};
    for (int k = 0; k < count; k++) {
        for (int n = 0; n < capture.count; n++) {
            struct captured_output *out = &capture.outputs[n];
            if (strcmp(out->name, outputs[k].name)) continue;
            out->x = outputs[k].x; out->y = outputs[k].y;
            out->width = outputs[k].width; out->height = outputs[k].height;
            matched[n] = 1;
        }
    }
    for (int n = 0; n < capture.count; n++) if (!matched[n]) return 1;
    return 0;
}

static int output_order(const void *a, const void *b)
{
    const struct captured_output *first = a, *second = b;
    if (first->x != second->x) return first->x - second->x;
    return first->y - second->y;
}

static int prepare_outputs(void)
{
    int fd = kmsvnc->drm->drm_fd;
    drmModeRes *resources = drmModeGetResources(fd);
    drmModePlaneRes *planes = drmModeGetPlaneResources(fd);
    if (!resources || !planes) {
        if (resources) drmModeFreeResources(resources);
        if (planes) drmModeFreePlaneResources(planes);
        return 1;
    }
    for (int n = 0; n < resources->count_connectors && capture.count < MAX_OUTPUTS; n++) {
        drmModeConnector *connector = drmModeGetConnector(fd, resources->connectors[n]);
        if (!connector) continue;
        drmModeEncoder *encoder = connector->encoder_id ? drmModeGetEncoder(fd, connector->encoder_id) : NULL;
        if (connector->connection == DRM_MODE_CONNECTED && encoder && encoder->crtc_id) {
            for (int k = 0; k < planes->count_planes; k++) {
                drmModePlane *plane = drmModeGetPlane(fd, planes->planes[k]);
                if (!plane) continue;
                int chosen = !kmsvnc->source_plane || plane->plane_id == kmsvnc->source_plane;
                chosen &= !kmsvnc->source_crtc || plane->crtc_id == kmsvnc->source_crtc;
                if (chosen && plane->crtc_id == encoder->crtc_id && plane->fb_id && primary_plane(plane)) {
                    drmModeFB2 *fb = drmModeGetFB2(fd, plane->fb_id);
                    if (fb) {
                        struct captured_output *out = &capture.outputs[capture.count++];
                        out->connector_id = connector->connector_id;
                        out->plane_id = plane->plane_id;
                        snprintf(out->name, sizeof(out->name), "%s-%u", connector_type(connector->connector_type), connector->connector_type_id);
                        out->native_width = out->width = fb->width;
                        out->native_height = out->height = fb->height;
                        close_handles(fb);
                        drmModeFreeFB2(fb);
                    }
                    drmModeFreePlane(plane);
                    break;
                }
                drmModeFreePlane(plane);
            }
        }
        if (encoder) drmModeFreeEncoder(encoder);
        drmModeFreeConnector(connector);
    }
    drmModeFreeResources(resources);
    drmModeFreePlaneResources(planes);
    if (!capture.count) return 1;
    if (!kmsvnc->source_plane && !kmsvnc->source_crtc) {
        if (compositor_layout() && capture.count > 1) {
            fprintf(stderr, "Cannot determine monitor layout; specify --source-plane or --source-crtc\n");
            return 1;
        }
    }
    if (capture.count == 1) capture.outputs[0].x = capture.outputs[0].y = 0;
    qsort(capture.outputs, capture.count, sizeof(capture.outputs[0]), output_order);
    strcpy(capture.desktop_name, "kmsvnc [");
    for (int n = 0; n < capture.count; n++) {
        struct captured_output *out = &capture.outputs[n];
        if (out->native_width <= 0 || out->native_width > 16384 || out->native_height <= 0 || out->native_height > 16384) return 1;
        size_t size = (size_t)out->native_width * out->native_height * 4;
        if (size > 256 * 1024 * 1024) return 1;
        out->pixels = calloc(1, size);
        if (!out->pixels) return 1;
        if (out->x + out->width > capture.width) capture.width = out->x + out->width;
        if (out->y + out->height > capture.height) capture.height = out->y + out->height;
        strcat(capture.desktop_name, n ? ", " : "");
        strcat(capture.desktop_name, out->name);
        printf("Output %d: %s plane %u region %dx%d+%d+%d\n", n + 1, out->name, out->plane_id, out->width, out->height, out->x, out->y);
    }
    strcat(capture.desktop_name, "]");
    if (capture.width <= 0 || capture.height <= 0 || (size_t)capture.width * capture.height * 4 > 256 * 1024 * 1024) return 1;
    return 0;
}

static int read_output(struct captured_output *out)
{
    int fd = kmsvnc->drm->drm_fd;
    drmModePlane *plane = drmModeGetPlane(fd, out->plane_id);
    if (!plane) return 1;
    uint32_t fb_id = plane->fb_id;
    drmModeFreePlane(plane);
    /* При DPMS оставляем последний кадр именно этого монитора. */
    if (!fb_id) return 0;
    drmModeFB2 *fb = drmModeGetFB2(fd, fb_id);
    if (!fb) return 1;
    int failed = fb->width != out->native_width || fb->height != out->native_height ||
        read_frame(fb, out->pixels);
    close_handles(fb);
    drmModeFreeFB2(fb);
    return failed;
}

/* Render node устройства (как у VAAPI), иначе сама карта, но с правом записи. */
static int open_gbm_device(void)
{
    char *render_node = drmGetRenderDeviceNameFromFd(kmsvnc->drm->drm_fd);
    int fd = render_node ? open(render_node, O_RDWR | O_CLOEXEC) : -1;
    free(render_node);
    if (fd < 0) fd = open(kmsvnc->card, O_RDWR | O_CLOEXEC);
    return fd;
}

int egl_capture_init(int own_device)
{
    const char *reason = "unknown";
#define EGL_FAIL(why) do { reason = (why); goto fail; } while (0)
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    capture.create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    capture.destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    capture.bind_image = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!get_display || !capture.create_image || !capture.destroy_image || !capture.bind_image) EGL_FAIL("EGL/GLES entry points missing");
    if (own_device) {
        capture.gbm_fd = open_gbm_device();
        if (capture.gbm_fd < 0) EGL_FAIL("cannot open render node for writing");
    }
    capture.gbm = gbm_create_device(own_device ? capture.gbm_fd : kmsvnc->drm->drm_fd);
    if (!capture.gbm) EGL_FAIL("gbm_create_device failed");
    capture.display = get_display(EGL_PLATFORM_GBM_KHR, capture.gbm, NULL);
    if (capture.display == EGL_NO_DISPLAY || !eglInitialize(capture.display, NULL, NULL)) EGL_FAIL("eglInitialize failed");
    const char *extensions = eglQueryString(capture.display, EGL_EXTENSIONS);
    if (!extension(extensions, "EGL_EXT_image_dma_buf_import") ||
        !extension(extensions, "EGL_EXT_image_dma_buf_import_modifiers") ||
        !extension(extensions, "EGL_KHR_surfaceless_context")) EGL_FAIL("EGL dma-buf import or surfaceless extensions missing");
    if (!eglBindAPI(EGL_OPENGL_ES_API)) EGL_FAIL("eglBindAPI failed");
    /*
     * Контекст без поверхности, конфигурация нужна только для его создания.
     * У Mesa на платформе GBM конфигураций pbuffer нет, поэтому вторая
     * попытка — любая конфигурация GLES2 (маска 0 подходит всем).
     */
    EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE,
    };
    EGLConfig config;
    EGLint count = 0;
    if (!eglChooseConfig(capture.display, config_attributes, &config, 1, &count) || !count) {
        config_attributes[1] = 0;
        if (!eglChooseConfig(capture.display, config_attributes, &config, 1, &count) || !count) EGL_FAIL("no EGL config");
    }
    const EGLint context_attributes[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    capture.context = eglCreateContext(capture.display, config, EGL_NO_CONTEXT, context_attributes);
    if (capture.context == EGL_NO_CONTEXT ||
        !eglMakeCurrent(capture.display, EGL_NO_SURFACE, EGL_NO_SURFACE, capture.context)) EGL_FAIL("cannot create EGL context");
    glGenTextures(1, &capture.texture);
    glGenFramebuffers(1, &capture.framebuffer);
    if (glGetError() != GL_NO_ERROR) EGL_FAIL("GL texture/framebuffer setup failed");
    /* Проверяем импорт каждого выхода до открытия VNC-порта. */
    if (prepare_outputs()) EGL_FAIL("no capturable outputs or unknown monitor layout");
    for (int n = 0; n < capture.count; n++) if (read_output(&capture.outputs[n])) EGL_FAIL(frame_error[0] ? frame_error : "cannot read output");
    printf("EGL DMA-BUF capture: %s, %.4s, modifier 0x%lx\n",
        eglQueryString(capture.display, EGL_VENDOR),
        (char *)&kmsvnc->drm->mfb->pixel_format, (unsigned long)kmsvnc->drm->mfb->modifier);
    kmsvnc->drm->mfb->width = capture.width;
    kmsvnc->drm->mfb->height = capture.height;
    kmsvnc->drm->mmap_size = (size_t)capture.width * capture.height * 4;
    return 0;
fail:
#undef EGL_FAIL
    fprintf(stderr, "EGL DMA-BUF capture initialization failed: %s (EGL 0x%x)\n", reason, eglGetError());
    egl_capture_cleanup();
    return 1;
}

void egl_capture_frame(const char *unused, int width, int height, char *output)
{
    if (width != capture.width || height != capture.height) return;
    memset(output, 0, (size_t)width * height * 4);
    int failed = 0;
    for (int n = 0; n < capture.count; n++) {
        struct captured_output *out = &capture.outputs[n];
        failed |= read_output(out);
        for (int y = 0; y < out->height; y++) {
            char *to = output + ((size_t)(out->y + y) * width + out->x) * 4;
            const char *from = out->pixels + (size_t)(y * (int64_t)out->native_height / out->height) * out->native_width * 4;
            if (out->width == out->native_width) memcpy(to, from, out->width * 4);
            else for (int x = 0; x < out->width; x++)
                memcpy(to + x * 4, from + (size_t)(x * (int64_t)out->native_width / out->width) * 4, 4);
        }
    }
    if (failed && !capture.failed) fprintf(stderr, "EGL DMA-BUF frame read failed; preserving the last frame\n");
    capture.failed = failed;
}

static int output_count(rfbClientPtr client)
{
    return capture.count;
}

static rfbBool output_description(int index, rfbExtDesktopScreen *screen, rfbClientPtr client)
{
    if (index < 0 || index >= capture.count) return FALSE;
    const struct captured_output *out = &capture.outputs[index];
    screen->id = out->connector_id;
    screen->x = out->x; screen->y = out->y;
    screen->width = out->width; screen->height = out->height;
    screen->flags = 0;
    return TRUE;
}

void egl_capture_configure_vnc(rfbScreenInfoPtr server)
{
    if (!capture.count) return;
    server->numberOfExtDesktopScreensHook = output_count;
    server->getExtDesktopScreenHook = output_description;
    if (!strcmp(server->desktopName, "kmsvnc")) server->desktopName = capture.desktop_name;
}
