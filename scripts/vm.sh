#!/bin/bash
# Builds and reloads the plugin in a Try Omarchy VM over SSH.
#
#   scripts/vm.sh sync      copy the source to ~/expanse-hypr in the VM
#   scripts/vm.sh build     sync, make, and reload the plugin
#   scripts/vm.sh run CMD   run CMD in the VM as the desktop user, hyprctl reachable
#
# Set VM_USER, VM_PORT (default 2222, Try Omarchy's "Add SSH") and VM_KEY.
set -euo pipefail

VM_USER=${VM_USER:-test}
VM_PORT=${VM_PORT:-2222}
VM_KEY=${VM_KEY:-$HOME/.ssh/omarchy_vm_ed25519}
ROOT=$(cd "$(dirname "$0")/.." && pwd)

ssh_vm() {
  ssh -i "$VM_KEY" -p "$VM_PORT" -o BatchMode=yes -o LogLevel=ERROR "$VM_USER@127.0.0.1" "$@"
}

run() {
  ssh_vm 'export XDG_RUNTIME_DIR=/run/user/$(id -u) WAYLAND_DISPLAY=wayland-1; export HYPRLAND_INSTANCE_SIGNATURE=$(ls -t $XDG_RUNTIME_DIR/hypr | head -1); '"$*"
}

sync() {
  (cd "$ROOT" && COPYFILE_DISABLE=1 tar --exclude=.git --exclude='*.so' -czf - .) |
    ssh_vm 'mkdir -p ~/expanse-hypr && tar -xzf - -C ~/expanse-hypr 2>/dev/null'
}

case ${1:-} in
  sync) sync ;;
  build)
    sync
    run 'cd ~/expanse-hypr && make && { hyprctl plugin unload "$PWD/expanse.so" >/dev/null 2>&1 || true; hyprctl plugin load "$PWD/expanse.so"; }'
    ;;
  run)
    shift
    run "$@"
    ;;
  *)
    sed -n '2,9p' "$0"
    exit 1
    ;;
esac
