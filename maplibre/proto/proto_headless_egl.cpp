// Headless MapLibre render on the Pi's V3D: EGL context on a GBM render node
// (no DRM master, no display — coexists with the running tracker), MapLibre
// drawn into an FBO, read back and written to a PNG for inspection.
//
// Build (on the Pi): see build_headless.sh in this directory.

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
#include <mbgl/util/image.hpp>

#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <atomic>
#include <chrono>
#include <vector>
#include <fstream>

static const int W = 1024, H = 600;

// ---- EGL/GBM headless context (render node, no master) ------------------------
struct EglCtx {
    int fd = -1;
    struct gbm_device *gbm = nullptr;
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLContext ctx = EGL_NO_CONTEXT;

    bool init(const char *node) {
        fd = open(node, O_RDWR);
        if (fd < 0) { perror("open render node"); return false; }
        gbm = gbm_create_device(fd);
        if (!gbm) { fprintf(stderr, "gbm_create_device failed\n"); return false; }
        auto getPD = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        dpy = getPD ? getPD(EGL_PLATFORM_GBM_KHR, gbm, nullptr)
                    : eglGetDisplay((EGLNativeDisplayType)gbm);
        if (dpy == EGL_NO_DISPLAY) { fprintf(stderr, "no EGL display\n"); return false; }
        EGLint major, minor;
        if (!eglInitialize(dpy, &major, &minor)) { fprintf(stderr, "eglInitialize failed\n"); return false; }
        eglBindAPI(EGL_OPENGL_ES_API);
        const EGLint cfgAttr[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_NONE
        };
        EGLConfig cfg; EGLint n;
        if (!eglChooseConfig(dpy, cfgAttr, &cfg, 1, &n) || n < 1) {
            fprintf(stderr, "no ES3 config\n"); return false;
        }
        const EGLint ctxAttr[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE };
        ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttr);
        if (ctx == EGL_NO_CONTEXT) { fprintf(stderr, "no ES3 context\n"); return false; }
        return makeCurrent();
    }
    bool makeCurrent() { return eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx); }
    void release() { eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); }
};

static EglCtx g_egl;

// ---- GL backend rendering into our FBO/texture --------------------------------
class TexBackend;

class TexRenderableResource final : public mbgl::gl::RenderableResource {
public:
    explicit TexRenderableResource(TexBackend &b) : backend(b) {}
    void bind() override;
    void swap() override { glFinish(); }
private:
    TexBackend &backend;
};

class TexBackend : public mbgl::gl::RendererBackend, public mbgl::gfx::Renderable {
public:
    TexBackend()
        : mbgl::gl::RendererBackend(mbgl::gfx::ContextMode::Unique),
          mbgl::gfx::Renderable(mbgl::Size{W, H}, std::make_unique<TexRenderableResource>(*this)) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glGenRenderbuffers(1, &rbo);
        glBindRenderbuffer(GL_RENDERBUFFER, rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, W, H);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            fprintf(stderr, "FBO incomplete!\n");
    }
    ~TexBackend() override = default;

    unsigned int fboId() const { return fbo; }

    mbgl::gfx::Renderable &getDefaultRenderable() override { return *this; }
    void activate() override { g_egl.makeCurrent(); }
    void deactivate() override { g_egl.release(); }
    mbgl::gl::ProcAddress getExtensionFunctionPointer(const char *name) override {
        return eglGetProcAddress(name);
    }
    void updateAssumedState() override {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        assumeFramebufferBinding(fbo);
        glViewport(0, 0, W, H);
        assumeViewport(0, 0, size);
    }
private:
    unsigned int tex = 0, fbo = 0, rbo = 0;
};

void TexRenderableResource::bind() {
    backend.setFramebufferBinding(backend.fboId());
    backend.setViewport(0, 0, backend.getSize());
}

