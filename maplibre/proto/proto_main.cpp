// Prototype: MapLibre Native rendered into a GL texture, composited by LVGL
// (lv_opengles GLFW window) in the same GL context — no CPU readback.
// Mouse drag = pan, wheel = zoom. A translucent LVGL panel sits on top to
// prove UI compositing over the live map.
//
// Build: make -C maplibre/proto && maplibre/proto/proto

#include "lvgl.h"
#include "src/drivers/opengles/lv_opengles_glfw.h"
#include "src/drivers/opengles/lv_opengles_window.h"
#include "src/drivers/opengles/lv_opengles_texture.h"
#include "src/drivers/opengles/lv_opengles_private.h"
#include "src/drivers/opengles/glad/include/glad/gl.h"

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

#include <GLFW/glfw3.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <atomic>
#include <vector>

static const int W = 1024, H = 600;

// ---- GL backend rendering into our FBO/texture --------------------------------
class TexBackend;

class TexRenderableResource final : public mbgl::gl::RenderableResource {
public:
    explicit TexRenderableResource(TexBackend &b) : backend(b) {}
    void bind() override;
    // End of the map frame: hand the default framebuffer back to LVGL,
    // otherwise its window compositing lands in the map FBO (flicker).
    void swap() override;
private:
    TexBackend &backend;
};

class TexBackend : public mbgl::gl::RendererBackend, public mbgl::gfx::Renderable {
public:
    TexBackend(GLFWwindow *win)
        : mbgl::gl::RendererBackend(mbgl::gfx::ContextMode::Shared),
          mbgl::gfx::Renderable(mbgl::Size{W, H}, std::make_unique<TexRenderableResource>(*this)),
          window(win) {
        glfwMakeContextCurrent(window);
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
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    ~TexBackend() override = default;

    unsigned int textureId() const { return tex; }
    unsigned int fboId() const { return fbo; }

    mbgl::gfx::Renderable &getDefaultRenderable() override { return *this; }
    void activate() override { glfwMakeContextCurrent(window); }
    void deactivate() override { glfwMakeContextCurrent(nullptr); }
    mbgl::gl::ProcAddress getExtensionFunctionPointer(const char *name) override {
        return glfwGetProcAddress(name);
    }
    void updateAssumedState() override {
        // Called once per frame: LVGL leaves framebuffer 0 bound, and mbgl's
        // dirty-state pass deliberately excludes the framebuffer binding, so an
        // assumption alone would make the later bind a no-op and the map would
        // be drawn to the window (then painted over). Really bind, then assume.
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        assumeFramebufferBinding(fbo);
        glViewport(0, 0, W, H);
        assumeViewport(0, 0, size);
    }

private:
    GLFWwindow *window;
    unsigned int tex = 0, fbo = 0, rbo = 0;
};

void TexRenderableResource::bind() {
    backend.setFramebufferBinding(backend.fboId());
    backend.setViewport(0, 0, backend.getSize());
}

void TexRenderableResource::swap() {
    // LVGL re-binds its own program/VAO/blend each draw but assumes the GL
    // default state for the rest; mbgl leaves these enabled.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
}

// ---- Frontend: forwards update params, renders on demand ----------------------
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
        auto params = updateParameters;
        renderer->render(params);
    }
private:
    TexBackend &backend;
    std::unique_ptr<mbgl::Renderer> renderer;
    std::shared_ptr<mbgl::UpdateParameters> updateParameters;
    std::atomic<bool> &dirty;
};

struct ProtoObserver : public mbgl::MapObserver {
    void onDidFinishLoadingStyle() override { fprintf(stderr, "[obs] style loaded\n"); }
    void onDidFailLoadingMap(mbgl::MapLoadError, const std::string &msg) override {
        fprintf(stderr, "[obs] MAP LOAD FAIL: %s\n", msg.c_str());
    }
    void onDidFinishRenderingFrame(const RenderFrameStatus &) override {
        static int n = 0;
        if (n++ < 3) fprintf(stderr, "[obs] frame rendered (%d)\n", n);
    }
};

// ---- Mouse state (GLFW callbacks → map pan/zoom) -------------------------------
static mbgl::Map *g_map = nullptr;
static bool g_down = false;
static double g_lastX = 0, g_lastY = 0;

