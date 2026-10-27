#!/usr/bin/python3
"""A window and, a moment later, a modal dialog over it (GTK 4), for scripts/sweep.py.

    scripts/dialog.py APP_ID
"""

import sys

import gi

gi.require_version("Gtk", "4.0")
from gi.repository import GLib, Gtk  # noqa: E402


def activate(app):
    parent = Gtk.ApplicationWindow(application=app, title="dialog-parent")
    parent.set_default_size(700, 450)
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
