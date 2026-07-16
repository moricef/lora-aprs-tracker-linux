#include "maplibre_display.h"
#ifdef WITH_MAPLIBRE

#include "map_state.h"

#include "src/drivers/opengles/lv_opengles_driver.h"
#include "src/drivers/opengles/lv_opengles_texture.h"
#include "src/drivers/opengles/glad/include/glad/gles2.h"

#include <mbgl/map/map.hpp>
#include <mbgl/map/map_observer.hpp>
#include <mbgl/map/map_options.hpp>
#include <mbgl/map/camera.hpp>
#include <mbgl/renderer/renderer.hpp>
#include <mbgl/renderer/renderer_frontend.hpp>
#include <mbgl/gfx/backend_scope.hpp>
#include <mbgl/gl/renderer_backend.hpp>
#include <mbgl/gl/renderable_resource.hpp>
#include <mbgl/style/style.hpp>
#include <mbgl/storage/resource_options.hpp>
#include <mbgl/util/run_loop.hpp>
#include <mbgl/util/geo.hpp>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <cmath>
#include <memory>
#include <atomic>
#include <string>
#include <vector>
#include <fstream>
#include <mbgl/util/image.hpp>

extern "C" {
    typedef void (*GLADapiproc)(void);
    typedef GLADapiproc (*GLADloadfunc)(const char *name);
    int gladLoadGLES2(GLADloadfunc load);
    // In lv_opengles_private.h, which clashes with the system EGL/GBM headers.
    void lv_opengles_render(unsigned int texture, const lv_area_t *texture_area, lv_opa_t opa,
                            int32_t disp_w, int32_t disp_h, const lv_area_t *texture_clip_area,
                            bool h_flip, bool v_flip, lv_color_t fill_color, bool blend_opt, bool flipRB);
}

namespace {

// ---- KMS/DRM display ----------------------------------------------------------
struct Kms {
    int fd = -1;
    uint32_t conn_id = 0, crtc_id = 0;
    drmModeModeInfo mode{};
    drmModeCrtc *saved_crtc = nullptr;

    bool init(const char *path) {
        fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            fprintf(stderr, "[maplibre] open %s failed: %s\n", path, strerror(errno));
            return false;
        }
        if (drmSetMaster(fd) != 0)
            fprintf(stderr, "[maplibre] drmSetMaster failed (%s)\n", strerror(errno));
        drmModeRes *res = drmModeGetResources(fd);
        if (!res) return false;
        drmModeConnector *conn = nullptr;
        for (int i = 0; i < res->count_connectors; i++) {
            drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
            if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) { conn = c; break; }
            if (c) drmModeFreeConnector(c);
        }
        if (!conn) { drmModeFreeResources(res); return false; }
        conn_id = conn->connector_id;
        mode = conn->modes[0];
        for (int i = 0; i < conn->count_modes; i++)
            if (conn->modes[i].hdisplay == 1024 && conn->modes[i].vdisplay == 600) { mode = conn->modes[i]; break; }
        drmModeEncoder *enc = conn->encoder_id ? drmModeGetEncoder(fd, conn->encoder_id) : nullptr;
        crtc_id = (enc && enc->crtc_id) ? enc->crtc_id : res->crtcs[0];
        if (enc) drmModeFreeEncoder(enc);
        saved_crtc = drmModeGetCrtc(fd, crtc_id);
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        return true;
    }
    void restore() {
        if (saved_crtc) {
            drmModeSetCrtc(fd, saved_crtc->crtc_id, saved_crtc->buffer_id,
                           saved_crtc->x, saved_crtc->y, &conn_id, 1, &saved_crtc->mode);
            drmModeFreeCrtc(saved_crtc);
            saved_crtc = nullptr;
        }
        if (fd >= 0) drmDropMaster(fd);
    }
};

struct GlState {
    struct gbm_device *gbm = nullptr;
    struct gbm_surface *surf = nullptr;
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLSurface egl_surf = EGL_NO_SURFACE;
    int W = 0, H = 0;
    bool makeCurrent() { return eglMakeCurrent(dpy, egl_surf, egl_surf, ctx); }
};

const uint32_t FB_FORMAT = GBM_FORMAT_XRGB8888;

