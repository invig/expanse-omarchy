#define WLR_USE_UNSTABLE

#include "Toast.hpp"
#include "Perf.hpp"
#include "globals.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>

#include <wayland-server-core.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace Expanse::Toast {

    using Clock = std::chrono::steady_clock;

    constexpr double SHOWN_SECONDS = 1.2;
    constexpr double FADE_SECONDS  = 0.3;
    constexpr double FONT_PT       = 15.0;
    constexpr double PAD_X = 20.0, PAD_Y = 11.0;
    constexpr double ABOVE_BOTTOM  = 80.0; // of the usable area

    static struct {
        std::string            text;
        PHLMONITORREF          monitor;
        Clock::time_point      shownAt;
        SP<::Render::ITexture> tex;
        float                  texScale = 0; // the monitor scale it was made for
        CBox                   lastBox;      // logical, as last drawn
        wl_event_source*       timer = nullptr;
    } g_toast;

    static double age() {
        return std::chrono::duration<double>(Clock::now() - g_toast.shownAt).count();
    }

    static bool alive() {
        return !g_toast.text.empty() && age() < SHOWN_SECONDS + FADE_SECONDS;
    }

    static CBox box(const PHLMONITOR& monitor) {
        // The pill's logical box, from the text's size on this monitor.
        const double   S    = monitor->m_scale;
        const Vector2D TEXT = g_toast.tex ? g_toast.tex->m_size / S : Vector2D{200, 18};
        const Vector2D SIZE = {TEXT.x + 2 * PAD_X, TEXT.y + 2 * PAD_Y};
        const CBox     AREA = monitor->logicalBoxMinusReserved();
        return {std::round(AREA.x + (AREA.w - SIZE.x) / 2), std::round(AREA.y + AREA.h - ABOVE_BOTTOM - SIZE.y), SIZE.x, SIZE.y};
    }

    // A texture made mid-frame waits for the GPU, so the text is made as the
    // toast is shown; only a change of scale makes it again while drawing.
    static bool makeText(const PHLMONITOR& monitor) {
        if (g_toast.tex && g_toast.texScale == monitor->m_scale)
            return true;
        Perf::CTextTime timed;
        g_toast.tex      = g_pHyprRenderer->renderText(g_toast.text, CHyprColor{1.F, 1.F, 1.F, 1.F}, std::round(FONT_PT * monitor->m_scale), false, "", 0, 500);
        g_toast.texScale = monitor->m_scale;
        return g_toast.tex != nullptr;
    }

    static void damageToast() {
        if (!g_toast.lastBox.empty())
            g_pHyprRenderer->damageBox(g_toast.lastBox.copy().expand(2));
        if (const auto MONITOR = g_toast.monitor.lock())
            g_pHyprRenderer->damageBox(box(MONITOR).expand(2));
    }

    void show(const std::string& text) {
        const auto MONITOR = Desktop::focusState()->monitor();
        if (!MONITOR || !g_pCompositor || !g_pCompositor->m_wlEventLoop)
            return;
        damageToast(); // the one it replaces
        g_toast.text    = text;
        g_toast.monitor = MONITOR;
        g_toast.shownAt = Clock::now();
        g_toast.tex.reset();
        if (::Render::GL::g_pHyprOpenGL) {
            ::Render::GL::g_pHyprOpenGL->makeEGLCurrent();
            makeText(MONITOR);
        }
        damageToast();
        // Frames come by themselves while it fades (fading()); this starts it.
        if (!g_toast.timer)
            g_toast.timer = wl_event_loop_add_timer(
                g_pCompositor->m_wlEventLoop,
                [](void*) {
                    damageToast();
                    return 0;
                },
                nullptr);
        wl_event_source_timer_update(g_toast.timer, sc<int>(SHOWN_SECONDS * 1000));
    }

    void afterFrame(const PHLMONITOR& monitor) {
        if (g_toast.text.empty() || monitor != g_toast.monitor.lock())
            return;
        if (alive()) {
            if (age() >= SHOWN_SECONDS)
                damageToast();
            return;
        }
        // Gone: one last frame clears it.
        damageToast();
        g_toast.text.clear();
        g_toast.tex.reset();
        g_toast.lastBox = {};
    }

    void draw(const PHLMONITOR& monitor) {
        if (!alive() || monitor != g_toast.monitor.lock())
            return;
        const double S = monitor->m_scale;
        if (!makeText(monitor))
            return;
        const float ALPHA = std::clamp(1.F - sc<float>((age() - SHOWN_SECONDS) / FADE_SECONDS), 0.F, 1.F);
        const CBox  BOX   = box(monitor);
        if (BOX != g_toast.lastBox) {
            damageToast();
            g_toast.lastBox = BOX;
        }
        const CBox PIXELS = BOX.copy().translate(-monitor->m_position).scale(S).round();

        CRectPassElement::SRectData pill;
        pill.box   = PIXELS;
        pill.color = CHyprColor{0.1F, 0.1F, 0.1F, 0.85F * ALPHA};
        pill.round = std::round(PIXELS.h / 2);
        g_pHyprRenderer->addPassElement(makeUnique<CRectPassElement>(pill));

        CTexPassElement::SRenderData text;
        text.tex = g_toast.tex;
        text.box = CBox{(PIXELS.middle() - g_toast.tex->m_size / 2.0).round(), g_toast.tex->m_size};
        text.a   = ALPHA;
        g_pHyprRenderer->addPassElement(makeUnique<CTexPassElement>(std::move(text)));
    }

    void shutdown() {
        if (g_toast.timer)
            wl_event_source_remove(g_toast.timer);
        g_toast = {};
    }
}
