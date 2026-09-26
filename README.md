# normal-productivity

A lightweight, efficient Terminal User Interface (TUI) application written in Go, designed to help you manage personal tasks, track recurring habits, and integrate a Pomodoro timer directly within your Linux workspace.

---

## Introduction

`normal-productivity` combines a weekly schedule board, daily task lists, a recurring task system (series/instances), and a Pomodoro timer with automatic eye break reminders. The application integrates directly with the Linux desktop notification daemon (`notify-send`) to deliver timely reminders.

---

## Core Features

- **Weekly Schedule Management:** Easily navigate and switch between days of the week.
- **Recurring Tasks:**
  - Single (No repeat).
  - Interval (Repeat every X days).
  - Weekly (Repeat on specific days of the week).
  - After Done (Spawn the next instance only after the current one is completed).
- **Integrated Pomodoro Timer:**
  - Customizable work and break durations.
  - Automatic state switching between work and rest.
  - Eye Break reminders (20-20-20 rule) based on elapsed screen time.
  - Runs in the background while keeping task elapsed time updated.
- **Smart Time Parsing:** Flexibly supports input formats like `14 30`, `14:30`, or just `14` which automatically converts to minutes.
- **System Notifications:** Sends desktop notifications directly via the Linux notification server when it's time to work or switch Pomodoro phases.
- **Local Storage:** Utilizes a SQLite3 database stored in the standard XDG path (`~/.local/share/normal-productivity/tasks.db`).

---

## Dependencies

To compile and run this application, your system needs the following:
1. **Go Toolchain** (to compile the application).
2. **C Compiler / Build tools** (required by the SQLite3 CGO driver).
3. **libnotify** (provides the `notify-send` command for notifications).

Below are the installation commands for various Linux distributions:

### Arch Linux

```bash
sudo pacman -S go base-devel libnotify
```

### Debian / Ubuntu

```bash
sudo apt update
sudo apt install -y golang build-essential libnotify-bin
```

### Fedora

```bash
sudo dnf install -y golang gcc libnotify
```

### Void Linux

```bash
sudo xbps-install -Sy go base-devel libnotify
```

---

## Installation and Usage

### 1. Development Mode (Run directly)

Run the program immediately without installing it into the system:

```bash
make run
```

Or using the Go command directly:

```bash
go run .
```

---

### 2. Install via Makefile (Recommended)

The default target will compile the binary and place it in `~/.local/bin/` (the standard user-space binary path on Linux).

Install for the current user (No sudo required):

```bash
make install
```

Install system-wide:

```bash
PREFIX=/usr/local sudo make install
```

---

### 3. Install / Uninstall via Scripts

If provided with installation scripts:

**Installation (`./install.sh`):**

```bash
chmod +x install.sh
./install.sh
```

**Uninstallation (`./uninstall.sh`):**

```bash
chmod +x uninstall.sh
./uninstall.sh
```

---

### 4. Uninstall via Makefile

```bash
make uninstall
```

Or if you installed it system-wide as root:

```bash
PREFIX=/usr/local sudo make uninstall
```

---

## Controls & Keybindings

### General Navigation

- `j` / `Down Arrow`: Move down.
- `k` / `Up Arrow`: Move up.
- `Enter` / `Space`: Select / Focus on list.
- `q` / `Esc`: Go back / Cancel / Exit.
- `Ctrl + c`: Force quit the application.

### Schedule & Task List

- `h` / `Left Arrow`: Move to the previous day.
- `l` / `Right Arrow`: Move to the next day.
- `t`: Jump to Today.
- `a`: Add a new task.
- `s`: Start / Pause the timer for the selected task.
- `d`: Mark task as Done.
- `D` / `X`: Skip or Delete the recurring series.
- `p`: Attach Pomodoro timer to the selected task.

### Task Editor

- `i` / `Enter`: Edit the currently selected field.
- `h` / `l`: Decrease / Increase time (5-minute intervals).
- `H` / `L`: Decrease / Increase time (60-minute intervals).
- `q`: Save changes and return to the schedule.
- `Esc`: Discard changes and return to the schedule.

### Pomodoro Timer

- `s`: Pause / Resume the countdown.
- `c`: Cancel the current Pomodoro session.
- `q`: Let the Pomodoro run in the background and return to the schedule.

---

## Data Storage

All task lists and progress data are automatically saved locally at:

```text
~/.local/share/normal-productivity/tasks.db
```

If you need to back up your data, simply copy the `tasks.db` file to a secure location.