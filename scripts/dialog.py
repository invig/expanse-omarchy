#!/usr/bin/python3
"""GTK 4 windows for scripts/sweep.py.

    scripts/dialog.py APP_ID           a window and, a moment later, a modal dialog over it
    scripts/dialog.py APP_ID popup     a window with a bright green popover (a popup) open
"""

import sys

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
from gi.repository import Gdk, GLib, Gtk  # noqa: E402

POPUP = len(sys.argv) > 2 and sys.argv[2] == "popup"


def activate(app):
    parent = Gtk.ApplicationWindow(application=app, title="dialog-parent")
    parent.set_default_size(700, 450)

    if POPUP:
        css = Gtk.CssProvider()
        css.load_from_data(b"label.green { background: #00ff00; color: #00ff00; padding: 60px 120px; }")
        Gtk.StyleContext.add_provider_for_display(Gdk.Display.get_default(), css, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
        button = Gtk.Button(label="menu")
        parent.set_child(button)
        popover = Gtk.Popover()
        label = Gtk.Label(label="popup")
        label.add_css_class("green")
        popover.set_child(label)
        popover.set_parent(button)
        popover.set_autohide(False)
        parent.present()
        GLib.timeout_add(1500, lambda: popover.popup() and False)
        return

    parent.present()

    def open_dialog():
        dialog = Gtk.Window(title="dialog-child", transient_for=parent, modal=True)
        dialog.set_default_size(360, 180)
        dialog.present()
        return False

    GLib.timeout_add(1500, open_dialog)


app = Gtk.Application(application_id=sys.argv[1] if len(sys.argv) > 1 else "org.expanse.SweepDialog")
app.connect("activate", activate)
app.run([sys.argv[0]])