int main(int argc, char **argv) {
    const char *style = (argc > 1) ? argv[1]
        : "file:///home/fab2/Developpement/LoRa_APRS/linux_tracker/maplibre/osm-bright.json";

    lv_init();
    lv_opengles_window_t *win = lv_opengles_glfw_window_create_ex(W, H, false, false, false,
                                                                  "proto MapLibre (blit)");
    GLFWwindow *gw = (GLFWwindow *)lv_opengles_glfw_window_get_glfw_window(win);
    glfwMakeContextCurrent(gw);
    fprintf(stderr, "[INFO] GPU: %s\n", glGetString(GL_RENDERER));

    mbgl::util::RunLoop runLoop;
    std::atomic<bool> dirty{true};

    TexBackend backend(gw);
    auto renderer = std::make_unique<mbgl::Renderer>(backend, 1.0f);
    TexFrontend frontend(std::move(renderer), backend, dirty);
    ProtoObserver observer;

    mbgl::ResourceOptions resOpts;
    mbgl::Map map(frontend, observer,
                  mbgl::MapOptions().withSize({W, H}).withPixelRatio(1.0f),
                  resOpts);
    g_map = &map;
    map.getStyle().loadURL(style);
    map.jumpTo(mbgl::CameraOptions().withCenter(mbgl::LatLng{43.5850, 1.4337}).withZoom(13.0));

    glfwSetMouseButtonCallback(gw, [](GLFWwindow *, int btn, int act, int) {
        if (btn == GLFW_MOUSE_BUTTON_LEFT) g_down = (act == GLFW_PRESS);
    });
    glfwSetCursorPosCallback(gw, [](GLFWwindow *, double x, double y) {
        if (g_down && g_map)
            g_map->moveBy({x - g_lastX, y - g_lastY});
        g_lastX = x; g_lastY = y;
    });
    glfwSetScrollCallback(gw, [](GLFWwindow *, double, double dy) {
        if (!g_map) return;
        double z = g_map->getCameraOptions().zoom.value_or(0);
        double target = dy > 0 ? std::floor(z) + 1.0 : std::ceil(z) - 1.0;
        if (target < 0) target = 0;
        g_map->jumpTo(mbgl::CameraOptions().withZoom(target));
    });

    // LVGL overlay display — renders widgets into a GL texture (no window compositor)
    lv_display_t *overlay = lv_opengles_texture_create(W, H);
    lv_obj_set_style_bg_opa(lv_display_get_screen_active(overlay), LV_OPA_TRANSP, 0);

    // Demo widget: translucent panel with callsign
    lv_obj_t *panel = lv_obj_create(lv_display_get_screen_active(overlay));
    lv_obj_set_size(panel, 200, 40);
    lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_50, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 5, 0);

    lv_obj_t *lbl = lv_label_create(panel);
    lv_label_set_text(lbl, "F4XXX-9  APRS");
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x00FF00), 0);
    lv_obj_center(lbl);

    lv_display_set_render_mode(overlay, LV_DISPLAY_RENDER_MODE_FULL);
    unsigned int overlay_tex = lv_opengles_texture_get_texture_id(overlay);
    lv_area_t overlay_area = {0, 0, W - 1, H - 1};

    mbgl::util::Timer tick;
    tick.start(mbgl::Duration::zero(), mbgl::Milliseconds(1000 / 60), [&] {
        if (glfwWindowShouldClose(gw)) { runLoop.stop(); return; }
        glfwPollEvents();
        if (dirty.exchange(false))
            frontend.render();

        // 1. Blit map to default framebuffer
        glBindFramebuffer(GL_READ_FRAMEBUFFER, backend.fboId());
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, W, H, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // 2. Render LVGL widgets into overlay texture
        lv_draw_buf_t *dbuf = lv_display_get_buf_active(overlay);
        if (dbuf && dbuf->data) memset(dbuf->data, 0, W * H * 4);
        lv_obj_invalidate(lv_display_get_screen_active(overlay));
        lv_display_t *prev = lv_display_get_default();
        lv_display_set_default(overlay);
        lv_refr_now(overlay);
        lv_display_set_default(prev);

        // Color-key: white background → transparent, then re-upload
        {
            uint32_t *px = (uint32_t *)dbuf->data;
            for (int i = 0; i < W * H; i++)
                if (px[i] == 0xFFFFFFFF) px[i] = 0;
            glBindTexture(GL_TEXTURE_2D, overlay_tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_BGRA, GL_UNSIGNED_BYTE, dbuf->data);
            glBindTexture(GL_TEXTURE_2D, 0);
        }

        // 3. Blend overlay on top of map
        lv_opengles_render(overlay_tex, &overlay_area, LV_OPA_COVER,
                           W, H, &overlay_area, false, false,
                           lv_color_hex(0x000000), true, true);
        glUseProgram(0);
        glBindVertexArray(0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

        char title[64];
        snprintf(title, sizeof(title), "proto MapLibre — z%.1f",
                 map.getCameraOptions().zoom.value_or(0));
        glfwSetWindowTitle(gw, title);
        glfwSwapBuffers(gw);
        runLoop.updateTime();
    });
    runLoop.run();
    return 0;
}
