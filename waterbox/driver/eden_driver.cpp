// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_driver.h"
#include "kept_images.h"
#include "memfs.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <semaphore>
#include <string>
#include <utility>
#include <vector>

#include <glad/glad.h>
#include <sys/mman.h>
#include <zlib.h>

#ifdef CHIMERA_GUEST
#include <emulibc.h>
#endif

#include "gl-bridge.h"

#include "audio_core/audio_core.h"
#include "audio_core/sink/chimera_sink.h"
#include "common/fs/path_util.h"
#include "common/input.h"
#include "common/param_package.h"
#include "common/settings_input.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/uuid.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/cpu_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/svc_types.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/loader.h"
#include "core/memory.h"
#include "hid_core/frontend/emulated_console.h"
#include "hid_core/hid_core.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"
#include "video_core/video_core.h"

namespace chimera_eden {

namespace {

// the picture: the console in the hand, or docked to a television, times
// the internal resolution
int SCREEN_W = 1280, SCREEN_H = 720;

// Memory no state carries (the sandbox's invisible pool; natively the heap):
// the picture, which is the engine's to keep, and the kept images' scratch.
void *alloc_unkept(size_t bytes) {
#ifdef CHIMERA_GUEST
    return alloc_invisible(bytes);
#else
    return calloc(1, bytes);
#endif
}

// The one GL context there is, the GPU bridge's: natively a real context
// that one thread holds at a time (the hooks hand it over), in the sandbox
// the host's, reached from whichever guest thread calls. Its "window" is a
// framebuffer object of the screen's size, read back at every present.
struct Screen {
    void (*bind)() = nullptr;
    void (*release)() = nullptr;
    bool opengl = false;
    bool readback = true;
    unsigned int fbo = 0, color = 0, vao = 0;
    // the context the renderer's objects (and these) were made in: it moves
    // when a savestate is loaded
    uint64_t context_id = 0;
    // the picture as GL reads it and as the frontend takes it: in memory no
    // state carries (a state is the machine; the frontend keeps pictures of
    // its own)
    uint8_t *rows = nullptr, *latest = nullptr;
    size_t allocated = 0;

    // (once the screen's size is known; that pool is never given back)
    void allocate() {
        const size_t bytes = static_cast<size_t>(SCREEN_W) * SCREEN_H * 4;
        if (bytes <= allocated)
            return;
        rows = static_cast<uint8_t *>(alloc_unkept(bytes));
        latest = static_cast<uint8_t *>(alloc_unkept(bytes));
        allocated = rows && latest ? bytes : 0;
    }

    unsigned int framebuffer() {
        if (fbo == 0) {
            // (made with the direct state calls: the renderer tracks what is
            // bound, and this runs in the middle of its present)
            glCreateRenderbuffers(1, &color);
            glNamedRenderbufferStorage(color, GL_RGBA8, SCREEN_W, SCREEN_H);
            glCreateFramebuffers(1, &fbo);
            glNamedFramebufferRenderbuffer(fbo, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
        }
        return fbo;
    }

    void present() {
        if (!readback || !opengl)
            return;
        if (getenv("CHIMERA_EDEN_GL_ERRORS")) {
            static unsigned long frames_seen, errors;
            for (GLenum e; (e = glGetError()) != GL_NO_ERROR;) {
                if (errors++ < 20)
                    fprintf(stderr, "GLERR present %lu: 0x%x\n", frames_seen, e);
            }
            frames_seen++;
        }
        const size_t stride = static_cast<size_t>(SCREEN_W) * 4;
        if (!rows || !latest)
            return;
        // The renderer's own state is left as it was found: its texture
        // downloads leave a pack buffer bound (a read would then go into
        // that buffer, at our pointer as an offset) and a row length set.
        GLint previous = 0, pack_buffer = 0, row_length = 0, alignment = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
        glGetIntegerv(GL_PACK_ROW_LENGTH, &row_length);
        glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer());
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, SCREEN_W, SCREEN_H, GL_BGRA, GL_UNSIGNED_BYTE, rows);
        glPixelStorei(GL_PACK_ALIGNMENT, alignment);
        glPixelStorei(GL_PACK_ROW_LENGTH, row_length);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(pack_buffer));
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previous));
        // GL's first row is the bottom one. The screen has no alpha: what
        // the renderer leaves there depends on where it took the layer from
        // (its texture cache gives 1, the machine's memory gives what the
        // game drew), so it is not part of the picture.
        for (int y = 0; y < SCREEN_H; y++) {
            const uint8_t *from = &rows[static_cast<size_t>(SCREEN_H - 1 - y) * stride];
            uint8_t *to = &latest[static_cast<size_t>(y) * stride];
            for (int x = 0; x < SCREEN_W; x++) {
                uint32_t pixel;
                std::memcpy(&pixel, from + x * 4, 4);
                pixel |= 0xFF000000u;
                std::memcpy(to + x * 4, &pixel, 4);
            }
        }
    }
};
Screen g_screen;

class BridgeContext : public Core::Frontend::GraphicsContext {
public:
    void MakeCurrent() override {
        if (g_screen.bind)
            g_screen.bind();
    }
    void DoneCurrent() override {
        if (g_screen.release)
            g_screen.release();
    }
    void SwapBuffers() override {
        g_screen.present();
    }
    unsigned int DefaultFramebuffer() override {
        return g_screen.framebuffer();
    }
};

class Window : public Core::Frontend::EmuWindow {
public:
    Window() {
        window_info.type = Core::Frontend::WindowSystemType::Headless;
        // one context: nothing may ask for a second one to compile shaders in
        strict_context_required = true;
        UpdateCurrentFramebufferLayout(SCREEN_W, SCREEN_H);
    }
    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override {
        return std::make_unique<BridgeContext>();
    }
    bool IsShown() const override { return true; }
    void OnFrameDisplayed() override { presented++; }
    std::atomic<uint64_t> presented{0};
};

// The bridge hands back every string in one buffer of its own; the renderer
// keeps the vendor's while it asks for the version's. Each name's string is
// kept apart here, for as long as the machine lives.
const GLubyte *stable_get_string(GLenum name) {
    static std::map<GLenum, std::string> kept;
    using Fn = const GLubyte *(*)(GLenum);
    const GLubyte *raw = reinterpret_cast<Fn>(chimera_gl_lookup("glGetString"))(name);
    if (!raw)
        return nullptr;
    std::string &s = kept[name];
    s = reinterpret_cast<const char *>(raw);
    return reinterpret_cast<const GLubyte *>(s.c_str());
}

const GLubyte *stable_get_stringi(GLenum name, GLuint index) {
    static std::map<std::pair<GLenum, GLuint>, std::string> kept;
    using Fn = const GLubyte *(*)(GLenum, GLuint);
    const GLubyte *raw = reinterpret_cast<Fn>(chimera_gl_lookup("glGetStringi"))(name, index);
    if (!raw)
        return nullptr;
    std::string &s = kept[{name, index}];
    s = reinterpret_cast<const char *>(raw);
    return reinterpret_cast<const GLubyte *>(s.c_str());
}

// The pad: every control of a player is an input device of the engine
// "chimera", made by the emulated controller from the settings below and
// found again here, so the driver can say what it reads at each frame.
constexpr int PLAYERS = 2;
struct PadControl;
// (a list each: the emulated controller makes its trigger devices from ZL's
// and ZR's parameters as well, and remakes every device on a reload)
std::vector<PadControl *> g_buttons[PLAYERS][Settings::NativeButton::NumButtons];
std::vector<PadControl *> g_sticks[PLAYERS][Settings::NativeAnalog::NumAnalogs];
// the touch screen's first finger (Eden's emulated console makes sixteen
// devices of the engine "touch", one a finger)
std::vector<PadControl *> g_touch;

struct PadControl final : Common::Input::InputDevice {
    std::vector<PadControl *> *slot = nullptr;
    ~PadControl() override {
        if (slot)
            std::erase(*slot, this);
    }
};

