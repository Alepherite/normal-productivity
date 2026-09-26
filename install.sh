#!/bin/sh

# Exit immediately if a command exits with a non-zero status
set -e

APP_NAME="normal-productivity"
BUILD_DIR="build"
INSTALL_DIR="$HOME/.local/bin"

# Logging helpers
info() { printf "\033[34m[INFO]\033[0m %s\n" "$1"; }
warn() { printf "\033[33m[WARN]\033[0m %s\n" "$1"; }
err() {
  printf "\033[31m[ERR]\033[0m %s\n" "$1"
  exit 1
}

info "Checking system environment..."

# 1. Check for Go compiler
if ! command -v go >/dev/null 2>&1; then
  err "Go compiler not found! Please install Go runtime first."
fi

# 2. Check for libnotify (notify-send)
if ! command -v notify-send >/dev/null 2>&1; then
  warn "Missing 'libnotify' (notify-send command not found)."
  echo "Suggested command to install build essentials and libnotify for your distro:"

  if [ -f /etc/os-release ]; then
    . /etc/os-release
    case "$ID" in
    arch | cachyos | endeavouros | manjaro)
      echo "  -> sudo pacman -S --needed base-devel libnotify"
      ;;
    void)
      echo "  -> sudo xbps-install -S base-devel libnotify"
      ;;
    debian | ubuntu | pop | mint)
      echo "  -> sudo apt update && sudo apt install build-essential libnotify-bin"
      ;;
    fedora | rhel)
      echo "  -> sudo dnf groupinstall \"Development Tools\" && sudo dnf install libnotify"
      ;;
    *)
      echo "  -> Please manually install 'libnotify' and build tools for this distribution."
      ;;
    esac
  fi
  echo ""
  read -p "Do you want to proceed with the build anyway? [y/N] " choice
  case "$choice" in
  [yY][eE][sS] | [yY]) ;;
  *) exit 1 ;;
  esac
fi

# 3. Synchronize Go dependencies
info "Syncing Go dependencies via 'go mod tidy'..."
go mod tidy

# 4. Invoke Makefile to build
info "Compiling application using Makefile..."
make build

if [ ! -f "$BUILD_DIR/$APP_NAME" ]; then
  err "Build failed: Executable not found in $BUILD_DIR/"
fi

info "Compilation successful! Binary built at: ./$BUILD_DIR/$APP_NAME"

# 5. Prompt for symlinking into PATH
printf "\n"
read -p "Create a symlink in '$INSTALL_DIR/$APP_NAME' to run it from anywhere? [y/N] " choice

case "$choice" in
[yY][eE][sS] | [yY])
  mkdir -p "$INSTALL_DIR"

  # Use absolute path so the symlink remains valid from anywhere
  TARGET_PATH="$(pwd)/$BUILD_DIR/$APP_NAME"

  ln -sf "$TARGET_PATH" "$INSTALL_DIR/$APP_NAME"
  info "Successfully symlinked to $INSTALL_DIR/$APP_NAME!"

  # Warn if ~/.local/bin isn't in PATH
  case ":$PATH:" in
  *:"$INSTALL_DIR":*) ;;
  *) warn "$INSTALL_DIR is not in your current PATH. Add 'export PATH=\"\$HOME/.local/bin:\$PATH\"' to your shell rc file." ;;
  esac
  ;;
*)
  info "Skipped system install. You can run the app locally via './$BUILD_DIR/$APP_NAME' or 'make run'."
  ;;
esac
