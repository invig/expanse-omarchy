// Symbols CCanvas::onKey and CCanvas::handleCanvasKey call that live in other
// translation units. live() and showsCanvas() are the real predicates
// (src/Canvas.cpp). The rest are only reached for chords the canvas handles
// itself (plain navigation, Super+A, Super+W); the leak cases never call them.
#include "Canvas.hpp"

namespace Expanse {
    bool CCanvas::live() const {
        // src/Canvas.cpp, CCanvas::live
        return m_state != STATE_IDLE;
    }

    bool CCanvas::showsCanvas() const {
        // src/Canvas.cpp, CCanvas::showsCanvas
        return m_state == STATE_OPENING || m_state == STATE_OVERVIEW;
    }

    void CCanvas::selectAll() {}

    std::vector<PHLWINDOW> CCanvas::selection() const {
        return {};
    }

    void CCanvas::clearSelection() {}
    void CCanvas::close() {}
    void CCanvas::glideTo(PHLWINDOW, bool) {}
    void CCanvas::hop(eDirection) {}
    void CCanvas::zoomTo(double) {}
    void CCanvas::fit() {}
    void CCanvas::tidy() {}
    void CCanvas::closeWindows(const std::vector<PHLWINDOW>&) {}

    std::vector<PHLWINDOW> CCanvas::canvasWindows() const {
        return {};
    }

    bool CCanvas::isCanvasWindow(PHLWINDOW) const {
        return false;
    }
}
