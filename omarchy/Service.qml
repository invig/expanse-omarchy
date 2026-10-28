import QtQuick
import Quickshell.Io

// As the shell starts (at login): Expanse's Hyprland plugin loaded, rebuilt
// first if Hyprland or Expanse changed since it was built. Until it has been
// set up, from the bar icon's menu, this does nothing.
Item {
  id: root

  property var settings: ({})

  Process {
    id: load
    command: [decodeURIComponent(String(Qt.resolvedUrl("expanse")).replace(/^file:\/\//, "")), "load"]
    running: false
  }

  Component.onCompleted: load.running = true
}