struct FbWrap { int fd; uint32_t fb; };
void bo_destroy_cb(struct gbm_bo *, void *data) {
    FbWrap *w = (FbWrap *)data;
    if (w) { drmModeRmFB(w->fd, w->fb); delete w; }
}
uint32_t fbForBo(int fd, struct gbm_bo *bo) {
    FbWrap *w = (FbWrap *)gbm_bo_get_user_data(bo);
    if (w) return w->fb;
    uint32_t width = gbm_bo_get_width(bo), height = gbm_bo_get_height(bo);
    uint32_t stride = gbm_bo_get_stride(bo), handle = gbm_bo_get_handle(bo).u32;
    uint32_t handles[4] = {handle, 0, 0, 0}, strides[4] = {stride, 0, 0, 0}, offsets[4] = {0, 0, 0, 0};
    uint32_t fb = 0;
    if (drmModeAddFB2(fd, width, height, FB_FORMAT, handles, strides, offsets, &fb, 0) != 0) return 0;
    w = new FbWrap{fd, fb};
    gbm_bo_set_user_data(bo, w, bo_destroy_cb);
    return fb;
}
void page_flip_handler(int, unsigned int, unsigned int, unsigned int, void *data) { *(int *)data = 0; }

// ---- mbgl backend rendering into framebuffer 0 --------------------------------
class Fb0Backend;
GlState g_gl;

class Fb0Resource final : public mbgl::gl::RenderableResource {
public:
    explicit Fb0Resource(Fb0Backend &b) : backend(b) {}
    void bind() override;
    void swap() override {}
private:
    Fb0Backend &backend;
};

class Fb0Backend : public mbgl::gl::RendererBackend, public mbgl::gfx::Renderable {
public:
    Fb0Backend(int w, int h)
        : mbgl::gl::RendererBackend(mbgl::gfx::ContextMode::Shared),
          mbgl::gfx::Renderable(mbgl::Size{(uint32_t)w, (uint32_t)h}, std::make_unique<Fb0Resource>(*this)),
          w_(w), h_(h) {}
    ~Fb0Backend() override = default;
    mbgl::gfx::Renderable &getDefaultRenderable() override { return *this; }
    void activate() override { g_gl.makeCurrent(); }
    void deactivate() override {}
    mbgl::gl::ProcAddress getExtensionFunctionPointer(const char *name) override { return eglGetProcAddress(name); }
    void updateAssumedState() override {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        assumeFramebufferBinding(0);
        glViewport(0, 0, w_, h_);
        assumeViewport(0, 0, size);
    }
private:
    int w_, h_;
};
void Fb0Resource::bind() {
    backend.setFramebufferBinding(0);
    backend.setViewport(0, 0, backend.getSize());
}

class Fb0Frontend : public mbgl::RendererFrontend {
public:
    Fb0Frontend(std::unique_ptr<mbgl::Renderer> r, Fb0Backend &b, std::atomic<bool> &d)
        : backend(b), renderer(std::move(r)), dirty(d) {}
    void reset() override { renderer.reset(); }
    void setObserver(mbgl::RendererObserver &obs) override { renderer->setObserver(&obs); }
    void update(std::shared_ptr<mbgl::UpdateParameters> params) override { updateParameters = std::move(params); dirty = true; }
    const mbgl::TaggedScheduler &getThreadPool() const override { return backend.getThreadPool(); }
    void render() {
        if (!updateParameters || !renderer) return;
        mbgl::gfx::BackendScope guard{backend, mbgl::gfx::BackendScope::ScopeType::Implicit};
        renderer->render(updateParameters);
    }
private:
    Fb0Backend &backend;
    std::unique_ptr<mbgl::Renderer> renderer;
    std::shared_ptr<mbgl::UpdateParameters> updateParameters;
    std::atomic<bool> &dirty;
};

struct Observer : public mbgl::MapObserver {
    std::atomic<bool> ready{false};
    void onWillStartLoadingMap() override { ready = false; }
    void onDidFinishRenderingMap(RenderMode mode) override {
        if (mode == RenderMode::Full) ready = true;
    }
    void onDidFailLoadingMap(mbgl::MapLoadError, const std::string &m) override {
        fprintf(stderr, "[maplibre] map load fail: %s\n", m.c_str());
    }
};

// ---- module state -------------------------------------------------------------
struct State {
    Kms kms;
    std::unique_ptr<mbgl::util::RunLoop> runLoop;
    std::unique_ptr<Fb0Backend> backend;
    std::unique_ptr<Fb0Frontend> frontend;
    std::unique_ptr<mbgl::Map> map;
    Observer observer;
    std::atomic<bool> dirty{true};
    lv_display_t *overlay = nullptr;
    unsigned int overlay_tex = 0;
    struct gbm_bo *prev_bo = nullptr;
    struct gbm_bo *pending_bo = nullptr;
    int flipPending = 0;
    bool crtc_set = false;
    int W = 0, H = 0;
    std::string shotPath;
};
State *S = nullptr;

