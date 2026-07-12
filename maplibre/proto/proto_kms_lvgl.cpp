// MapLibre + LVGL overlay on the Pi's physical display via KMS/DRM.
// EGL on a GBM scanout surface on card0. MapLibre renders straight into the
// window framebuffer; the LVGL overlay (rendered into a GL texture) is then
// blended on top in the same context. Presented with drmModePageFlip.
// Requires DRM master → stop the tracker service first.
//
// GL is used through LVGL's bundled GLES2 glad loader (no GLES3 calls), so
// MapLibre draws into framebuffer 0 rather than an offscreen FBO + blit.

#include "lvgl.h"
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
#include <mbgl/util/timer.hpp>
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
#include <cstdlib>
#include <memory>
#include <atomic>
#include <chrono>

// ---- KMS/DRM display -----------------------------------------------------------
struct Kms {
    int fd = -1;
    uint32_t conn_id = 0, crtc_id = 0;
    drmModeModeInfo mode{};
    drmModeCrtc *saved_crtc = nullptr;

    bool init(const char *path) {
        fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) { perror("open card0"); return false; }
        if (drmSetMaster(fd) != 0)
            fprintf(stderr, "[kms] warning: drmSetMaster failed (%s)\n", strerror(errno));
        drmModeRes *res = drmModeGetResources(fd);
        if (!res) { fprintf(stderr, "[kms] no resources\n"); return false; }
        drmModeConnector *conn = nullptr;
        for (int i = 0; i < res->count_connectors; i++) {
            drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
            if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) { conn = c; break; }
            if (c) drmModeFreeConnector(c);
        }
        if (!conn) { fprintf(stderr, "[kms] no connected connector\n"); drmModeFreeResources(res); return false; }
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
        fprintf(stderr, "[kms] connector %u crtc %u mode %ux%u@%u\n",
                conn_id, crtc_id, mode.hdisplay, mode.vdisplay, mode.vrefresh);
        return true;
    }
    void restore() {
        if (saved_crtc) {
            drmModeSetCrtc(fd, saved_crtc->crtc_id, saved_crtc->buffer_id,
                           saved_crtc->x, saved_crtc->y, &conn_id, 1, &saved_crtc->mode);
            drmModeFreeCrtc(saved_crtc);
            saved_crtc = nullptr;
        }
        drmDropMaster(fd);
    }
};
static Kms g_kms;

struct GlState {
    struct gbm_device *gbm = nullptr;
    struct gbm_surface *surf = nullptr;
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLSurface egl_surf = EGL_NO_SURFACE;
    int W = 0, H = 0;
    bool makeCurrent() { return eglMakeCurrent(dpy, egl_surf, egl_surf, ctx); }
};
static GlState g_gl;
static const uint32_t FB_FORMAT = GBM_FORMAT_XRGB8888;

static bool initGl() {
    g_gl.W = g_kms.mode.hdisplay;
    g_gl.H = g_kms.mode.vdisplay;
    g_gl.gbm = gbm_create_device(g_kms.fd);
    if (!g_gl.gbm) { fprintf(stderr, "[gl] gbm_create_device failed\n"); return false; }
    g_gl.surf = gbm_surface_create(g_gl.gbm, g_gl.W, g_gl.H, FB_FORMAT,
                                   GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!g_gl.surf) { fprintf(stderr, "[gl] gbm_surface_create failed\n"); return false; }
    auto getPD = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    g_gl.dpy = getPD ? getPD(EGL_PLATFORM_GBM_KHR, g_gl.gbm, nullptr)
                     : eglGetDisplay((EGLNativeDisplayType)g_gl.gbm);
    EGLint major, minor;
    if (!eglInitialize(g_gl.dpy, &major, &minor)) { fprintf(stderr, "[gl] eglInitialize failed\n"); return false; }
    eglBindAPI(EGL_OPENGL_ES_API);
    // MapLibre needs a stencil buffer for clipping.
    const EGLint cfgAttr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 0,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfgs[32]; EGLint n = 0;
    if (!eglChooseConfig(g_gl.dpy, cfgAttr, cfgs, 32, &n) || n < 1) {
        fprintf(stderr, "[gl] no ES3 window config\n"); return false;
    }
    EGLConfig cfg = cfgs[0];
    for (int i = 0; i < n; i++) {
        EGLint vid = 0;
        eglGetConfigAttrib(g_gl.dpy, cfgs[i], EGL_NATIVE_VISUAL_ID, &vid);
        if ((uint32_t)vid == FB_FORMAT) { cfg = cfgs[i]; break; }
    }
    const EGLint ctxAttr[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE };
    g_gl.ctx = eglCreateContext(g_gl.dpy, cfg, EGL_NO_CONTEXT, ctxAttr);
    if (g_gl.ctx == EGL_NO_CONTEXT) { fprintf(stderr, "[gl] eglCreateContext failed\n"); return false; }
    g_gl.egl_surf = eglCreateWindowSurface(g_gl.dpy, cfg, (EGLNativeWindowType)g_gl.surf, nullptr);
    if (g_gl.egl_surf == EGL_NO_SURFACE) { fprintf(stderr, "[gl] eglCreateWindowSurface failed\n"); return false; }
    return g_gl.makeCurrent();
}