class PadFactory final : public Common::Input::Factory<Common::Input::InputDevice> {
public:
    std::unique_ptr<Common::Input::InputDevice> Create(const Common::ParamPackage &params) override {
        auto control = std::make_unique<PadControl>();
        if (params.Get("engine", "") == "touch") {
            if (params.Get("button", -1) == 0) {
                control->slot = &g_touch;
                g_touch.push_back(control.get());
            }
            return control;
        }
        const int player = params.Get("player", 0);
        // the battery, colour, camera and NFC devices are made from A's and
        // D-pad right's parameters with one of these added: not the button
        const bool derived = params.Has("color") || params.Has("battery") || params.Has("camera") || params.Has("nfc")
            || params.Has("output");
        if (player >= 0 && player < PLAYERS && !derived) {
            if (params.Has("button")) {
                const int b = params.Get("button", 0);
                if (b >= 0 && b < Settings::NativeButton::NumButtons)
                    control->slot = &g_buttons[player][b];
            } else if (params.Has("stick")) {
                const int a = params.Get("stick", 0);
                if (a >= 0 && a < Settings::NativeAnalog::NumAnalogs)
                    control->slot = &g_sticks[player][a];
            }
        }
        if (control->slot)
            control->slot->push_back(control.get());
        return control;
    }
};

// what each player's pad was last told, so only changes are sent
uint64_t g_pad_buttons[PLAYERS];
int32_t g_pad_axes[PLAYERS][4];

void configure_pads() {
    const auto factory = std::make_shared<PadFactory>();
    Common::Input::RegisterInputFactory("chimera", factory);
    Common::Input::RegisterInputFactory("touch", factory);
    auto &players = Settings::values.players.GetValue();
    for (int p = 0; p < static_cast<int>(players.size()); p++) {
        auto &player = players[p];
        player.connected = p == 0;
        player.controller_type = Settings::ControllerType::ProController;
        for (auto &b : player.buttons)
            b.clear();
        for (auto &a : player.analogs)
            a.clear();
        for (auto &m : player.motions)
            m.clear();
        if (p >= PLAYERS)
            continue;
        for (int b = 0; b < Settings::NativeButton::NumButtons; b++)
            player.buttons[b] = "engine:chimera,player:" + std::to_string(p) + ",button:" + std::to_string(b);
        for (int a = 0; a < Settings::NativeAnalog::NumAnalogs; a++)
            player.analogs[a] = "engine:chimera,player:" + std::to_string(p) + ",stick:" + std::to_string(a);
    }
}

// CHIMERA_EDEN_GL_STATE=<file>: what the context holds at this moment, one
// value a line under `label`. A renderer made in a context another one has
// used starts from whatever that one left; this is how the leftovers are
// read (the first dump is the context as the host made it).
void dump_gl_state(const char *label) {
    const char *path = getenv("CHIMERA_EDEN_GL_STATE");
    if (!path)
        return;
    FILE *f = fopen(path, "a");
    if (!f)
        return;
    fprintf(f, "== %s\n", label);
#define CAP(c) fprintf(f, #c " %d\n", glIsEnabled(c))
    CAP(GL_BLEND); CAP(GL_CULL_FACE); CAP(GL_DEPTH_TEST); CAP(GL_DEPTH_CLAMP); CAP(GL_DITHER);
    CAP(GL_FRAMEBUFFER_SRGB); CAP(GL_LINE_SMOOTH); CAP(GL_MULTISAMPLE); CAP(GL_POLYGON_OFFSET_FILL);
    CAP(GL_POLYGON_OFFSET_LINE); CAP(GL_POLYGON_OFFSET_POINT); CAP(GL_POLYGON_SMOOTH); CAP(GL_PRIMITIVE_RESTART);
    CAP(GL_PRIMITIVE_RESTART_FIXED_INDEX); CAP(GL_RASTERIZER_DISCARD); CAP(GL_SAMPLE_ALPHA_TO_COVERAGE);
    CAP(GL_SAMPLE_ALPHA_TO_ONE); CAP(GL_SAMPLE_COVERAGE); CAP(GL_SAMPLE_SHADING); CAP(GL_SAMPLE_MASK);
    CAP(GL_SCISSOR_TEST); CAP(GL_STENCIL_TEST); CAP(GL_TEXTURE_CUBE_MAP_SEAMLESS); CAP(GL_PROGRAM_POINT_SIZE);
    CAP(GL_COLOR_LOGIC_OP); CAP(GL_DEBUG_OUTPUT);
#undef CAP
    for (int i = 0; i < 8; i++)
        fprintf(f, "GL_CLIP_DISTANCE%d %d\n", i, glIsEnabled(GL_CLIP_DISTANCE0 + i));
    for (int i = 0; i < 8; i++)
        fprintf(f, "GL_BLEND[%d] %d\n", i, glIsEnabledi(GL_BLEND, i));
    for (int i = 0; i < 16; i++)
        fprintf(f, "GL_SCISSOR_TEST[%d] %d\n", i, glIsEnabledi(GL_SCISSOR_TEST, i));
#define INT(c, n) do { GLint v[4] = {}; glGetIntegerv(c, v); fprintf(f, #c); \
        for (int k = 0; k < n; k++) fprintf(f, " %d", v[k]); fprintf(f, "\n"); } while (0)
#define FLT(c, n) do { GLfloat v[4] = {}; glGetFloatv(c, v); fprintf(f, #c); \
        for (int k = 0; k < n; k++) fprintf(f, " %g", v[k]); fprintf(f, "\n"); } while (0)
    INT(GL_ACTIVE_TEXTURE, 1); INT(GL_ARRAY_BUFFER_BINDING, 1); INT(GL_CULL_FACE_MODE, 1); INT(GL_FRONT_FACE, 1);
    INT(GL_CURRENT_PROGRAM, 1); INT(GL_PROGRAM_PIPELINE_BINDING, 1); INT(GL_DEPTH_FUNC, 1); INT(GL_DEPTH_WRITEMASK, 1);
    INT(GL_DRAW_FRAMEBUFFER_BINDING, 1); INT(GL_READ_FRAMEBUFFER_BINDING, 1); INT(GL_RENDERBUFFER_BINDING, 1);
    INT(GL_LOGIC_OP_MODE, 1); INT(GL_PACK_ALIGNMENT, 1); INT(GL_PACK_ROW_LENGTH, 1); INT(GL_PACK_SKIP_PIXELS, 1);
    INT(GL_PACK_SKIP_ROWS, 1); INT(GL_PACK_IMAGE_HEIGHT, 1); INT(GL_PACK_SKIP_IMAGES, 1); INT(GL_UNPACK_ALIGNMENT, 1);
    INT(GL_UNPACK_ROW_LENGTH, 1); INT(GL_UNPACK_SKIP_PIXELS, 1); INT(GL_UNPACK_SKIP_ROWS, 1);
    INT(GL_UNPACK_IMAGE_HEIGHT, 1); INT(GL_UNPACK_SKIP_IMAGES, 1); INT(GL_PIXEL_PACK_BUFFER_BINDING, 1);
    INT(GL_PIXEL_UNPACK_BUFFER_BINDING, 1); INT(GL_POLYGON_MODE, 2); INT(GL_PRIMITIVE_RESTART_INDEX, 1);
    INT(GL_PROVOKING_VERTEX, 1); INT(GL_STENCIL_FUNC, 1); INT(GL_STENCIL_REF, 1); INT(GL_STENCIL_VALUE_MASK, 1);
    INT(GL_STENCIL_WRITEMASK, 1); INT(GL_STENCIL_FAIL, 1); INT(GL_STENCIL_PASS_DEPTH_FAIL, 1);
    INT(GL_STENCIL_PASS_DEPTH_PASS, 1); INT(GL_STENCIL_BACK_FUNC, 1); INT(GL_STENCIL_BACK_REF, 1);
    INT(GL_STENCIL_BACK_VALUE_MASK, 1); INT(GL_STENCIL_BACK_WRITEMASK, 1); INT(GL_STENCIL_BACK_FAIL, 1);
    INT(GL_STENCIL_BACK_PASS_DEPTH_FAIL, 1); INT(GL_STENCIL_BACK_PASS_DEPTH_PASS, 1); INT(GL_STENCIL_CLEAR_VALUE, 1);
    INT(GL_CLIP_ORIGIN, 1); INT(GL_CLIP_DEPTH_MODE, 1); INT(GL_PATCH_VERTICES, 1);
    INT(GL_TRANSFORM_FEEDBACK_BINDING, 1); INT(GL_TRANSFORM_FEEDBACK_BUFFER_BINDING, 1); INT(GL_VERTEX_ARRAY_BINDING, 1);
    INT(GL_DRAW_INDIRECT_BUFFER_BINDING, 1); INT(GL_COPY_READ_BUFFER_BINDING, 1); INT(GL_COPY_WRITE_BUFFER_BINDING, 1);
    INT(GL_DISPATCH_INDIRECT_BUFFER_BINDING, 1); INT(GL_TEXTURE_BUFFER_BINDING, 1); INT(GL_QUERY_BUFFER_BINDING, 1);
    INT(GL_UNIFORM_BUFFER_BINDING, 1); INT(GL_SHADER_STORAGE_BUFFER_BINDING, 1); INT(GL_POINT_SPRITE_COORD_ORIGIN, 1);
    INT(GL_FRAGMENT_SHADER_DERIVATIVE_HINT, 1); INT(GL_BLEND_SRC_RGB, 1); INT(GL_BLEND_DST_RGB, 1);
    INT(GL_BLEND_SRC_ALPHA, 1); INT(GL_BLEND_DST_ALPHA, 1); INT(GL_BLEND_EQUATION_RGB, 1);
    INT(GL_BLEND_EQUATION_ALPHA, 1); INT(GL_DRAW_BUFFER, 1); INT(GL_READ_BUFFER, 1);
    FLT(GL_POINT_SIZE, 1); FLT(GL_LINE_WIDTH, 1); FLT(GL_POLYGON_OFFSET_FACTOR, 1); FLT(GL_POLYGON_OFFSET_UNITS, 1);
    FLT(GL_POLYGON_OFFSET_CLAMP, 1); FLT(GL_SAMPLE_COVERAGE_VALUE, 1); FLT(GL_MIN_SAMPLE_SHADING_VALUE, 1);
    FLT(GL_COLOR_CLEAR_VALUE, 4); FLT(GL_DEPTH_CLEAR_VALUE, 1); FLT(GL_BLEND_COLOR, 4);
    FLT(GL_POINT_FADE_THRESHOLD_SIZE, 1); FLT(GL_PATCH_DEFAULT_OUTER_LEVEL, 4); FLT(GL_PATCH_DEFAULT_INNER_LEVEL, 2);