bool initGl() {
    g_gl.W = S->kms.mode.hdisplay;
    g_gl.H = S->kms.mode.vdisplay;
    g_gl.gbm = gbm_create_device(S->kms.fd);
    if (!g_gl.gbm) return false;
    g_gl.surf = gbm_surface_create(g_gl.gbm, g_gl.W, g_gl.H, FB_FORMAT,
                                   GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!g_gl.surf) return false;
    auto getPD = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    g_gl.dpy = getPD ? getPD(EGL_PLATFORM_GBM_KHR, g_gl.gbm, nullptr)
                     : eglGetDisplay((EGLNativeDisplayType)g_gl.gbm);
    EGLint major, minor;
    if (!eglInitialize(g_gl.dpy, &major, &minor)) return false;
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint cfgAttr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 0,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE
    };
    EGLConfig cfgs[32]; EGLint n = 0;
    if (!eglChooseConfig(g_gl.dpy, cfgAttr, cfgs, 32, &n) || n < 1) return false;
    EGLConfig cfg = cfgs[0];
    for (int i = 0; i < n; i++) {
        EGLint vid = 0;
        eglGetConfigAttrib(g_gl.dpy, cfgs[i], EGL_NATIVE_VISUAL_ID, &vid);
        if ((uint32_t)vid == FB_FORMAT) { cfg = cfgs[i]; break; }
    }
    const EGLint ctxAttr[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE };
    g_gl.ctx = eglCreateContext(g_gl.dpy, cfg, EGL_NO_CONTEXT, ctxAttr);
    if (g_gl.ctx == EGL_NO_CONTEXT) return false;
    g_gl.egl_surf = eglCreateWindowSurface(g_gl.dpy, cfg, (EGLNativeWindowType)g_gl.surf, nullptr);
    if (g_gl.egl_surf == EGL_NO_SURFACE) return false;
    return g_gl.makeCurrent();
}

} // namespace

