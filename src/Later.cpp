#define WLR_USE_UNSTABLE

#include "Later.hpp"

#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>

#include <algorithm>
#include <memory>
#include <vector>

namespace Expanse {

    static std::vector<uint64_t> g_pending;

    void later(std::function<void()> fn) {
        auto seq = std::make_shared<uint64_t>(0);
        *seq     = g_pEventLoopManager->doLater([fn = std::move(fn), seq] {
            std::erase(g_pending, *seq);
            fn();
        });
        g_pending.push_back(*seq);
    }

    void cancelLater() {
        for (const auto SEQ : g_pending)
            g_pEventLoopManager->removeDoLater(SEQ);
        g_pending.clear();
    }
}