// ---- Frontend -----------------------------------------------------------------
class TexFrontend : public mbgl::RendererFrontend {
public:
    TexFrontend(std::unique_ptr<mbgl::Renderer> r, TexBackend &b, std::atomic<bool> &dirtyFlag)
        : backend(b), renderer(std::move(r)), dirty(dirtyFlag) {}
    void reset() override { renderer.reset(); }
    void setObserver(mbgl::RendererObserver &obs) override { renderer->setObserver(&obs); }
    void update(std::shared_ptr<mbgl::UpdateParameters> params) override {
        updateParameters = std::move(params);
        dirty = true;
    }
    const mbgl::TaggedScheduler &getThreadPool() const override { return backend.getThreadPool(); }
    void render() {
        if (!updateParameters || !renderer) return;
        mbgl::gfx::BackendScope guard{backend, mbgl::gfx::BackendScope::ScopeType::Implicit};
        renderer->render(updateParameters);
    }
private:
    TexBackend &backend;
    std::unique_ptr<mbgl::Renderer> renderer;
    std::shared_ptr<mbgl::UpdateParameters> updateParameters;
    std::atomic<bool> &dirty;
};

struct SnapObserver : public mbgl::MapObserver {
    std::atomic<bool> &idle;
    explicit SnapObserver(std::atomic<bool> &i) : idle(i) {}
    void onDidFinishLoadingStyle() override { fprintf(stderr, "[obs] style loaded\n"); }
    void onDidFailLoadingMap(mbgl::MapLoadError, const std::string &msg) override {
        fprintf(stderr, "[obs] MAP LOAD FAIL: %s\n", msg.c_str());
    }
    void onDidBecomeIdle() override { fprintf(stderr, "[obs] idle\n"); idle = true; }
};

static void savePNG(TexBackend &backend, const char *path) {
    std::vector<uint8_t> buf(W * H * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, backend.fboId());
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
    // GL origin is bottom-left; flip to top-left for the image.
    mbgl::PremultipliedImage img({(uint32_t)W, (uint32_t)H});
    for (int y = 0; y < H; y++)
        memcpy(img.data.get() + y * W * 4, buf.data() + (H - 1 - y) * W * 4, W * 4);
    std::string png = mbgl::encodePNG(img);
    std::ofstream(path, std::ios::binary).write(png.data(), png.size());
    fprintf(stderr, "[snap] wrote %s (%zu bytes)\n", path, png.size());
}

int main(int argc, char **argv) {
    const char *style = (argc > 1) ? argv[1]
        : "file:///data/LoRa_Tracker/MapLibre/osm-bright.json";
    const char *out = (argc > 2) ? argv[2] : "/home/adrasec09/headless.png";
    double lat = (argc > 3) ? atof(argv[3]) : 43.5850;
    double lon = (argc > 4) ? atof(argv[4]) : 1.4337;
    double zoom = (argc > 5) ? atof(argv[5]) : 13.0;

    if (!g_egl.init("/dev/dri/renderD128")) { fprintf(stderr, "EGL/GBM init failed\n"); return 1; }
    fprintf(stderr, "[INFO] GL_RENDERER: %s\n", glGetString(GL_RENDERER));

    mbgl::util::RunLoop runLoop;
    std::atomic<bool> dirty{true};
    std::atomic<bool> idle{false};

    TexBackend backend;
    auto renderer = std::make_unique<mbgl::Renderer>(backend, 1.0f);
    TexFrontend frontend(std::move(renderer), backend, dirty);
    SnapObserver observer(idle);

    mbgl::ResourceOptions resOpts;
    mbgl::Map map(frontend, observer,
                  mbgl::MapOptions().withSize({W, H}).withPixelRatio(1.0f),
                  resOpts);
    map.getStyle().loadURL(style);
    map.jumpTo(mbgl::CameraOptions().withCenter(mbgl::LatLng{lat, lon}).withZoom(zoom));

    auto start = std::chrono::steady_clock::now();
    bool saved = false;
    mbgl::util::Timer tick;
    tick.start(mbgl::Duration::zero(), mbgl::Milliseconds(1000 / 60), [&] {
        if (dirty.exchange(false)) frontend.render();
        auto elapsed = std::chrono::steady_clock::now() - start;
        bool timeout = elapsed > std::chrono::seconds(20);
        if (!saved && (idle.load() || timeout)) {
            frontend.render();               // one clean frame with everything loaded
            savePNG(backend, out);
            saved = true;
            if (timeout && !idle.load()) fprintf(stderr, "[snap] saved on timeout (not fully idle)\n");
            runLoop.stop();
        }
    });
    runLoop.run();
    return saved ? 0 : 2;
}
