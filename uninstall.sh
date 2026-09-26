#!/bin/sh

# Exit immediately if a command exits with a non-zero status
set -e

APP_NAME="normal-productivity"
INSTALL_DIR="$HOME/.local/bin"
DATA_DIR="$HOME/.local/share/normal-productivity"
CONFIG_DIR="$HOME/.config/normal-productivity"

# Logging helpers
info() { printf "\033[34m[INFO]\033[0m %s\n" "$1"; }
warn() { printf "\033[33m[WARN]\033[0m %s\n" "$1"; }
err() {
  printf "\033[31m[ERR]\033[0m %s\n" "$1"
  exit 1
}

info "Starting uninstallation process for '$APP_NAME'..."

# 1. Always remove the symlink from ~/.local/bin
if [ -L "$INSTALL_DIR/$APP_NAME" ] || [ -f "$INSTALL_DIR/$APP_NAME" ]; then
  rm -f "$INSTALL_DIR/$APP_NAME"
  info "Removed binary/symlink from $INSTALL_DIR/$APP_NAME"
else
  warn "No binary or symlink found at $INSTALL_DIR/$APP_NAME"
fi

printf "\n"

# 2. Option: Remove user config & data files (Default: Keep)
read -p "Do you want to delete user config and app data ($CONFIG_DIR, $DATA_DIR)? [y/N] " remove_data
case "$remove_data" in
[yY][eE][sS] | [yY])
  if [ -d "$CONFIG_DIR" ]; then
    rm -rf "$CONFIG_DIR"
    info "Removed configuration directory: $CONFIG_DIR"
  fi
  if [ -d "$DATA_DIR" ]; then
    rm -rf "$DATA_DIR"
    info "Removed data directory: $DATA_DIR"
  fi
  ;;
*)
  info "Kept configuration and data files intact."
  ;;
esac

printf "\n"

# 3. Option: Remove local repository / source code directory (Default: Keep)
PROJECT_ROOT="$(pwd)"
read -p "Do you want to delete the current source code directory ($PROJECT_ROOT)? [y/N] " remove_source
case "$remove_source" in
[yY][eE][sS] | [yY])
  warn "Deleting current directory in 3 seconds... Press Ctrl+C to abort!"
  sleep 3
  cd "$HOME"
  rm -rf "$PROJECT_ROOT"
  info "Source code directory deleted."
  ;;
*)
  info "Kept source code directory intact."
  ;;
esac

printf "\n"
info "Uninstallation complete!"
