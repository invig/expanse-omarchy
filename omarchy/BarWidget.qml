import QtQuick
import qs.Commons
import qs.Ui

// Expanse in Omarchy's bar, as its menu bar icon on the Mac: a click opens its
// menu (in Omarchy's own), a right-click zooms out to every window.
BarWidget {
  id: root
  moduleName: "io.github.invig.expanse"

  readonly property string script: decodeURIComponent(String(Qt.resolvedUrl("expanse")).replace(/^file:\/\//, ""))

  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  BarIconButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    text: ""
    tooltipText: "Expanse"
    onPressed: function(b) {
      if (b === Qt.RightButton) Util.execArgv(["hyprctl", "expanse", "toggle"])
      else Util.execArgv([root.script, "menu"])
    }
  }
}