#undef INT
#undef FLT
    for (int i = 0; i < 8; i++) {
        GLint v[6] = {};
        GLboolean m[4] = {};
        glGetIntegeri_v(GL_BLEND_SRC_RGB, i, &v[0]); glGetIntegeri_v(GL_BLEND_DST_RGB, i, &v[1]);
        glGetIntegeri_v(GL_BLEND_SRC_ALPHA, i, &v[2]); glGetIntegeri_v(GL_BLEND_DST_ALPHA, i, &v[3]);
        glGetIntegeri_v(GL_BLEND_EQUATION_RGB, i, &v[4]); glGetIntegeri_v(GL_BLEND_EQUATION_ALPHA, i, &v[5]);
        glGetBooleani_v(GL_COLOR_WRITEMASK, i, m);
        fprintf(f, "blend[%d] %x %x %x %x %x %x mask %d%d%d%d\n", i, v[0], v[1], v[2], v[3], v[4], v[5], m[0], m[1],
                m[2], m[3]);
    }
    for (int i = 0; i < 16; i++) {
        GLfloat vp[4] = {};
        GLint sc[4] = {};
        GLdouble dr[2] = {};
        glGetFloati_v(GL_VIEWPORT, i, vp); glGetIntegeri_v(GL_SCISSOR_BOX, i, sc); glGetDoublei_v(GL_DEPTH_RANGE, i, dr);
        fprintf(f, "viewport[%d] %g %g %g %g scissor %d %d %d %d range %g %g\n", i, vp[0], vp[1], vp[2], vp[3], sc[0],
                sc[1], sc[2], sc[3], dr[0], dr[1]);
    }
    for (int i = 0; i < 32; i++) {
        GLint t2 = 0, ta = 0, tc = 0, t3 = 0, tb = 0, ms = 0, sm = 0, im = 0;
        glGetIntegeri_v(GL_TEXTURE_BINDING_2D, i, &t2); glGetIntegeri_v(GL_TEXTURE_BINDING_2D_ARRAY, i, &ta);
        glGetIntegeri_v(GL_TEXTURE_BINDING_CUBE_MAP, i, &tc); glGetIntegeri_v(GL_TEXTURE_BINDING_3D, i, &t3);
        glGetIntegeri_v(GL_TEXTURE_BINDING_BUFFER, i, &tb); glGetIntegeri_v(GL_TEXTURE_BINDING_2D_MULTISAMPLE, i, &ms);
        glGetIntegeri_v(GL_SAMPLER_BINDING, i, &sm);
        if (i < 8)
            glGetIntegeri_v(GL_IMAGE_BINDING_NAME, i, &im);
        if (t2 | ta | tc | t3 | tb | ms | sm | im)
            fprintf(f, "unit[%d] 2d %d array %d cube %d 3d %d buffer %d ms %d sampler %d image %d\n", i, t2, ta, tc, t3,
                    tb, ms, sm, im);
    }
    for (int i = 0; i < 90; i++) {
        GLint ub = 0, ss = 0;
        glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, i, &ub);
        if (i < 16)
            glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, i, &ss);
        if (ub | ss)
            fprintf(f, "block[%d] uniform %d storage %d\n", i, ub, ss);
    }
    static const GLenum QUERIES[] = {GL_SAMPLES_PASSED, GL_ANY_SAMPLES_PASSED, GL_ANY_SAMPLES_PASSED_CONSERVATIVE,
                                     GL_PRIMITIVES_GENERATED, GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN,
                                     GL_TIME_ELAPSED};
    for (const GLenum q : QUERIES) {
        GLint id = 0;
        glGetQueryiv(q, GL_CURRENT_QUERY, &id);
        fprintf(f, "query %x %d\n", q, id);
    }
    while (glGetError() != GL_NO_ERROR) {
    }
    fclose(f);
}

