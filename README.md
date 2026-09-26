# Normal Productivity

A terminal-based productivity suite written in Go using the Charm Bubble Tea library. It combines a 7-day weekly task planner, time tracking, customizable task recurrence, desktop reminders, and an integrated Pomodoro timer with eye-rest features into a responsive TUI.

---

## Features

- **Weekly Planner Grid**: Responsive 7-day calendar view with date navigation, current-day highlighting, and daily task counts.
- **Task Management**:
  - Timed tasks with start times, duration tracking, and custom deadlines.
  - Granular task statuses: Pending, Active, Paused, Completed, and Skipped.
  - Live completion percentage based on tracked vs. estimated duration.
- **Flexible Recurrence Logic**:
  - Interval-based (every $N$ days).
  - Specific day-of-week selection.
  - Dynamic "After Done" recurrence (reschedules $N$ days after completion).
- **Integrated Pomodoro Engine**:
  - Customizable Work and Break intervals.
  - 20-20-20 Eye Rest Rule enforcement (triggers a 20-second break every 20 minutes).
  - Automatic status and elapsed time sync with the target task.
- **Desktop Notifications**: Uses system-native `notify-send` for scheduled reminders and Pomodoro phase changes.
- **Local SQLite Persistence**: Automatic storage management under `~/.local/share/normal-productivity/tasks.db`.
- **Responsive Layout**: Dynamic UI scaling that adapts to terminal resizing with minimum window size safety checks.

---

## Architecture

The project is structured into two core components:

1. `tui.go`: Handles the Bubble Tea model state machine (`stateMainMenu`, `stateSchedule`, `stateTaskEdit`, `statePomodoro`, `statePomodoroRun`, `stateInfo`), user input events, dynamic Lip Gloss rendering, and UI scaling logic.
2. `logic.go`: Manages SQLite database operations, task recurrence parsing, background tick calculations, and `notify-send` system calls.

---

## Prerequisites

Before building `normal-productivity`, ensure you have the following installed on your system:

- **Go**: Version 1.18 or higher.
- **SQLite3 Development Libraries**: Required for CGO compilation with `go-sqlite3` (e.g., `libsqlite3-dev` on Debian/Ubuntu or `sqlite` on Arch Linux).
- **libnotify**: Provides the `notify-send` executable for Linux desktop notifications.
- **Make**: Standard build tool to run project automation tasks.
- **Air** *(Optional)*: Live-reload engine for Go development (`go install github.com/air-verse/air@latest`).

---

## Building & Running

### Using Make

1. Clone the repository:
   ```bash
   git clone https://github.com/your-username/normal-productivity.git
   cd normal-productivity
   ```

2. Build the binary:
   ```bash
   make build
   ```
   *The binary will be generated in the `build/` directory.*

3. Run the application:
   ```bash
   make run
   ```

4. Development mode with live reloading (requires `air`):
   ```bash
   make dev
   ```

5. Clean build artifacts:
   ```bash
   make clean
   ```

### Manual Compilation

Alternatively, you can compile and run directly with Go:

```bash
go build -o build/normal-productivity .
./build/normal-productivity
```

---

## Usage & Keybindings

Launch the application via `make run` or directly executing `./build/normal-productivity`.

### Navigation & Global Controls

| Key | Context | Action |
| --- | --- | --- |
| `j` / `Down` | Global | Navigate down |
| `k` / `Up` | Global | Navigate up |
| `Enter` / `Space` | Global | Select option / Focus panel |
| `Esc` / `q` | Global | Return to previous menu / Exit application |
| `Ctrl + C` | Global | Force quit application |

### Schedule View

| Key | Action |
| --- | --- |
| `h` / `Left` | Move to previous day (auto-paginates week if needed) |
| `l` / `Right` | Move to next day (auto-paginates week if needed) |
| `t` | Jump directly to today |
| `a` | Add a new task for the selected day |
| `s` | Start / Pause tracking for the selected task |
| `d` | Mark task as completed |
| `D` | Skip task instance |
| `X` | Delete recurring task series |
| `p` | Attach Pomodoro timer to selected task |

### Task Editor

| Key | Action |
| --- | --- |
| `i` / `Enter` | Edit selected field or enter text input mode |
| `h` / `l` | Decrease / Increase time fields by 5 minutes |
| `H` / `L` | Decrease / Increase time fields by 60 minutes |
| `q` | Save task updates and exit editor |
| `Esc` | Cancel editing and discard unsaved changes |

### Pomodoro Timer

| Key | Action |
| --- | --- |
| `s` | Pause / Resume current Pomodoro session |
| `c` | Cancel active Pomodoro session |
| `q` | Run Pomodoro session in the background |

---

## Data Storage

All local task definitions and instance states are saved in SQLite format at:

```
~/.local/share/normal-productivity/tasks.db
```

The database utilizes two main tables:
- `series`: Stores primary task definitions, time metadata, and recurrence configurations.
- `instances`: Stores daily execution metrics, statuses, and elapsed tracking time per task.

---

## License

This project is licensed under the MIT License. See the `LICENSE` file for details.

