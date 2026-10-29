#define WLR_USE_UNSTABLE

#include "Mode.hpp"
#include "Canvas.hpp"
#include "Later.hpp"
#include "globals.hpp"

#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/algorithm/TiledAlgorithm.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/target/Target.hpp>

#include <utility>
#include <vector>

namespace Expanse::Mode {

    static constexpr const char* NAME = "expanse";

    using Desktop::View::IGeometric;

    // Where each window was as a workspace took this layout. Hyprland makes
    // the layout first, then hands it the windows one at a time, tiling the
    // ones still to come again in the old layout as each leaves it.
    static std::vector<std::pair<PHLWINDOWREF, CBox>> g_before;

    static void noteWhereWindowsAre() {
        if (g_before.empty())
            later([] { g_before.clear(); });
        g_before.clear();
        for (const auto& window : Desktop::windowState()->windows()) {
            if (window->m_isMapped)
                g_before.emplace_back(window, CBox{window->position(IGeometric::GEOMETRIC_GOAL), window->size(IGeometric::GEOMETRIC_GOAL)});
        }
    }

    static std::optional<CBox> whereItWas(const PHLWINDOW& window) {
        for (const auto& [ref, box] : g_before) {
            if (ref.lock() == window)
                return box;
        }
        return std::nullopt;
    }

    // Hyprland hands a workspace's tiled windows to its layout. This one tiles
    // nothing: each window floats where it is (the canvas places a new one
    // beside the window it came from), so the canvas has them all.
    class CExpanseAlgorithm : public ::Layout::ITiledAlgorithm {
      public:
        CExpanseAlgorithm() = default;

        ~CExpanseAlgorithm() override {
            // The workspace took another layout (or went): what arrived tiled
            // tiles again, unless it's still an Expanse one (a config reload
            // makes the layout anew).
            if (const auto WORKSPACE = m_workspace.lock())
                later([ref = PHLWORKSPACEREF{WORKSPACE}] {
                    if (const auto W = ref.lock(); W && !isExpanse(W))
                        g_canvas.leaveWorkspace(W);
                });
        }

        void newTarget(SP<::Layout::ITarget> target) override {
            hold(target);
        }

        void movedTarget(SP<::Layout::ITarget> target, std::optional<Vector2D>) override {
            hold(target);
        }

        void removeTarget(SP<::Layout::ITarget>) override {}

        void resizeTarget(const Vector2D&, SP<::Layout::ITarget>, ::Layout::eRectCorner) override {}

        void recalculate(::Layout::eRecalculateReason) override {
            noteWorkspace();
        }

        void swapTargets(SP<::Layout::ITarget>, SP<::Layout::ITarget>) override {}

        void moveTargetInDirection(SP<::Layout::ITarget>, Math::eDirection, bool) override {}

        SP<::Layout::ITarget> getNextCandidate(SP<::Layout::ITarget>) override {
            return nullptr;
        }

        std::optional<std::string> layoutName() const override {
            return NAME;
        }

      private:
        PHLWORKSPACEREF m_workspace;

        void noteWorkspace() {
            if (const auto PARENT = m_parent.lock(); PARENT && PARENT->space())
                m_workspace = PARENT->space()->workspace();
        }

        void hold(const SP<::Layout::ITarget>& target) {
            noteWorkspace();
            // A window already on screen (tiled until the workspace took this
            // layout) floats where it was: Hyprland hands it over with the
            // whole work area as its box.
            const auto WINDOW = target->window();
            CBox       box    = target->position();
            if (const auto BEFORE = WINDOW ? whereItWas(WINDOW) : std::nullopt)
                box = *BEFORE;
            // Back in Expanse after another layout, at its spot on the canvas then.
            if (const auto SPOT = WINDOW ? g_canvas.spotBeforeLeaving(WINDOW) : std::nullopt)
                box = *SPOT;
            // A new window has no box yet: until it floats, the size it asks
            // for, in the middle.
            if (box.w < 2 || box.h < 2) {
                const auto SPACE = target->space();
                const auto WANT  = target->desiredGeometry();
                if (SPACE && WANT) {
                    const CBox     AREA = SPACE->workArea();
                    const Vector2D SIZE = WANT->size;
                    box                 = {WANT->pos.value_or(AREA.middle() - SIZE / 2.0), SIZE};
                }
            }
            if (box.w >= 2 && box.h >= 2)
                target->setPositionGlobal(box);
            // Floated once Hyprland has finished handing it over.
            later([ref = WP<::Layout::ITarget>{target}, box] {
                if (const auto TARGET = ref.lock())
                    g_canvas.adoptTiled(TARGET, box);
            });
        }
    };

    bool add(HANDLE handle) {
        return HyprlandAPI::addTiledAlgo(handle, NAME, &typeid(CExpanseAlgorithm), [] {
            noteWhereWindowsAre();
            return UP<::Layout::ITiledAlgorithm>(makeUnique<CExpanseAlgorithm>());
        });
    }

    void remove(HANDLE handle) {
        HyprlandAPI::removeAlgo(handle, NAME);
    }

    std::string layoutName(const PHLWORKSPACE& workspace) {
        if (!workspace || !workspace->m_space)
            return "?";
        const auto ALGORITHM = workspace->m_space->algorithm();
        return ALGORITHM && ALGORITHM->tiledAlgo() ? ALGORITHM->tiledAlgo()->layoutName().value_or("?") : "?";
    }

    bool isExpanse(const PHLWORKSPACE& workspace) {
        if (!workspace || workspace->m_isSpecialWorkspace || !workspace->m_space)
            return false;
        const auto ALGORITHM = workspace->m_space->algorithm();
        return ALGORITHM && ALGORITHM->tiledAlgo() && dynamic_cast<const CExpanseAlgorithm*>(ALGORITHM->tiledAlgo().get());
    }
}