namespace MaplibreDisplay {

lv_display_t *init(const char *stylePath, double lat, double lon, double zoom) {
    S = new State();
    // DRM card numbering is not stable across boots on the Pi 4: vc4-drm can
    // be card0 or card1 depending on whether V3D probes first.  This udev link
    // always targets the vc4 display device, while card0 may be render-only.
    if (!S->kms.init("/dev/dri/by-path/platform-gpu-card")) {
        fprintf(stderr, "[maplibre] KMS init failed\n");
        return nullptr;
    }
    if (!initGl()) { fprintf(stderr, "[maplibre] GL init failed\n"); S->kms.restore(); return nullptr; }
    if (!gladLoadGLES2((GLADloadfunc)eglGetProcAddress)) { fprintf(stderr, "[maplibre] gladLoadGLES2 failed\n"); return nullptr; }
    fprintf(stderr, "[maplibre] GL_RENDERER: %s\n", glGetString(GL_RENDERER));

    S->W = g_gl.W; S->H = g_gl.H;

    // Overlay display for the LVGL UI (caller builds widgets on it).
    S->overlay = lv_opengles_texture_create(S->W, S->H);
    // Real alpha so transparent areas expose the map underneath — the texture
    // display defaults to XRGB8888 (opaque), which forced the white color-key hack.
    lv_display_set_color_format(S->overlay, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_set_style_bg_opa(lv_display_get_screen_active(S->overlay), LV_OPA_TRANSP, 0);
    lv_display_set_render_mode(S->overlay, LV_DISPLAY_RENDER_MODE_FULL);
    S->overlay_tex = lv_opengles_texture_get_texture_id(S->overlay);

    S->runLoop = std::make_unique<mbgl::util::RunLoop>();
    S->backend = std::make_unique<Fb0Backend>(S->W, S->H);
    auto renderer = std::make_unique<mbgl::Renderer>(*S->backend, 1.0f);
    S->frontend = std::make_unique<Fb0Frontend>(std::move(renderer), *S->backend, S->dirty);
    mbgl::ResourceOptions resOpts;
    S->map = std::make_unique<mbgl::Map>(*S->frontend, S->observer,
        mbgl::MapOptions().withSize({(uint32_t)S->W, (uint32_t)S->H}).withPixelRatio(1.0f), resOpts);
    S->map->getStyle().loadURL(stylePath);
    S->map->jumpTo(mbgl::CameraOptions().withCenter(mbgl::LatLng{lat, lon}).withZoom(zoom));
    return S->overlay;
}

bool isActive() { return S != nullptr; }
bool isReady() { return S && S->observer.ready.load(); }

void requestScreenshot(const char *path) {
    if (S && path) S->shotPath = path;
}

void setCenter(double lat, double lon) {
    if (S && S->map) S->map->jumpTo(mbgl::CameraOptions().withCenter(mbgl::LatLng{lat, lon}));
}
void setZoom(double zoom) {
    if (S && S->map) S->map->jumpTo(mbgl::CameraOptions().withZoom(zoom));
}
void zoomAround(double zoom, double anchorX, double anchorY) {
    if (S && S->map)
        S->map->jumpTo(mbgl::CameraOptions()
                           .withZoom(zoom)
                           .withAnchor(mbgl::ScreenCoordinate{anchorX, anchorY}));
}
double getZoom() {
    return (S && S->map) ? S->map->getCameraOptions().zoom.value_or(DEFAULT_ZOOM)
                         : DEFAULT_ZOOM;
}
void moveBy(double dx, double dy) {
    if (S && S->map) S->map->moveBy({dx, dy});
}
bool getCenter(double *lat, double *lon) {
    if (!S || !S->map || !lat || !lon) return false;
    const auto camera = S->map->getCameraOptions();
    if (!camera.center) return false;
    *lat = camera.center->latitude();
    *lon = camera.center->longitude();
    return true;
}
bool project(double lat, double lon, int *x, int *y) {
    if (!S || !S->map || !x || !y) return false;
    mbgl::ScreenCoordinate p = S->map->pixelForLatLng(mbgl::LatLng{lat, lon});
    *x = (int)std::lround(p.x);
    *y = (int)std::lround(p.y);
    return p.x >= 0.0 && p.x < S->W && p.y >= 0.0 && p.y < S->H;
}

void renderTick() {
    if (!S) return;
    const bool mapVisible = MapState::screen_map &&
        lv_display_get_screen_active(S->overlay) == MapState::screen_map;

    // Keep MapLibre's run loop alive while another UI page is displayed so
    // style/tile requests can complete in the background.  Only request and
    // compose a map frame when the MAP screen is actually active.
    if (mapVisible) S->map->triggerRepaint();
    S->runLoop->runOnce();

    // lv_timer_handler() in the main loop flushes the LVGL overlay and leaves
    // the pixel-store row length set; MapLibre's glyph-atlas uploads below then
    // land corrupted (accented glyphs, added to the atlas once their labels
    // appear, render as tofu boxes). Reset before rendering the map.
    glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    // 1. EGL alternates scanout buffers. Render the map into every MAP frame;
    // on all other pages erase the GPU base explicitly. Depending on LVGL
    // alpha alone allowed the map to flash through Settings during redraws.
    if (mapVisible) {
        S->dirty.exchange(false);
        S->frontend->render();
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, S->W, S->H);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0x1a / 255.0f, 0x1a / 255.0f, 0x2e / 255.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    // 2. Redraw the complete LVGL overlay on a cleared ARGB buffer.  Clearing
    //    is essential: deleting an opaque widget (notably the splash screen)
    //    must restore transparent pixels so the MapLibre framebuffer becomes
    //    visible underneath it.
    lv_draw_buf_t *dbuf = lv_display_get_buf_active(S->overlay);
    if (dbuf && dbuf->data) std::memset(dbuf->data, 0, S->W * S->H * 4);
    lv_obj_invalidate(lv_display_get_screen_active(S->overlay));
    lv_display_t *previousDisplay = lv_display_get_default();
    lv_display_set_default(S->overlay);
    lv_refr_now(S->overlay);
    lv_display_set_default(previousDisplay);
    if (dbuf && dbuf->data) {
        glBindTexture(GL_TEXTURE_2D, S->overlay_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, S->W, S->H, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, dbuf->data);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // 3. Blend the overlay in a deterministic full-screen GL state. MapLibre
    // leaves scissor/stencil/depth configured according to the last rendered
    // tile or symbol layer. Reusing that state clipped or shifted the LVGL
    // top/bottom bars depending on the visible map area.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, S->W, S->H);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glActiveTexture(GL_TEXTURE0);
    glBlendEquation(GL_FUNC_ADD);

    lv_area_t area = {0, 0, S->W - 1, S->H - 1};
    // GL_BGRA_EXT upload already samples as correct RGB on V3D; no shader R/B swap.
    lv_opengles_render(S->overlay_tex, &area, LV_OPA_COVER, S->W, S->H, &area,
                       false, false, lv_color_hex(0x000000), true, false);

    glUseProgram(0);
    glBindVertexArrayOES(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Screenshot request (SIGUSR2): read the just-composed frame before present.
    if (!S->shotPath.empty()) {
        std::vector<uint8_t> b(S->W * S->H * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, S->W, S->H, GL_RGBA, GL_UNSIGNED_BYTE, b.data());
        mbgl::PremultipliedImage im({(uint32_t)S->W, (uint32_t)S->H});
        for (int y = 0; y < S->H; y++)  // GL origin bottom-left → flip to top-left
            memcpy(im.data.get() + y * S->W * 4, b.data() + (S->H - 1 - y) * S->W * 4, S->W * 4);
        std::string png = mbgl::encodePNG(im);
        std::ofstream(S->shotPath, std::ios::binary).write(png.data(), png.size());
        fprintf(stderr, "[maplibre] screenshot -> %s (%zu bytes)\n", S->shotPath.c_str(), png.size());
        S->shotPath.clear();
    }

    // 4. Present
    eglSwapBuffers(g_gl.dpy, g_gl.egl_surf);
    struct gbm_bo *bo = gbm_surface_lock_front_buffer(g_gl.surf);
    if (!bo) return;
    uint32_t fb = fbForBo(S->kms.fd, bo);
    if (!S->crtc_set) {
        int rc = drmModeSetCrtc(S->kms.fd, S->kms.crtc_id, fb, 0, 0,
                                &S->kms.conn_id, 1, &S->kms.mode);
        S->crtc_set = rc == 0;
        if (rc != 0)
            fprintf(stderr, "[maplibre] set-crtc failed: %s\n", strerror(errno));
        if (S->prev_bo) gbm_surface_release_buffer(g_gl.surf, S->prev_bo);
        S->prev_bo = bo;
    } else {
        drmEventContext ev{}; ev.version = 2; ev.page_flip_handler = page_flip_handler;
        struct pollfd pfd{S->kms.fd, POLLIN, 0};
        // A completion event that arrived after the wait below timed out on a
        // previous tick is still queued; a new flip would then return EBUSY
        // and a per-frame SetCrtc fallback re-modesets the panel — that was
        // visible as a flicker of the screen bars. Drain it first, and skip
        // this frame entirely if the flip is genuinely still pending.
        if (S->flipPending) {
            if (poll(&pfd, 1, 100) > 0) drmHandleEvent(S->kms.fd, &ev);
            if (S->flipPending) {
                gbm_surface_release_buffer(g_gl.surf, bo);
                return;
            }
        }
        // The completed flip put pending_bo on scanout; its predecessor is
        // now free.
        if (S->pending_bo) {
            if (S->prev_bo) gbm_surface_release_buffer(g_gl.surf, S->prev_bo);
            S->prev_bo = S->pending_bo;
            S->pending_bo = nullptr;
        }
        S->flipPending = 1;
        int rc = drmModePageFlip(S->kms.fd, S->kms.crtc_id, fb,
                                 DRM_MODE_PAGE_FLIP_EVENT, &S->flipPending);
        if (rc != 0) {
            S->flipPending = 0;
            fprintf(stderr, "[maplibre] page-flip failed: %s; frame skipped\n",
                    strerror(errno));
            gbm_surface_release_buffer(g_gl.surf, bo);
            return;
        }
        S->pending_bo = bo;
        while (S->flipPending) {
            if (poll(&pfd, 1, 100) > 0) drmHandleEvent(S->kms.fd, &ev);
            else break;
        }
        if (!S->flipPending) {
            if (S->prev_bo) gbm_surface_release_buffer(g_gl.surf, S->prev_bo);
            S->prev_bo = S->pending_bo;
            S->pending_bo = nullptr;
        }
    }
}

void shutdown() {
    if (!S) return;
    if (S->pending_bo) gbm_surface_release_buffer(g_gl.surf, S->pending_bo);
    if (S->prev_bo) gbm_surface_release_buffer(g_gl.surf, S->prev_bo);
    S->map.reset();
    S->frontend.reset();
    S->backend.reset();
    S->runLoop.reset();
    S->kms.restore();
    delete S;
    S = nullptr;
}

} // namespace MaplibreDisplay

#endif // WITH_MAPLIBRE
