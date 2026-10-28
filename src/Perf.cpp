#define WLR_USE_UNSTABLE

#include "Perf.hpp"

#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/state/MonitorState.hpp>

#include <GLES3/gl32.h>

#include <algorithm>
#include <format>
#include <map>
#include <vector>

namespace Expanse::Perf {

    using Clock = std::chrono::steady_clock;

    struct SFrame {
        double start    = 0; // ms since recording began
        double cpu      = 0; // render.pre to the pass drawn, ms
        double plugin   = 0; // of which the plugin's own, ms
        double gpu      = 0; // then waiting for the GPU to finish it, ms (only with "gpu")
        int    texts    = 0; // text textures made
        double text     = 0; // making them, ms (part of the plugin's)
    };

    struct SMonitor {
        float               refresh = 60;
        std::vector<SFrame> frames;
        std::vector<double> presented; // ms since recording began
        CHyprSignalListener listener;
    };

    static bool                            g_recording = false;
    static bool                            g_gpu       = false;
    static Clock::time_point               g_began;
    static std::map<std::string, SMonitor> g_monitors;
    static SMonitor*                       g_current = nullptr;
    static SFrame                          g_frame;
    static Clock::time_point               g_frameStart;
    static double                          g_pending = 0; // plugin time before the frame starts

    static double since(Clock::time_point from, Clock::time_point to = Clock::now()) {
        return std::chrono::duration<double, std::milli>(to - from).count();
    }

    bool recording() {
        return g_recording;
    }

    void start(bool gpu) {
        g_gpu = gpu;
        g_monitors.clear();
        g_current   = nullptr;
        g_pending   = 0;
        g_began     = Clock::now();
        g_recording = true;
        for (const auto& monitor : State::monitorState()->monitors()) {
            if (!monitor || !monitor->m_enabled)
                continue;
            auto& entry    = g_monitors[monitor->m_name];
            entry.refresh  = monitor->m_refreshRate;
            entry.listener = monitor->m_events.presented.listen([name = monitor->m_name](auto when) {
                if (const auto IT = g_monitors.find(name); g_recording && IT != g_monitors.end())
                    IT->second.presented.push_back(since(g_began, when));
            });
        }
    }

    void frameStart(const PHLMONITOR& monitor) {
        if (!g_recording || !monitor)
            return;
        const auto IT = g_monitors.find(monitor->m_name);
        // A frame Hyprland skipped (nothing damaged) never finishes: this one replaces it.
        g_current    = IT == g_monitors.end() ? nullptr : &IT->second;
        g_frameStart = Clock::now();
        g_frame      = {.start = since(g_began, g_frameStart), .plugin = g_pending};
        g_pending    = 0;
    }

    void frameEnd() {
        if (!g_recording || !g_current)
            return;
        g_frame.cpu = since(g_frameStart);
        if (g_gpu && Render::GL::g_pHyprOpenGL) {
            // The frame is on its way to the GPU; waiting for it shows what it
            // cost there (and slows everything down while measuring).
            const auto WAIT = Clock::now();
            Render::GL::g_pHyprOpenGL->makeEGLCurrent();
            glFinish();
            g_frame.gpu = since(WAIT);
        }
        g_current->frames.push_back(g_frame);
        g_current = nullptr;
    }

    CTextTime::CTextTime() : m_start(Clock::now()) {}

    CTextTime::~CTextTime() {
        if (!g_recording || !g_current)
            return;
        ++g_frame.texts;
        g_frame.text += since(m_start);
    }

    CPluginTime::CPluginTime() : m_start(Clock::now()) {}

    CPluginTime::~CPluginTime() {
        if (!g_recording)
            return;
        (g_current ? g_frame.plugin : g_pending) += since(m_start);
    }

    static std::string numbers(const std::vector<double>& values) {
        std::string out;
        for (const double value : values)
            out += std::format("{}{:.2f}", out.empty() ? "" : ",", value);
        return out;
    }

    std::string stop() {
        g_recording = false;
        std::string out;
        for (auto& [name, monitor] : g_monitors) {
            monitor.listener.reset();
            std::vector<double> start, cpu, plugin, gpu, texts, text;
            for (const auto& frame : monitor.frames) {
                start.push_back(frame.start);
                cpu.push_back(frame.cpu);
                plugin.push_back(frame.plugin);
                gpu.push_back(frame.gpu);
                texts.push_back(frame.texts);
                text.push_back(frame.text);
            }
            out += std::format(R"({}"{}":{{"refresh":{:.2f},"start":[{}],"cpu":[{}],"plugin":[{}],"gpu":[{}],"texts":[{}],"text":[{}],"presented":[{}]}})",
                               out.empty() ? "" : ",", name, monitor.refresh, numbers(start), numbers(cpu), numbers(plugin), numbers(gpu), numbers(texts), numbers(text),
                               numbers(monitor.presented));
        }
        g_monitors.clear();
        return "{" + out + "}\n";
    }
}