struct FbWrap { int fd; uint32_t fb; };
static void bo_destroy_cb(struct gbm_bo *, void *data) {
    FbWrap *w = (FbWrap *)data;
    if (w) { drmModeRmFB(w->fd, w->fb); delete w; }
}
static uint32_t fbForBo(int fd, struct gbm_bo *bo) {
    FbWrap *w = (FbWrap *)gbm_bo_get_user_data(bo);
    if (w) return w->fb;
    uint32_t width = gbm_bo_get_width(bo), height = gbm_bo_get_height(bo);
    uint32_t stride = gbm_bo_get_stride(bo), handle = gbm_bo_get_handle(bo).u32;
    uint32_t handles[4] = {handle, 0, 0, 0}, strides[4] = {stride, 0, 0, 0}, offsets[4] = {0, 0, 0, 0};
    uint32_t fb = 0;
    if (drmModeAddFB2(fd, width, height, FB_FORMAT, handles, strides, offsets, &fb, 0) != 0) {
        fprintf(stderr, "[kms] AddFB2 failed: %s\n", strerror(errno)); return 0;
    }
    w = new FbWrap{fd, fb};
    gbm_bo_set_user_data(bo, w, bo_destroy_cb);
    return fb;
}
static void page_flip_handler(int, unsigned int, unsigned int, unsigned int, void *data) { *(int *)data = 0; }

// ---- mbgl backend rendering straight into framebuffer 0 -----------------------
class Fb0Backend;
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
    std::atomic<bool> &idle;
    explicit Observer(std::atomic<bool> &i) : idle(i) {}
    void onDidFinishLoadingStyle() override { fprintf(stderr, "[obs] style loaded\n"); }
    void onDidFailLoadingMap(mbgl::MapLoadError, const std::string &m) override { fprintf(stderr, "[obs] FAIL: %s\n", m.c_str()); }
    void onDidBecomeIdle() override { fprintf(stderr, "[obs] idle\n"); idle = true; }
};

// LVGL's GLES2 glad loader (forward-declared to avoid pulling glad macros here).
extern "C" {
    typedef void (*GLADapiproc)(void);
    typedef GLADapiproc (*GLADloadfunc)(const char *name);
    int gladLoadGLES2(GLADloadfunc load);
    // Declared in lv_opengles_private.h, which we cannot include here: it pulls
    // LVGL's bundled glad EGL headers, which clash with the system EGL/GBM ones.
    void lv_opengles_render(unsigned int texture, const lv_area_t *texture_area, lv_opa_t opa,
                            int32_t disp_w, int32_t disp_h, const lv_area_t *texture_clip_area,
                            bool h_flip, bool v_flip, lv_color_t fill_color, bool blend_opt, bool flipRB);
}