// The context as a new one is: a renderer takes that for granted (the state
// tracker's idea of the clip control, a query it believes is not running,
// pixel store it never set), and after a state load the context is the one
// later frames left behind. Everything a renderer of ours touches, put back.
void reset_gl_state() {
    static const GLenum QUERIES[] = {GL_SAMPLES_PASSED, GL_ANY_SAMPLES_PASSED, GL_ANY_SAMPLES_PASSED_CONSERVATIVE,
                                     GL_PRIMITIVES_GENERATED, GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN,
                                     GL_TIME_ELAPSED};
    for (const GLenum q : QUERIES) {
        GLint running = 0;
        glGetQueryiv(q, GL_CURRENT_QUERY, &running);
        if (running)
            glEndQuery(q);
    }
    GLint feedback = 0;
    glGetIntegerv(GL_TRANSFORM_FEEDBACK_ACTIVE, &feedback);
    if (feedback)
        glEndTransformFeedback();
    glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, 0);

    static const GLenum CAPS[] = {GL_BLEND, GL_CULL_FACE, GL_DEPTH_TEST, GL_DEPTH_CLAMP, GL_FRAMEBUFFER_SRGB,
                                  GL_LINE_SMOOTH, GL_POLYGON_OFFSET_FILL, GL_POLYGON_OFFSET_LINE,
                                  GL_POLYGON_OFFSET_POINT, GL_POLYGON_SMOOTH, GL_PRIMITIVE_RESTART,
                                  GL_PRIMITIVE_RESTART_FIXED_INDEX, GL_RASTERIZER_DISCARD,
                                  GL_SAMPLE_ALPHA_TO_COVERAGE, GL_SAMPLE_ALPHA_TO_ONE, GL_SAMPLE_COVERAGE,
                                  GL_SAMPLE_SHADING, GL_SAMPLE_MASK, GL_SCISSOR_TEST, GL_STENCIL_TEST,
                                  GL_TEXTURE_CUBE_MAP_SEAMLESS, GL_PROGRAM_POINT_SIZE, GL_COLOR_LOGIC_OP,
                                  GL_DEBUG_OUTPUT, GL_DEBUG_OUTPUT_SYNCHRONOUS};
    for (const GLenum cap : CAPS)
        glDisable(cap);
    glEnable(GL_DITHER);
    glEnable(GL_MULTISAMPLE);
    for (int i = 0; i < 8; i++)
        glDisable(GL_CLIP_DISTANCE0 + i);
    GLint buffers = 8, viewports = 16;
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &buffers);
    glGetIntegerv(GL_MAX_VIEWPORTS, &viewports);
    for (int i = 0; i < buffers; i++)
        glDisablei(GL_BLEND, i);
    for (int i = 0; i < viewports; i++) {
        glDisablei(GL_SCISSOR_TEST, i);
        glDepthRangeIndexed(i, 0.0, 1.0);
    }

    glFrontFace(GL_CCW);
    glCullFace(GL_BACK);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glClipControl(GL_LOWER_LEFT, GL_NEGATIVE_ONE_TO_ONE);
    glBlendFunc(GL_ONE, GL_ZERO);
    glBlendEquation(GL_FUNC_ADD);
    glBlendColor(0, 0, 0, 0);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 0);
    glClearDepth(1.0);
    glClearStencil(0);
    glStencilFunc(GL_ALWAYS, 0, ~0u);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glStencilMask(~0u);
    glLogicOp(GL_COPY);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glPolygonOffsetClamp(0, 0, 0);
    glLineWidth(1);
    glPointSize(1);
    glPointParameteri(GL_POINT_SPRITE_COORD_ORIGIN, GL_UPPER_LEFT);
    glPointParameterf(GL_POINT_FADE_THRESHOLD_SIZE, 1);
    glProvokingVertex(GL_LAST_VERTEX_CONVENTION);
    glPrimitiveRestartIndex(0);
    glSampleCoverage(1, GL_FALSE);
    glMinSampleShading(0);
    glSampleMaski(0, ~0u);
    glPatchParameteri(GL_PATCH_VERTICES, 3);
    static const GLfloat ONES[4] = {1, 1, 1, 1};
    glPatchParameterfv(GL_PATCH_DEFAULT_OUTER_LEVEL, ONES);
    glPatchParameterfv(GL_PATCH_DEFAULT_INNER_LEVEL, ONES);
    for (const GLenum store : {GL_PACK_ROW_LENGTH, GL_PACK_SKIP_PIXELS, GL_PACK_SKIP_ROWS, GL_PACK_IMAGE_HEIGHT,
                               GL_PACK_SKIP_IMAGES, GL_UNPACK_ROW_LENGTH, GL_UNPACK_SKIP_PIXELS, GL_UNPACK_SKIP_ROWS,
                               GL_UNPACK_IMAGE_HEIGHT, GL_UNPACK_SKIP_IMAGES})
        glPixelStorei(store, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    glUseProgram(0);
    glBindProgramPipeline(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glBindVertexArray(0);
    for (const GLenum target : {GL_ARRAY_BUFFER, GL_PIXEL_PACK_BUFFER, GL_PIXEL_UNPACK_BUFFER, GL_COPY_READ_BUFFER,
                                GL_COPY_WRITE_BUFFER, GL_DRAW_INDIRECT_BUFFER, GL_DISPATCH_INDIRECT_BUFFER,
                                GL_TEXTURE_BUFFER, GL_QUERY_BUFFER, GL_UNIFORM_BUFFER, GL_SHADER_STORAGE_BUFFER,
                                GL_TRANSFORM_FEEDBACK_BUFFER, GL_ATOMIC_COUNTER_BUFFER})
        glBindBuffer(target, 0);
    GLint units = 0, images = 0, uniforms = 0, storages = 0, feedbacks = 0, counters = 0;
    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &units);
    glGetIntegerv(GL_MAX_IMAGE_UNITS, &images);
    glGetIntegerv(GL_MAX_UNIFORM_BUFFER_BINDINGS, &uniforms);
    glGetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &storages);
    glGetIntegerv(GL_MAX_TRANSFORM_FEEDBACK_BUFFERS, &feedbacks);
    glGetIntegerv(GL_MAX_ATOMIC_COUNTER_BUFFER_BINDINGS, &counters);
    glBindTextures(0, units, nullptr);
    glBindSamplers(0, units, nullptr);
    glBindImageTextures(0, images, nullptr);
    glBindBuffersBase(GL_UNIFORM_BUFFER, 0, uniforms, nullptr);
    glBindBuffersBase(GL_SHADER_STORAGE_BUFFER, 0, storages, nullptr);
    glBindBuffersBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, feedbacks, nullptr);
    glBindBuffersBase(GL_ATOMIC_COUNTER_BUFFER, 0, counters, nullptr);
    glActiveTexture(GL_TEXTURE0);
    while (glGetError() != GL_NO_ERROR) {
    }
}

// Between the old renderer and the new one, when a state load has moved the
// GL context: the driver's own objects are names of the old context too.
void rebuild_own_gl() {
    if (g_screen.fbo)
        glDeleteFramebuffers(1, &g_screen.fbo);
    if (g_screen.color)
        glDeleteRenderbuffers(1, &g_screen.color);
    if (g_screen.vao)
        glDeleteVertexArrays(1, &g_screen.vao);
    g_screen.fbo = g_screen.color = 0;
    dump_gl_state("left behind");
    reset_gl_state();
    dump_gl_state("rebuild");
    glGenVertexArrays(1, &g_screen.vao);
    glBindVertexArray(g_screen.vao);
    g_screen.context_id = chimera_gl_context_id();
}

// An entry point the bridge does not carry. glad would leave its pointer
// null, and a call through it is a jump to address 0 that names nothing;
// each gets a stub of its own instead, which says the function's name in the
// machine's last words and stops.
std::vector<std::string> g_missing_gl;
[[noreturn]] void missing_gl_called(size_t index) {
    fprintf(stderr, "Eden's renderer called %s, which Chimera's GPU bridge does not carry\n",
            index < g_missing_gl.size() ? g_missing_gl[index].c_str() : "a GL function");
    fflush(stderr);
    abort();
}
template <size_t I>
void missing_gl_stub() {
    missing_gl_called(I);
}
template <size_t... I>
constexpr std::array<void (*)(), sizeof...(I)> missing_gl_stubs(std::index_sequence<I...>) {
    return {&missing_gl_stub<I>...};
}
constexpr auto MISSING_GL = missing_gl_stubs(std::make_index_sequence<4096>{});

void *bridge_lookup(const char *name) {
    if (std::strcmp(name, "glGetString") == 0)
        return reinterpret_cast<void *>(&stable_get_string);
    if (std::strcmp(name, "glGetStringi") == 0)
        return reinterpret_cast<void *>(&stable_get_stringi);
    if (void *found = chimera_gl_lookup(name))
        return found;
    if (g_missing_gl.size() >= MISSING_GL.size())
        return nullptr;
    g_missing_gl.emplace_back(name);
    return reinterpret_cast<void *>(MISSING_GL[g_missing_gl.size() - 1]);
}

// glad's entry points through the bridge; false if the context is not what
// Eden's renderer needs.
bool load_gl(std::string &error) {
    if (g_screen.bind)
        g_screen.bind();
    const bool ok = gladLoadGLLoader(bridge_lookup) != 0;
    if (ok) {
        // The vendor paths whose entry points the bridge does not carry
        // (NVIDIA's unified memory, assembly programs, draw texture, viewport
        // swizzle) are never taken: the renderer then draws the same way on
        // every GPU.
        GLAD_GL_NV_vertex_buffer_unified_memory = 0;
        GLAD_GL_NV_gpu_program5 = 0;
        GLAD_GL_NV_compute_program5 = 0;
        GLAD_GL_NV_transform_feedback = 0;
        GLAD_GL_NV_transform_feedback2 = 0;
        GLAD_GL_NV_draw_texture = 0;
        GLAD_GL_NV_viewport_swizzle = 0;
        GLAD_GL_NV_depth_buffer_float = 0;
        // The renderer draws with the default vertex array, which only a
        // compatibility profile has; the bridge's context is a core profile
        // one, where a draw with no vertex array bound is refused (every draw
        // was, and the picture stayed black). One array, bound for good,
        // stands in for the default.
        dump_gl_state("fresh");
        glGenVertexArrays(1, &g_screen.vao);
        glBindVertexArray(g_screen.vao);
        g_screen.context_id = chimera_gl_context_id();
    }
    if (g_screen.release)
        g_screen.release();
    if (!ok)
        error = "error.gl_not_loaded";
    return ok;
}

struct Machine {
    // the updates and DLC the project names (made before the system, which
    // holds a pointer to it, and so gone after it)
    FileSys::ManualContentProvider content;
    Core::System system;
    Window window;
    // The frame: a CoreTiming event at each frame's end parks the CPU thread
    // (the one thread the machine runs on) and wakes the driver; frame()
    // lets it go again. The first event, at time 0, holds the machine until
    // the first frame() is asked for.
    std::shared_ptr<Core::Timing::EventType> frame_event;
    std::binary_semaphore go{0}, done{0};
    uint64_t frame = 0;
    // CoreTiming's clock when the frame's end event ran: later than the
    // frame's end by what the running slice had left
    uint64_t ended_ns = 0;
    // the sound of the last frame: stereo pairs, interleaved
    std::vector<int16_t> audio;
    // the frame in which the game exited (0: it runs)
    uint64_t exited = 0;
    bool rebuild_every_frame = false;
    // the gate's negative control: CHIMERA_EDEN_NO_KEEP rebuilds with the
    // upscaled images untold, as after a state the core was not told about
    bool keep_before_rebuild = true;
    // the gate's negative control: CHIMERA_EDEN_NO_QUIESCE leaves what the
    // GPU drew on the GPU, and a renderer made again must then draw wrong
    bool quiesce = true;
};
std::unique_ptr<Machine> g_machine;