int main(int argc, char **argv) {
    const char *style = (argc > 1) ? argv[1] : "file:///data/LoRa_Tracker/MapLibre/osm-bright.json";
    double lat = (argc > 2) ? atof(argv[2]) : 43.5850;
    double lon = (argc > 3) ? atof(argv[3]) : 1.4337;
    double zoom = (argc > 4) ? atof(argv[4]) : 13.0;
    int hold_s = (argc > 5) ? atoi(argv[5]) : 20;

    if (!g_kms.init("/dev/dri/card0")) return 1;
    if (!initGl()) { g_kms.restore(); return 1; }
    if (!gladLoadGLES2((GLADloadfunc)eglGetProcAddress)) { fprintf(stderr, "gladLoadGLES2 failed\n"); return 1; }
    fprintf(stderr, "[INFO] GL_RENDERER: %s\n", glGetString(GL_RENDERER));

    const int W = g_gl.W, H = g_gl.H;

    // LVGL overlay: widgets rendered into a GL texture, transparent background.
    lv_init();
    lv_display_t *overlay = lv_opengles_texture_create(W, H);
    lv_obj_set_style_bg_opa(lv_display_get_screen_active(overlay), LV_OPA_TRANSP, 0);
    lv_display_set_render_mode(overlay, LV_DISPLAY_RENDER_MODE_FULL);
    lv_obj_t *panel = lv_obj_create(lv_display_get_screen_active(overlay));
    lv_obj_set_size(panel, 200, 40);
    lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_50, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 5, 0);
    lv_obj_t *lbl = lv_label_create(panel);
    lv_label_set_text(lbl, "F4MLV-9  APRS");
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x00FF00), 0);
    lv_obj_center(lbl);
    unsigned int overlay_tex = lv_opengles_texture_get_texture_id(overlay);
    lv_area_t overlay_area = {0, 0, W - 1, H - 1};

    mbgl::util::RunLoop runLoop;
    std::atomic<bool> dirty{true}, idle{false};
    Fb0Backend backend(W, H);
    auto renderer = std::make_unique<mbgl::Renderer>(backend, 1.0f);
    Fb0Frontend frontend(std::move(renderer), backend, dirty);
    Observer observer(idle);
    mbgl::ResourceOptions resOpts;
    mbgl::Map map(frontend, observer, mbgl::MapOptions().withSize({(uint32_t)W, (uint32_t)H}).withPixelRatio(1.0f), resOpts);
    map.getStyle().loadURL(style);
    map.jumpTo(mbgl::CameraOptions().withCenter(mbgl::LatLng{lat, lon}).withZoom(zoom));

    struct gbm_bo *prev_bo = nullptr;
    bool crtc_set = false;
    auto idle_at = std::chrono::steady_clock::time_point::max();

    mbgl::util::Timer tick;
    tick.start(mbgl::Duration::zero(), mbgl::Milliseconds(1000 / 60), [&] {
        // 1. MapLibre into framebuffer 0 (the window surface)
        if (dirty.exchange(false)) frontend.render();

        // 2. LVGL widgets into the overlay texture
        lv_draw_buf_t *dbuf = lv_display_get_buf_active(overlay);
        if (dbuf && dbuf->data) memset(dbuf->data, 0, W * H * 4);
        lv_obj_invalidate(lv_display_get_screen_active(overlay));
        lv_display_t *prev = lv_display_get_default();
        lv_display_set_default(overlay);
        lv_refr_now(overlay);
        lv_display_set_default(prev);
        if (dbuf && dbuf->data) {
            uint32_t *px = (uint32_t *)dbuf->data;
            for (int i = 0; i < W * H; i++) if (px[i] == 0xFFFFFFFF) px[i] = 0;
            glBindTexture(GL_TEXTURE_2D, overlay_tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, dbuf->data);
            glBindTexture(GL_TEXTURE_2D, 0);
        }

        // 3. Blend overlay on top of the map, then restore GL state for mbgl
        lv_opengles_render(overlay_tex, &overlay_area, LV_OPA_COVER, W, H, &overlay_area,
                           false, false, lv_color_hex(0x000000), true, true);
        glUseProgram(0);
        glBindVertexArrayOES(0);
        glBindTexture(GL_TEXTURE_2D, 0);
        // LVGL's flush leaves the pixel-store row length set, which corrupts
        // MapLibre's texture uploads on the next frame.
        glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // 4. Present
        eglSwapBuffers(g_gl.dpy, g_gl.egl_surf);
        struct gbm_bo *bo = gbm_surface_lock_front_buffer(g_gl.surf);
        if (!bo) { fprintf(stderr, "[kms] lock_front_buffer failed\n"); return; }
        uint32_t fb = fbForBo(g_kms.fd, bo);
        if (!crtc_set) {
            drmModeSetCrtc(g_kms.fd, g_kms.crtc_id, fb, 0, 0, &g_kms.conn_id, 1, &g_kms.mode);
            crtc_set = true;
        } else {
            int waiting = 1;
            if (drmModePageFlip(g_kms.fd, g_kms.crtc_id, fb, DRM_MODE_PAGE_FLIP_EVENT, &waiting) == 0) {
                drmEventContext ev{}; ev.version = 2; ev.page_flip_handler = page_flip_handler;
                struct pollfd pfd{g_kms.fd, POLLIN, 0};
                while (waiting) { if (poll(&pfd, 1, 100) > 0) drmHandleEvent(g_kms.fd, &ev); else break; }
            }
        }
        if (prev_bo) gbm_surface_release_buffer(g_gl.surf, prev_bo);
        prev_bo = bo;

        if (idle.load() && idle_at == std::chrono::steady_clock::time_point::max())
            idle_at = std::chrono::steady_clock::now();
        if (idle_at != std::chrono::steady_clock::time_point::max() &&
            std::chrono::steady_clock::now() - idle_at > std::chrono::seconds(hold_s))
            runLoop.stop();
    });
    runLoop.run();

    if (prev_bo) gbm_surface_release_buffer(g_gl.surf, prev_bo);
    g_kms.restore();
    fprintf(stderr, "[kms] done, master released\n");
    return 0;
}