// Frame f ends f sixtieths of a second into the machine's time (never
// accumulated, so no rounding drifts), a microsecond after the vsync that
// composes it.
uint64_t frame_end_ns(uint64_t f) {
    return f * 1000000000ull / 60 + 1000;
}

} // namespace

// A whole file the frontend provided (a path natively, a mounted file's name
// in the sandbox).
bool read_host_file(const std::string &name, std::vector<uint8_t> &out) {
    FILE *f = fopen(name.c_str(), "rb");
    if (!f)
        return false;
    out.clear();
    uint8_t buf[1 << 16];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        out.insert(out.end(), buf, buf + n);
    fclose(f);
    return true;
}

uint32_t le32(const uint8_t *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t le16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

// A plain zip reader (stored and deflated entries, no zip64: a firmware is
// 340 MiB), over zlib: `each` is given every file's name and bytes, and stops
// the walk by returning false with `error` set. `what` names the zip in the
// reader's own errors: a key, "zip.system_software" or "zip.save_data".
bool walk_zip(const std::vector<uint8_t> &zip, const std::string &what,
              const std::function<bool(const std::string &, std::vector<uint8_t> &&, std::string &)> &each,
              std::string &error) {
    // the end of central directory record, searched from the end
    size_t eocd = std::string::npos;
    for (size_t i = zip.size() >= 22 ? zip.size() - 22 : 0; zip.size() >= 22; i--) {
        if (le32(&zip[i]) == 0x06054b50) {
            eocd = i;
            break;
        }
        if (i == 0 || zip.size() - i > 22 + 65535)
            break;
    }
    if (eocd == std::string::npos) {
        error = std::string("error.zip_not_zip" TEXT_ARG) + what;
        return false;
    }
    const size_t count = le16(&zip[eocd + 10]);
    size_t at = le32(&zip[eocd + 16]);
    for (size_t e = 0; e < count; e++) {
        if (at + 46 > zip.size() || le32(&zip[at]) != 0x02014b50) {
            error = std::string("error.zip_damaged" TEXT_ARG) + what;
            return false;
        }
        const unsigned method = le16(&zip[at + 10]);
        const size_t packed = le32(&zip[at + 20]), size = le32(&zip[at + 24]);
        const size_t name_len = le16(&zip[at + 28]), extra_len = le16(&zip[at + 30]), comment_len = le16(&zip[at + 32]);
        const size_t local = le32(&zip[at + 42]);
        if (at + 46 + name_len > zip.size()) {
            error = std::string("error.zip_damaged" TEXT_ARG) + what;
            return false;
        }
        std::string name(reinterpret_cast<const char *>(&zip[at + 46]), name_len);
        at += 46 + name_len + extra_len + comment_len;
        if (name.empty() || name.back() == '/')
            continue; // a folder's own entry
        if (local + 30 > zip.size() || le32(&zip[local]) != 0x04034b50) {
            error = std::string("error.zip_damaged" TEXT_ARG) + what;
            return false;
        }
        const size_t data = local + 30 + le16(&zip[local + 26]) + le16(&zip[local + 28]);
        if (data + packed > zip.size()) {
            error = std::string("error.zip_cut_short" TEXT_ARG) + what;
            return false;
        }
        std::vector<uint8_t> out(size);
        if (method == 0 && packed == size) {
            if (size)
                std::memcpy(out.data(), &zip[data], size);
        } else if (method == 8) {
            z_stream z{};
            if (inflateInit2(&z, -MAX_WBITS) != Z_OK) {
                error = "error.zlib_failed";
                return false;
            }
            z.next_in = const_cast<Bytef *>(&zip[data]);
            z.avail_in = static_cast<uInt>(packed);
            z.next_out = out.data();
            z.avail_out = static_cast<uInt>(size);
            const int rc = inflate(&z, Z_FINISH);
            inflateEnd(&z);
            if (rc != Z_STREAM_END || z.total_out != size) {
                error = std::string("error.zip_entry_broken" TEXT_ARG) + what + TEXT_ARG + name;
                return false;
            }
        } else {
            error = std::string("error.zip_entry_method" TEXT_ARG) + what + TEXT_ARG + name;
            return false;
        }
        if (!each(name, std::move(out), error))
            return false;
    }
    return true;
}

// The system software: every .nca of the frontend's zip into the system
// NAND's registered contents, flat, as Eden's own installer leaves them.
bool install_firmware(const std::string &host_name, const std::string &registered, std::string &error) {
    std::vector<uint8_t> zip;
    if (!read_host_file(host_name, zip)) {
        error = std::string("error.firmware_unreadable" TEXT_ARG) + host_name;
        return false;
    }
    size_t installed = 0;
    const bool ok = walk_zip(
        zip, "zip.system_software",
        [&](const std::string &entry, std::vector<uint8_t> &&bytes, std::string &why) {
            // a dump may sit in a folder; a name is the file's own
            std::string name = entry;
            if (const size_t slash = name.find_last_of('/'); slash != std::string::npos)
                name.erase(0, slash + 1);
            if (name.size() < 4 || name.compare(name.size() - 4, 4, ".nca") != 0)
                return true;
            if (!chimera::memfs::put(registered + "/" + name, std::move(bytes))) {
                why = std::string("error.firmware_not_installed" TEXT_ARG) + name;
                return false;
            }
            installed++;
            return true;
        },
        error);
    if (!ok)
        return false;
    if (installed == 0) {
        error = std::string("error.firmware_no_nca" TEXT_ARG) + host_name;
        return false;
    }
    return true;
}

bool is_hex(const std::string &s, size_t digits) {
    if (s.size() != digits)
        return false;
    for (const char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            return false;
    return true;
}
std::string upper(std::string s) {
    for (char &c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::vector<std::string> split_path(const std::string &path) {
    std::vector<std::string> parts;
    size_t from = 0;
    while (from <= path.size()) {
        const size_t slash = path.find('/', from);
        const std::string part = path.substr(from, slash == std::string::npos ? std::string::npos : slash - from);
        if (!part.empty() && part != ".")
            parts.push_back(part);
        if (slash == std::string::npos)
            break;
        from = slash + 1;
    }
    return parts;
}

// The save data area: Eden keeps a game's save as plain files under
// nand/user/save/0000000000000000/<user>/<TITLE ID>/, and the console's own
// (the saves no user owns) under the user 0.
constexpr const char *SAVE_AREA = "/chimera/user/nand/user/save/0000000000000000";
constexpr const char *NO_USER = "00000000000000000000000000000000";

// "<32 hex digits>": the machine's one user, as the save area names it
std::string user_folder(Core::System &system) {
    const auto uuid = system.GetProfileManager().GetUser(0).value_or(Common::UUID::MakeDefault());
    const auto id = uuid.AsU128();
    char buf[40];
    snprintf(buf, sizeof buf, "%016llX%016llX", static_cast<unsigned long long>(id[1]),
             static_cast<unsigned long long>(id[0]));
    return buf;
}

// What the game already saved, from the frontend's zip into the save area
// before the game runs. Taken, an entry at a time:
//   <TITLE ID>/...             this core's own export: the user's save
//   device/<TITLE ID>/...      and the console's
//   .../save/0000000000000000/<user>/<TITLE ID>/...
//                              a save area copied out of Eden (or yuzu),
//                              whatever stands in front and whoever the user
//                              was there
// and a zip with none of those is the game's save folder itself (what Eden's
// "Open Save Data Location" shows), which goes to the game being run.
bool install_savedata(Core::System &system, const std::string &host_name, std::string &error) {
    std::vector<uint8_t> zip;
    if (!read_host_file(host_name, zip)) {
        error = std::string("error.savedata_unreadable" TEXT_ARG) + host_name;
        return false;
    }
    const std::string user = user_folder(system);
    char title[20];
    snprintf(title, sizeof title, "%016llX", static_cast<unsigned long long>(system.GetApplicationProcessProgramID()));
    struct Entry {
        std::string name;
        std::vector<uint8_t> bytes;
    };
    std::vector<Entry> placed, bare;
    const bool ok = walk_zip(
        zip, "zip.save_data",
        [&](const std::string &entry, std::vector<uint8_t> &&bytes, std::string &) {
            const std::vector<std::string> parts = split_path(entry);
            for (const std::string &part : parts)
                if (part == "..")
                    return true;
            const auto rest = [&](size_t from) {
                std::string out;
                for (size_t i = from; i < parts.size(); i++)
                    out += "/" + parts[i];
                return out;
            };
            for (size_t i = 0; i + 4 < parts.size(); i++) {
                if (parts[i] == "save" && parts[i + 1] == "0000000000000000" && is_hex(parts[i + 2], 32)
                    && is_hex(parts[i + 3], 16)) {
                    const std::string owner = parts[i + 2] == NO_USER ? NO_USER : user;
                    placed.push_back({owner + "/" + upper(parts[i + 3]) + rest(i + 4), std::move(bytes)});
                    return true;
                }
            }
            if (parts.size() >= 3 && parts[0] == "device" && is_hex(parts[1], 16))
                placed.push_back({std::string(NO_USER) + "/" + upper(parts[1]) + rest(2), std::move(bytes)});
            else if (parts.size() >= 2 && is_hex(parts[0], 16))
                placed.push_back({user + "/" + upper(parts[0]) + rest(1), std::move(bytes)});
            else if (!parts.empty() && parts[0] != "__MACOSX")
                bare.push_back({user + "/" + title + rest(0), std::move(bytes)});
            return true;
        },
        error);
    if (!ok)
        return false;
    std::vector<Entry> &take = placed.empty() ? bare : placed;
    if (take.empty()) {
        error = std::string("error.savedata_empty" TEXT_ARG) + host_name;
        return false;
    }
    for (Entry &e : take) {
        const std::string path = std::string(SAVE_AREA) + "/" + e.name;
        chimera::memfs::mkdirs(path.substr(0, path.find_last_of('/')));
        if (!chimera::memfs::put(path, std::move(e.bytes))) {
            error = std::string("error.savedata_not_installed" TEXT_ARG) + e.name;
            return false;
        }
    }
    return true;
}

int64_t machine_seconds() {
    const int64_t start = Settings::values.custom_rtc.GetValue();
    if (!g_machine)
        return start;
    return start + g_machine->system.CoreTiming().GetGlobalTimeNs().count() / 1000000000;
}

constexpr const char *TREE = "/chimera";

bool boot(const std::string &game, const Options &options, std::string &error) {
    auto &v = Settings::values;
    v.custom_rtc_enabled.SetValue(true);
    v.custom_rtc.SetValue(static_cast<s64>(options.rtc_start ? options.rtc_start : 1488499200)); // 2017-03-03 UTC
    // a timezone from the host would show in the dates the machine formats
    setenv("TZ", "UTC0", 1);
    tzset();
    v.time_zone_index.SetValue(Settings::TimeZone::Utc);

    // Everything the emulator keeps lives in the machine's own filesystem,
    // the same in both flavours of the core: the console's NAND and SD card,
    // its keys, config, caches and log. The game is grafted in read-only.
    chimera::memfs::set_clock(machine_seconds);
    const std::string tree = TREE;
    const std::string user = tree + "/user";
    for (const char *d : { "/keys", "/nand/system/Contents/registered", "/nand/user", "/sdmc", "/log", "/config", "/cache",
             "/load", "/dump" })
        chimera::memfs::mkdirs(user + d);
    chimera::memfs::mkdirs(tree + "/rom");

    std::vector<uint8_t> bytes;
    if (options.prod_keys.empty() || !read_host_file(options.prod_keys, bytes)) {
        error = "error.no_prod_keys";
        return false;
    }
    chimera::memfs::put(user + "/keys/prod.keys", std::move(bytes));
    if (!options.title_keys.empty() && read_host_file(options.title_keys, bytes))
        chimera::memfs::put(user + "/keys/title.keys", std::move(bytes));
    if (!options.firmware.empty() && !install_firmware(options.firmware, user + "/nand/system/Contents/registered", error))
        return false;

    const auto base_name = [](std::string name) {
        if (const size_t slash = name.find_last_of('/'); slash != std::string::npos)
            name.erase(0, slash + 1);
        return name;
    };
    const std::string rom = tree + "/rom/" + base_name(game);
    if (!chimera::memfs::graft(rom, game)) {
        error = std::string("error.game_unreadable" TEXT_ARG) + game;
        return false;
    }
    // the updates and DLC, read-only beside it (numbered: two files may
    // carry the same name)
    std::vector<std::string> content;
    for (size_t i = 0; i < options.content.size(); i++) {
        content.push_back(tree + "/content/" + std::to_string(i) + "-" + base_name(options.content[i]));
        chimera::memfs::mkdirs(tree + "/content");
        if (!chimera::memfs::graft(content.back(), options.content[i])) {
            error = std::string("error.content_unreadable" TEXT_ARG) + options.content[i];
            return false;
        }
    }

    Common::FS::SetAppDirectory(user);
    Common::Log::Initialize();
    if (!options.log_filter.empty()) {
        Common::Log::Filter filter;
        filter.ParseFilterString(options.log_filter);
        Common::Log::SetGlobalFilter(filter);
    }
    Common::Log::Start();

    configure_pads();
    // one host thread runs every guest core in turn, and time is the
    // instructions it ran (CoreTiming's cpu_ticks), never the host's clock
    v.use_multi_core.SetValue(getenv("CHIMERA_EDEN_MULTICORE") != nullptr);
    v.use_asynchronous_gpu_emulation.SetValue(false);
    v.use_asynchronous_shaders.SetValue(false);
    v.use_disk_shader_cache.SetValue(false);
    v.use_speed_limit.SetValue(false);
    // video is decoded by the core's own FFmpeg, in software: a GPU's
    // decoder is another machine's arithmetic
    v.nvdec_emulation.SetValue(Settings::NvdecEmulation::Cpu);
    if (options.docked) {
        SCREEN_W = 1920;
        SCREEN_H = 1080;
    }
    // Above 1x the renderer draws what it can at the larger size, and the
    // screen is that much larger (1.5x of the hand's 1280x720 is 1920x1080).
    struct Scale {
        const char *name;
        Settings::ResolutionSetup setup;
        int num, den;
    };
    static constexpr Scale SCALES[] = { { "1x", Settings::ResolutionSetup::Res1X, 1, 1 },
        { "1.5x", Settings::ResolutionSetup::Res3_2X, 3, 2 }, { "2x", Settings::ResolutionSetup::Res2X, 2, 1 },
        { "3x", Settings::ResolutionSetup::Res3X, 3, 1 } };
    const Scale *scale = nullptr;
    for (const Scale &s : SCALES)
        if (options.resolution == s.name)
            scale = &s;
    if (!scale) {
        error = std::string("error.bad_resolution" TEXT_ARG) + options.resolution;
        return false;
    }
    v.resolution_setup.SetValue(scale->setup);
    SCREEN_W = SCREEN_W * scale->num / scale->den;
    SCREEN_H = SCREEN_H * scale->num / scale->den;
    g_screen.allocate();
    g_screen.opengl = options.renderer != "null";
    v.renderer_backend.SetValue(g_screen.opengl ? Settings::RendererBackend::OpenGL_GLSL : Settings::RendererBackend::Null);
    // the screen is the console's own, 1280x720, whatever the game would do
    // on a television
    v.use_docked_mode.SetValue(options.docked ? Settings::ConsoleMode::Docked : Settings::ConsoleMode::Handheld);
    if (g_screen.opengl && !load_gl(error))
        return false;
    // Above 1x the images the renderer draws at the larger size are copied
    // into the core's memory before every state (kept_images.cpp): a block a
    // state carries, and a scratch one it does not. NOW and not on first use:
    // whether a page is mapped at all is part of every state. Address space,
    // not memory - a page is committed when it is first written.
    if (g_screen.opengl && scale->num != scale->den) {
        static constexpr size_t BLOCK = size_t{2} << 30;
        void *block = mmap(nullptr, BLOCK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        void *scratch = alloc_unkept(kept_images::scratch_bytes());
        if (block == MAP_FAILED || !scratch) {
            error = "error.no_kept_memory";
            return false;
        }
        kept_images::set_memory(block, BLOCK, scratch, kept_images::scratch_bytes());
    }
    v.rng_seed_enabled.SetValue(true);
    v.rng_seed.SetValue(0);

    g_machine = std::make_unique<Machine>();
    Core::System &system = g_machine->system;
    system.Initialize();
    system.ApplySettings();
    system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
    system.GetFileSystemController().CreateFactories(*system.GetFilesystem());
    system.GetUserChannel().clear();
    // Updates and DLC, as Eden's frontend registers the ones its game list
    // finds: the patch manager takes the newest update of the game, and the
    // DLC service lists every add-on. A file that holds neither is refused,
    // not ignored: a base game in that slot would otherwise do nothing.
    for (size_t i = 0; i < content.size(); i++) {
        const auto file = system.GetFilesystem()->OpenFile(content[i], FileSys::OpenMode::Read);
        if (!file || !g_machine->content.AddEntriesFromContainer(file, true)) {
            error = std::string("error.not_content" TEXT_ARG) + base_name(options.content[i]);
            return false;
        }
    }
    system.RegisterContentProvider(FileSys::ContentProviderUnionSlot::FrontendManual, &g_machine->content);

    Service::AM::FrontendAppletParameters params{
        .applet_id = Service::AM::AppletId::Application,
    };
    const Core::SystemResultStatus rc = system.Load(g_machine->window, rom, params);
    if (rc != Core::SystemResultStatus::Success) {
        // said so a person can act on it
        const u32 code = static_cast<u32>(rc);
        if (rc == Core::SystemResultStatus::ErrorVideoCore) {
            if (g_screen.opengl && g_screen.bind)
                g_screen.bind();
            const char *version = g_screen.opengl ? reinterpret_cast<const char *>(stable_get_string(GL_VERSION)) : nullptr;
            if (g_screen.opengl && g_screen.release)
                g_screen.release();
            error = version ? std::string("error.gl_too_old" TEXT_ARG) + version : std::string("error.gl_no_version");
        } else if (rc == Core::SystemResultStatus::ErrorGetLoader) {
            error = "error.not_a_game";
        } else if (code >= static_cast<u32>(Core::SystemResultStatus::ErrorLoader)) {
            // Eden's own reason, in its words
            error = std::string("error.game_not_loaded" TEXT_ARG)
                + Loader::GetResultStatusString(static_cast<Loader::ResultStatus>(
                    code - static_cast<u32>(Core::SystemResultStatus::ErrorLoader)));
        } else {
            error = std::string("error.game_not_loaded_code" TEXT_ARG) + std::to_string(code);
        }
        return false;
    }
    if (!options.savedata.empty() && !install_savedata(system, options.savedata, error))
        return false;
    system.RegisterExitCallback([] {
        if (g_machine && !g_machine->exited)
            g_machine->exited = g_machine->frame;
    });

    Machine &m = *g_machine;
    m.rebuild_every_frame = options.rebuild_every_frame;
    m.keep_before_rebuild = getenv("CHIMERA_EDEN_NO_KEEP") == nullptr;
    m.quiesce = options.write_back && getenv("CHIMERA_EDEN_NO_QUIESCE") == nullptr;
    m.frame_event = Core::Timing::CreateEvent("ChimeraFrame",
        [&m](s64, std::chrono::nanoseconds) -> std::optional<std::chrono::nanoseconds> {
            if (m.frame != 0) {
                m.ended_ns = static_cast<uint64_t>(m.system.CoreTiming().GetGlobalTimeNs().count());
                // Between frames nothing of the machine's may be on the
                // host's GPU alone: a savestate is its memory and no more.
                if (g_screen.opengl && m.quiesce)
                    m.system.GPU().ChimeraQuiesce();
                m.done.release();
            }
            m.go.acquire();
            // A state was loaded while the machine stood here: the calls now
            // land on another GL context, where every name the renderer
            // remembers is nothing or something else. It is made again, and
            // fills its caches from the machine's memory.
            if (g_screen.opengl && (chimera_gl_context_id() != g_screen.context_id || m.rebuild_every_frame)) {
                // the gate's rebuild before every frame stands for a state
                // taken and loaded there: the core is told first, as the
                // engine tells it before every state
                if (m.rebuild_every_frame && m.keep_before_rebuild)
                    VideoCore::ChimeraKeepScaled(m.system);
                kept_images::renderer_made_again();
                VideoCore::ChimeraRebuildRenderer(m.system, m.window, rebuild_own_gl);
            }
            m.frame++;
            m.system.CoreTiming().ScheduleEvent(std::chrono::nanoseconds(frame_end_ns(m.frame)), m.frame_event, true);
            return std::nullopt;
        });
    system.CoreTiming().ScheduleEvent(std::chrono::nanoseconds(0), m.frame_event, true);
    // The machine is let go (Run) BEFORE its CPU thread is (OnGpuReady).
    // Upstream does it the other way round, and between the two the CPU
    // thread runs the kernel's idle loop, which moves the machine's clock at
    // the host's pace: natively a few microseconds, in the sandbox a tenth
    // of a second of machine time, and the first frames were as many as fit
    // in it. This way the CPU thread's first act is the event above.
    void(system.Run());
    system.GPU().Start();
    system.GetCpuManager().OnGpuReady();

    return true;
}

void frame() {
    Machine &m = *g_machine;
    m.go.release();
    m.done.acquire();
    // The gate's: what Eden's emulated console holds of the touch screen
    // after the frame, to compare with what the script asked for.
    static const bool touch_trace = getenv("CHIMERA_EDEN_TOUCH_TRACE") != nullptr;
    if (touch_trace) {
        const auto fingers = m.system.HIDCore().GetEmulatedConsole()->GetTouch();
        fprintf(stderr, "touch frame=%llu pressed=%d x=%.5f y=%.5f\n", static_cast<unsigned long long>(m.frame),
                fingers[0].pressed ? 1 : 0, fingers[0].position.x, fingers[0].position.y);
    }
    m.audio = static_cast<AudioCore::Sink::ChimeraSink &>(m.system.AudioCore().GetOutputSink()).Take();
}

const int16_t *audio(int &pairs) {
    pairs = g_machine ? static_cast<int>(g_machine->audio.size() / 2) : 0;
    return g_machine ? g_machine->audio.data() : nullptr;
}

// Player `player`'s pad for the coming frames: `buttons` by NativeButton's
// order (A, B, X, Y, LStick, RStick, L, R, ZL, ZR, Plus, Minus, DLeft, DUp,
// DRight, DDown, ...), the sticks as -32768..32767 (right and up positive).
void set_pad(int player, uint64_t buttons, const int32_t axes[4]) {
    if (player < 0 || player >= PLAYERS)
        return;
    for (int b = 0; b < Settings::NativeButton::NumButtons; b++) {
        const bool now = (buttons >> b) & 1, was = (g_pad_buttons[player] >> b) & 1;
        if (now == was)
            continue;
        Common::Input::CallbackStatus status{};
        status.type = Common::Input::InputType::Button;
        status.button_status.value = now;
        for (PadControl *control : g_buttons[player][b])
            control->TriggerOnChange(status);
    }
    g_pad_buttons[player] = buttons;
    for (int a = 0; a < 2; a++) {
        if (axes[a * 2] == g_pad_axes[player][a * 2] && axes[a * 2 + 1] == g_pad_axes[player][a * 2 + 1])
            continue;
        Common::Input::CallbackStatus status{};
        status.type = Common::Input::InputType::Stick;
        status.stick_status.x.raw_value = std::clamp(axes[a * 2] / 32767.0f, -1.0f, 1.0f);
        status.stick_status.y.raw_value = std::clamp(axes[a * 2 + 1] / 32767.0f, -1.0f, 1.0f);
        for (PadControl *control : g_sticks[player][a])
            control->TriggerOnChange(status);
        g_pad_axes[player][a * 2] = axes[a * 2];
        g_pad_axes[player][a * 2 + 1] = axes[a * 2 + 1];
    }
}

// The touch screen from now on: one finger, down or not, at a place given as
// 0..65535 across and down the screen.
void set_touch(bool down, int32_t x, int32_t y) {
    static bool was_down = false;
    static int32_t was_x = -1, was_y = -1;
    if (down == was_down && (!down || (x == was_x && y == was_y)))
        return;
    Common::Input::CallbackStatus status{};
    status.type = Common::Input::InputType::Touch;
    status.touch_status.pressed.value = down;
    // (65536 steps across: 65535 is the last pixel, never one past it)
    status.touch_status.x.raw_value = std::clamp(x / 65536.0f, 0.0f, 1.0f);
    status.touch_status.y.raw_value = std::clamp(y / 65536.0f, 0.0f, 1.0f);
    status.touch_status.id = 0;
    for (PadControl *control : g_touch)
        control->TriggerOnChange(status);
    was_down = down;
    was_x = x;
    was_y = y;
}

void set_gl_thread_hooks(void (*bind)(), void (*release)()) {
    g_screen.bind = bind;
    g_screen.release = release;
}

const uint8_t *video(int &width, int &height) {
    width = SCREEN_W;
    height = SCREEN_H;
    if (!g_screen.latest)
        g_screen.allocate();
    return g_screen.latest;
}

void set_rendering(bool on) {
    g_screen.readback = on;
}

void state_saving() {
    if (!g_machine || !g_screen.opengl || !kept_images::active())
        return;
    if (g_screen.bind)
        g_screen.bind();
    VideoCore::ChimeraKeepScaled(g_machine->system);
    if (g_screen.release)
        g_screen.release();
}

uint64_t exited_at() {
    return g_machine ? g_machine->exited : 0;
}

void log(std::vector<uint8_t> &out) {
    Common::Log::Stop();
    out.clear();
    chimera::memfs::get(std::string(TREE) + "/user/log/eden_log.txt", out);
}

uint64_t frame_count() {
    return g_machine ? g_machine->frame : 0;
}

uint64_t frame_ended_ns() {
    return g_machine ? g_machine->ended_ns : 0;
}

uint64_t cpu_ticks() {
    return g_machine ? g_machine->system.CoreTiming().GetClockTicks() : 0;
}

// FNV-1a over every page the application has mapped and may read, with each
// block's address and size: the machine's memory as the game sees it.
uint64_t ram_digest() {
    if (!g_machine)
        return 0;
    Core::System &system = g_machine->system;
    Kernel::KProcess *process = system.ApplicationProcess();
    if (!process)
        return 0;
    auto &table = process->GetPageTable();
    Core::Memory::Memory &memory = process->GetMemory();
    uint64_t h = 0xcbf29ce484222325ull;
    const auto mix = [&h](const uint8_t *p, size_t n) {
        for (size_t i = 0; i < n; i++)
            h = (h ^ p[i]) * 0x100000001b3ull;
    };
    std::vector<uint8_t> buf(1 << 20);
    FILE *dump = getenv("CHIMERA_EDEN_RAM_DUMP") ? fopen(getenv("CHIMERA_EDEN_RAM_DUMP"), "a") : nullptr;
    uint64_t addr = GetInteger(table.GetAddressSpaceStart());
    const uint64_t end = addr + table.GetAddressSpaceSize();
    while (addr < end) {
        Kernel::KMemoryInfo info;
        Kernel::Svc::PageInfo page;
        if (table.QueryInfo(&info, &page, addr).IsError())
            break;
        const auto svc = info.GetSvcMemoryInfo();
        const uint64_t base = svc.base_address, size = svc.size;
        if (size == 0)
            break;
        if (svc.state != Kernel::Svc::MemoryState::Free && svc.state != Kernel::Svc::MemoryState::Inaccessible
            && True(svc.permission & Kernel::Svc::MemoryPermission::Read)) {
            const uint64_t before = h;
            mix(reinterpret_cast<const uint8_t *>(&base), sizeof base);
            mix(reinterpret_cast<const uint8_t *>(&size), sizeof size);
            for (uint64_t off = 0; off < size; off += buf.size()) {
                const size_t n = static_cast<size_t>(std::min<uint64_t>(buf.size(), size - off));
                if (!memory.ReadBlockUnsafe(base + off, buf.data(), n))
                    std::fill(buf.begin(), buf.begin() + n, 0);
                if (dump) {
                    // page digests, for finding where two runs part
                    for (size_t pg = 0; pg < n; pg += 4096) {
                        uint64_t ph = 0xcbf29ce484222325ull;
                        for (size_t i = pg; i < pg + 4096 && i < n; i++)
                            ph = (ph ^ buf[i]) * 0x100000001b3ull;
                        fprintf(dump, "%llx %016llx\n", (unsigned long long)(base + off + pg), (unsigned long long)ph);
                    }
                }
                mix(buf.data(), n);
            }
            if (dump)
                fprintf(dump, "block %llx %llx state=%u perm=%u %016llx\n", (unsigned long long)base, (unsigned long long)size,
                    (unsigned)svc.state, (unsigned)svc.permission, (unsigned long long)(h ^ before));
        }
        if (base + size <= addr)
            break;
        addr = base + size;
    }
    if (dump) {
        fclose(dump);
        // CHIMERA_EDEN_PAGES=addr,addr: those pages' bytes beside the digests
        if (const char *pages = getenv("CHIMERA_EDEN_PAGES")) {
            const std::string out = std::string(getenv("CHIMERA_EDEN_RAM_DUMP")) + ".pages";
            if (FILE *f = fopen(out.c_str(), "ab")) {
                for (const char *p = pages; *p;) {
                    char *e = nullptr;
                    const uint64_t a = strtoull(p, &e, 16);
                    uint8_t page[4096] = {};
                    memory.ReadBlockUnsafe(a, page, sizeof page);
                    fwrite(page, 1, sizeof page, f);
                    p = *e == ',' ? e + 1 : e;
                }
                fclose(f);
            }
        }
    }
    return h;
}

} // namespace chimera_eden

// For a debugger: where the application's page at `guest` lives in this
// process (gdb: watch -l *(long *)chimera_eden_host_pointer(0x...)).
extern "C" void *chimera_eden_host_pointer(uint64_t guest) {
    if (!chimera_eden::g_machine)
        return nullptr;
    Kernel::KProcess *process = chimera_eden::g_machine->system.ApplicationProcess();
    return process ? process->GetMemory().GetPointer(guest) : nullptr;
}

namespace chimera_eden {

uint64_t frames() {
    return g_machine ? g_machine->window.presented.load() : 0;
}

uint64_t time_ns() {
    return g_machine ? static_cast<uint64_t>(g_machine->system.CoreTiming().GetGlobalTimeNs().count()) : 0;
}

// Every thread of the game: its state, why it waits, where it stands.
void dump_threads(FILE *out) {
    if (!g_machine)
        return;
    Kernel::KProcess *process = g_machine->system.ApplicationProcess();
    if (!process)
        return;
    static const char *const reasons[] = { "running", "sleep", "ipc", "sync", "condvar", "arbitration", "suspended" };
    for (auto &thread : process->GetThreadList()) {
        const auto &ctx = thread.GetContext();
        const unsigned reason = static_cast<unsigned>(thread.GetWaitReasonForDebugging());
        fprintf(out, "thread %llu prio=%d core=%d state=%u wait=%s pc=%llx lr=%llx sp=%llx\n",
            (unsigned long long)thread.GetThreadId(), thread.GetPriority(), thread.GetActiveCore(),
            static_cast<unsigned>(thread.GetRawState()), reason < 7 ? reasons[reason] : "?",
            (unsigned long long)ctx.pc, (unsigned long long)ctx.lr, (unsigned long long)ctx.sp);
    }
}

void flush_log() {
    Common::Log::Stop();
}

// The export: the save area's files, the machine's user taken out of the
// names (another project's machine has the same one, but a zip a person
// keeps should not say so).
namespace {
std::vector<std::pair<std::string, std::vector<uint8_t>>> g_savedata;
}
size_t savedata_snapshot() {
    g_savedata.clear();
    if (!g_machine)
        return 0;
    const std::string user = user_folder(g_machine->system);
    for (const std::string &rel : chimera::memfs::list(SAVE_AREA)) {
        const size_t slash = rel.find('/');
        if (slash == std::string::npos)
            continue;
        const std::string owner = rel.substr(0, slash), rest = rel.substr(slash + 1);
        std::string name;
        if (owner == user)
            name = rest;
        else if (owner == NO_USER)
            name = "device/" + rest;
        else
            continue;
        std::vector<uint8_t> bytes;
        if (chimera::memfs::get(std::string(SAVE_AREA) + "/" + rel, bytes))
            g_savedata.emplace_back(std::move(name), std::move(bytes));
    }
    return g_savedata.size();
}
const std::string &savedata_name(size_t i) {
    static const std::string none;
    return i < g_savedata.size() ? g_savedata[i].first : none;
}
const std::vector<uint8_t> &savedata_bytes(size_t i) {
    static const std::vector<uint8_t> none;
    return i < g_savedata.size() ? g_savedata[i].second : none;
}

void shutdown() {
    if (!g_machine)
        return;
    void(g_machine->system.Pause());
    g_machine->system.ShutdownMainProcess();
    g_machine.reset();
}

} // namespace chimera_eden
