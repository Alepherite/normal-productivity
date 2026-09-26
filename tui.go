package main

import (
	"fmt"
	"strconv"
	"strings"
	"time"

	"github.com/charmbracelet/bubbles/textinput"
	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
)

type sessionState int

const (
	stateMainMenu sessionState = iota
	stateSchedule
	stateTaskEdit
	statePomodoro
	statePomodoroRun
	stateInfo
)

var (
	white   = lipgloss.Color("#FFFFFF")
	dimGray = lipgloss.Color("240")
	black   = lipgloss.Color("#000000")
	red     = lipgloss.Color("#FF5555")

	baseStyle = lipgloss.NewStyle().
			Border(lipgloss.NormalBorder()).
			BorderForeground(dimGray).
			Foreground(dimGray).
			Align(lipgloss.Center, lipgloss.Center)

	activeStyle = baseStyle.Copy().
			Border(lipgloss.ThickBorder()).
			BorderForeground(white).
			Foreground(white).
			Bold(true)

	hlWhite     = lipgloss.NewStyle().Foreground(white).Bold(true)
	dimText     = lipgloss.NewStyle().Foreground(dimGray)
	reverseText = lipgloss.NewStyle().Background(white).Foreground(black).Bold(true)
)

type tickMsg time.Time

func doTick() tea.Cmd {
	return tea.Tick(time.Second, func(t time.Time) tea.Msg { return tickMsg(t) })
}

type model struct {
	state          sessionState
	width, height  int
	tasksDB        map[string][]Task
	currentWeekSun time.Time

	mainSel       int
	schedSelC     int
	taskSelIdx    int
	isTaskFocused bool
	selectedDate  string

	editSelIdx   int
	isInsertMode bool
	textInput    textinput.Model

	pomoSel            int
	pomoTaskDate       string
	pomoTaskIdx        int
	pomoTaskID         int
	workTime           int
	breakTime          int
	eyeBreakEnabled    bool
	timeRemaining      int
	isWorkPhase        bool
	isPomoActive       bool
	isPomoPaused       bool
	sessionTimeElapsed int
	isEyeBreakActive   bool
	eyeBreakRemaining  int
	pomoLastTick       time.Time
}

func initialModel() model {
	ti := textinput.New()
	ti.Prompt = "> "
	ti.PromptStyle = hlWhite
	ti.TextStyle = lipgloss.NewStyle().Foreground(white)
	ti.Cursor.Style = reverseText
	ti.Width = 38
	ti.Focus()

	now := time.Now()
	sun := now.AddDate(0, 0, -int(now.Weekday()))

	m := model{
		state:           stateMainMenu,
		textInput:       ti,
		currentWeekSun:  sun,
		schedSelC:       int(now.Weekday()),
		workTime:        25,
		breakTime:       5,
		eyeBreakEnabled: true,
	}
	m = m.reloadTasks()
	m.selectedDate = m.currentWeekSun.AddDate(0, 0, m.schedSelC).Format("2006-01-02")
	return m
}

func (m model) Init() tea.Cmd {
	return tea.Batch(textinput.Blink, doTick())
}

// --- CORE REFRESH LOGIC ---
func (m model) reloadTasks() model {
	tasksDB := make(map[string][]Task)
	windowStart := m.currentWeekSun.AddDate(0, 0, -30)
	windowEnd := m.currentWeekSun.AddDate(0, 0, 60)
	limitPast := windowStart.Format("2006-01-02")
	limitFuture := windowEnd.Format("2006-01-02")

	// [FIX]: Added ORDER BY id ASC to ensure stable task list ordering
	rows, err := db.Query("SELECT * FROM series ORDER BY id ASC")
	if err == nil && rows != nil {
		defer rows.Close()
		for rows.Next() {
			var t Task
			rows.Scan(
				&t.ID, &t.Name, &t.Desc, &t.HasStart, &t.StartMin, 
				&t.DurationMin, &t.HasCustomDeadline, &t.DeadlineMin, 
				&t.RepeatType, &t.RepeatVal, &t.CreatedDate, &t.UntilDate,
			)

			endLimit := limitFuture
			if t.UntilDate != "" && t.UntilDate < limitFuture {
				endLimit = t.UntilDate
			}

			currTm, _ := time.Parse("2006-01-02", t.CreatedDate)
			interval := 1
			weekDays := []int{}

			if t.RepeatType == 1 {
				interval, _ = strconv.Atoi(t.RepeatVal)
				if interval <= 0 {
					interval = 1
				}
			} else if t.RepeatType == 2 {
				if t.RepeatVal == "" {
					weekDays = append(weekDays, int(currTm.Weekday()))
				} else {
					for _, p := range strings.Split(t.RepeatVal, ",") {
						w, err := strconv.Atoi(p)
						if err == nil {
							weekDays = append(weekDays, w)
						}
					}
				}
			}

			startOffset := 0
			if t.CreatedDate < limitPast {
				startOffset = int(windowStart.Sub(currTm).Hours() / 24)
				if startOffset < 0 {
					startOffset = 0
				}
			}

			for i := startOffset; i <= startOffset+95; i++ {
				nextTm := currTm.AddDate(0, 0, i)
				nextDate := nextTm.Format("2006-01-02")
				if nextDate > endLimit {
					break
				}
				if nextDate < limitPast {
					continue
				}

				shouldSpawn := false
				if t.RepeatType == 0 || t.RepeatType == 3 {
					shouldSpawn = (i == 0)
				} else if t.RepeatType == 1 && (i%interval == 0) {
					shouldSpawn = true
				} else if t.RepeatType == 2 {
					for _, wd := range weekDays {
						if int(nextTm.Weekday()) == wd {
							shouldSpawn = true
							break
						}
					}
				}

				if shouldSpawn {
					tasksDB[nextDate] = append(tasksDB[nextDate], t)
				}
				if t.RepeatType == 0 || t.RepeatType == 3 {
					break
				}
			}
		}
	}

	instRows, err := db.Query("SELECT series_id, date_str, status, last_start, elapsed_sec, is_notified FROM instances WHERE date_str >= ? AND date_str <= ?", limitPast, limitFuture)
	if err == nil && instRows != nil {
		defer instRows.Close()
		for instRows.Next() {
			var sID, status, isNotified int
			var dStr string
			var lastStart, elapsed int64
			instRows.Scan(&sID, &dStr, &status, &lastStart, &elapsed, &isNotified)

			if tList, ok := tasksDB[dStr]; ok {
				for i := range tList {
					if tList[i].ID == sID {
						tList[i].Status = status
						tList[i].LastStartTimestamp = lastStart
						tList[i].ElapsedSec = elapsed
						tList[i].IsNotified = (isNotified != 0)
						break
					}
				}
			}
		}
	}
	m.tasksDB = tasksDB

	if m.isPomoActive && m.pomoTaskDate != "" {
		found := false
		if tasks, ok := m.tasksDB[m.pomoTaskDate]; ok {
			for i, t := range tasks {
				if t.ID == m.pomoTaskID && m.pomoTaskID != 0 {
					m.pomoTaskIdx = i
					found = true
					break
				}
			}
		}
		if !found {
			m.isPomoActive = false
		}
	}
	return m
}

func handleTick(m model) model {
	now := time.Now()
	todayStr := now.Format("2006-01-02")
	yesterdayStr := now.AddDate(0, 0, -1).Format("2006-01-02")

	for _, dKey := range []string{yesterdayStr, todayStr} {
		if tasks, ok := m.tasksDB[dKey]; ok {
			for i, t := range tasks {
				if t.HasStart && t.Status == 0 && !t.IsNotified {
					dateTm, _ := time.Parse("2006-01-02", dKey)
					taskEpoch := dateTm.Unix() + int64(t.StartMin*60)
					if now.Unix() >= taskEpoch && now.Unix()-taskEpoch < 7200 {
						t.IsNotified = true
						saveInstance(t, dKey)
						m.tasksDB[dKey][i] = t
						notifySend("critical", "Task Reminder", "Time to work: "+t.Name)
					}
				}
			}
		}
	}

	if m.isPomoActive && !m.isPomoPaused {
		delta := int(now.Sub(m.pomoLastTick).Seconds())
		if delta > 0 {
			if delta > 180 { 
				m.isPomoPaused = true
				m.pomoLastTick = now
				return m
			}
			m.timeRemaining -= delta
			m.sessionTimeElapsed += delta
			m.pomoLastTick = now

			if m.eyeBreakEnabled {
				prevElapsed := m.sessionTimeElapsed - delta
				if prevElapsed >= 0 && (m.sessionTimeElapsed/1200) > (prevElapsed/1200) {
					m.isEyeBreakActive = true
					m.eyeBreakRemaining = 20
					if m.state != statePomodoroRun {
						m.state = statePomodoroRun
					}
				}
			}
			if m.isEyeBreakActive {
				m.eyeBreakRemaining -= delta
				if m.eyeBreakRemaining <= 0 {
					m.isEyeBreakActive = false
				}
			}

			if m.timeRemaining <= 0 {
				m.isWorkPhase = !m.isWorkPhase
				if m.isWorkPhase {
					m.timeRemaining = m.workTime * 60
				} else {
					m.timeRemaining = m.breakTime * 60
				}
				m.sessionTimeElapsed = 0
				m.isEyeBreakActive = false

				t := m.tasksDB[m.pomoTaskDate][m.pomoTaskIdx]
				if m.isWorkPhase {
					if t.Status != 1 {
						t.Status = 1
						t.LastStartTimestamp = now.Unix()
					}
					notifySend("critical", "Pomodoro", "Work phase started!")
				} else {
					if t.Status == 1 {
						t.Status = 2
						t.ElapsedSec += (now.Unix() - t.LastStartTimestamp)
						t.LastStartTimestamp = 0
					}
					notifySend("critical", "Pomodoro", "Break phase started!")
				}
				m.tasksDB[m.pomoTaskDate][m.pomoTaskIdx] = t
				saveInstance(t, m.pomoTaskDate)
				fmt.Print("\a") // System beep
			}
		}
	} else {
		m.pomoLastTick = now
	}
	return m
}

func (m model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	var cmd tea.Cmd
	var cmds []tea.Cmd

	switch msg := msg.(type) {
	case tickMsg:
		m = handleTick(m)
		return m, doTick()

	case tea.WindowSizeMsg:
		m.width = msg.Width
		m.height = msg.Height
		return m, nil

	case tea.KeyMsg:
		k := msg.String()
		if k == "ctrl+c" {
			return m, tea.Quit
		}

		m.selectedDate = m.currentWeekSun.AddDate(0, 0, m.schedSelC).Format("2006-01-02")
		tasks := m.tasksDB[m.selectedDate]

		switch m.state {
		case stateMainMenu:
			if k == "q" || k == "esc" {
				return m, tea.Quit
			}
			if k == "k" || k == "up" {
				if m.mainSel > 0 {
					m.mainSel--
				}
			}
			if k == "j" || k == "down" {
				if m.mainSel < 1 {
					m.mainSel++
				}
			}
			if k == "enter" || k == " " {
				if m.mainSel == 0 {
					m.state = stateSchedule
					m.isTaskFocused = false
				} else {
					m.state = stateInfo
				}
			}

		case stateSchedule:
			if !m.isTaskFocused {
				if k == "q" || k == "esc" {
					m.state = stateMainMenu
				}
				if k == "h" || k == "left" {
					if m.schedSelC > 0 {
						m.schedSelC--
					} else {
						m.schedSelC = 6
						m.currentWeekSun = m.currentWeekSun.AddDate(0, 0, -7)
						m = m.reloadTasks()
					}
				}
				if k == "l" || k == "right" {
					if m.schedSelC < 6 {
						m.schedSelC++
					} else {
						m.schedSelC = 0
						m.currentWeekSun = m.currentWeekSun.AddDate(0, 0, 7)
						m = m.reloadTasks()
					}
				}
				if k == "t" {
					now := time.Now()
					m.currentWeekSun = now.AddDate(0, 0, -int(now.Weekday()))
					m.schedSelC = int(now.Weekday())
					m = m.reloadTasks()
				}
				if k == "enter" || k == " " {
					if len(m.tasksDB[m.currentWeekSun.AddDate(0, 0, m.schedSelC).Format("2006-01-02")]) > 0 {
						m.isTaskFocused = true
						m.taskSelIdx = 0
					}
				}
				if k == "a" {
					m = m.startInsertMode()
				}
			} else {
				if k == "q" || k == "esc" {
					m.isTaskFocused = false
				}
				if k == "j" || k == "down" {
					if m.taskSelIdx < len(tasks)-1 {
						m.taskSelIdx++
					}
				}
				if k == "k" || k == "up" {
					if m.taskSelIdx > 0 {
						m.taskSelIdx--
					}
				}
				if k == "a" {
					m = m.startInsertMode()
				}
				if k == "enter" {
					m.state = stateTaskEdit
					m.editSelIdx = 0
				}
				if k == "p" {
					if m.isPomoActive && m.pomoTaskDate == m.selectedDate && m.pomoTaskIdx == m.taskSelIdx {
						m.state = statePomodoroRun
					} else {
						m.pomoTaskDate = m.selectedDate
						m.pomoTaskIdx = m.taskSelIdx
						m.pomoTaskID = tasks[m.taskSelIdx].ID
						m.state = statePomodoro
						m.pomoSel = 0
					}
				}
				if k == "s" {
					m = m.toggleTaskState()
				}
				if k == "d" {
					m = m.markTaskDone()
				}
				if k == "D" || k == "X" {
					m = m.deleteTask(k == "X")
				}
			}

		case stateTaskEdit:
			t := m.tasksDB[m.selectedDate][m.taskSelIdx]
			maxFields := 3
			if t.HasStart {
				maxFields = 6
				if t.HasCustomDeadline {
					maxFields = 7
				}
			}
			idxRepeat := maxFields
			maxFields += 1
			if t.RepeatType > 0 {
				maxFields += 1
			}

			if m.isInsertMode {
				if k == "esc" || k == "enter" {
					m.isInsertMode = false
					val := m.textInput.Value()
					if k == "enter" {
						if m.editSelIdx == 0 && val != "" {
							t.Name = val
						} else if m.editSelIdx == 1 {
							t.Desc = val
						} else if m.editSelIdx == idxRepeat+1 {
							t.RepeatVal = val
						}
						m.tasksDB[m.selectedDate][m.taskSelIdx] = t
					}
				} else {
					m.textInput, cmd = m.textInput.Update(msg)
					cmds = append(cmds, cmd)
				}
			} else {
				if k == "esc" {
					m = m.reloadTasks() 
					m.state = stateSchedule
				}
				if k == "q" { 
					if t.ID == 0 {
						if t.Name != "New Task" || t.Desc != "" {
							insertSeries(&t)
						}
					} else {
						updateSeries(t)
					}
					m = m.reloadTasks()
					m.state = stateSchedule
				}
				if k == "j" || k == "down" {
					if m.editSelIdx < maxFields-1 {
						m.editSelIdx++
					}
				}
				if k == "k" || k == "up" {
					if m.editSelIdx > 0 {
						m.editSelIdx--
					}
				}
				if k == "i" || k == "enter" || k == " " {
					if m.editSelIdx == 2 {
						t.HasStart = !t.HasStart
					} else if m.editSelIdx == 5 && t.HasStart {
						t.HasCustomDeadline = !t.HasCustomDeadline
					} else if m.editSelIdx == idxRepeat {
						t.RepeatType = (t.RepeatType + 1) % 4
						if t.RepeatType == 0 {
							t.RepeatVal = ""
						}
					} else if m.editSelIdx == 0 || m.editSelIdx == 1 || m.editSelIdx == idxRepeat+1 {
						m.isInsertMode = true
						m.textInput.Reset()
						if m.editSelIdx == 0 {
							m.textInput.SetValue(t.Name)
						} else if m.editSelIdx == 1 {
							m.textInput.SetValue(t.Desc)
						} else if m.editSelIdx == idxRepeat+1 {
							m.textInput.SetValue(t.RepeatVal)
						}
						m.textInput.CursorEnd()
					}
					m.tasksDB[m.selectedDate][m.taskSelIdx] = t
				}
				if k == "h" || k == "H" || k == "l" || k == "L" || k == "left" || k == "right" {
					delta := 5
					if k == "H" || k == "L" {
						delta = 60
					}
					if k == "h" || k == "H" || k == "left" {
						delta = -delta
					}

					if m.editSelIdx == 3 && t.HasStart {
						t.StartMin = max(0, t.StartMin+delta)
					} else if m.editSelIdx == 4 && t.HasStart {
						t.DurationMin = max(0, t.DurationMin+delta)
					} else if m.editSelIdx == 6 && t.HasCustomDeadline {
						t.DeadlineMin = max(0, t.DeadlineMin+delta)
					}
					m.tasksDB[m.selectedDate][m.taskSelIdx] = t
				}
			} 

		case statePomodoro:
			if k == "q" || k == "esc" {
				m.state = stateSchedule
			}
			if k == "k" || k == "up" {
				if m.pomoSel > 0 {
					m.pomoSel--
				}
			}
			if k == "j" || k == "down" {
				if m.pomoSel < 3 {
					m.pomoSel++
				}
			}
			if k == "h" || k == "left" {
				if m.pomoSel == 1 && m.workTime > 1 {
					m.workTime--
				}
				if m.pomoSel == 2 && m.breakTime > 1 {
					m.breakTime--
				}
			}
			if k == "l" || k == "right" {
				if m.pomoSel == 1 && m.workTime < 99 {
					m.workTime++
				}
				if m.pomoSel == 2 && m.breakTime < 99 {
					m.breakTime++
				}
			}
			if k == "enter" || k == " " {
				if m.pomoSel == 0 {
					if m.isPomoActive && (m.pomoTaskDate != m.selectedDate || m.pomoTaskIdx != m.taskSelIdx) {
						oldT := m.tasksDB[m.pomoTaskDate][m.pomoTaskIdx]
						if oldT.Status == 1 {
							oldT.Status = 2
							oldT.ElapsedSec += (time.Now().Unix() - oldT.LastStartTimestamp)
							oldT.LastStartTimestamp = 0
							saveInstance(oldT, m.pomoTaskDate)
						}
					}
					m.isPomoActive = true
					m.isPomoPaused = false
					m.state = statePomodoroRun
					m.pomoLastTick = time.Now()
					m.isWorkPhase = true
					m.timeRemaining = m.workTime * 60
					m.sessionTimeElapsed = 0
					m.isEyeBreakActive = false
					
					t := m.tasksDB[m.pomoTaskDate][m.pomoTaskIdx]
					if t.Status != 1 {
						t.Status = 1
						t.LastStartTimestamp = time.Now().Unix()
						m.tasksDB[m.pomoTaskDate][m.pomoTaskIdx] = t
						saveInstance(t, m.pomoTaskDate)
					}
				} else if m.pomoSel == 3 {
					m.eyeBreakEnabled = !m.eyeBreakEnabled
				}
			}

		case statePomodoroRun:
			if k == "q" || k == "esc" {
				m.state = stateSchedule
			}
			if k == "s" && !m.isEyeBreakActive {
				m.isPomoPaused = !m.isPomoPaused
				if !m.isPomoPaused {
					m.pomoLastTick = time.Now()
				}
				t := m.tasksDB[m.pomoTaskDate][m.pomoTaskIdx]
				if m.isPomoPaused {
					if t.Status == 1 {
						t.Status = 2
						t.ElapsedSec += (time.Now().Unix() - t.LastStartTimestamp)
						t.LastStartTimestamp = 0
					}
				} else {
					if m.isWorkPhase {
						t.Status = 1
						t.LastStartTimestamp = time.Now().Unix()
					}
				}
				m.tasksDB[m.pomoTaskDate][m.pomoTaskIdx] = t
				saveInstance(t, m.pomoTaskDate)
			}
			if k == "c" {
				m.isPomoActive = false
				t := m.tasksDB[m.pomoTaskDate][m.pomoTaskIdx]
				if t.Status == 1 {
					t.Status = 2
					t.ElapsedSec += (time.Now().Unix() - t.LastStartTimestamp)
					t.LastStartTimestamp = 0
				}
				m.tasksDB[m.pomoTaskDate][m.pomoTaskIdx] = t
				saveInstance(t, m.pomoTaskDate)
				m.state = stateSchedule
			}

		case stateInfo:
			if k == "q" || k == "esc" || k == "enter" {
				m.state = stateMainMenu
			}
		}

		// Ensure task cursor stays in bounds after data manipulations
		m.selectedDate = m.currentWeekSun.AddDate(0, 0, m.schedSelC).Format("2006-01-02")
		if m.isTaskFocused {
			tList := m.tasksDB[m.selectedDate]
			if len(tList) == 0 {
				m.isTaskFocused = false
				m.taskSelIdx = 0
			} else if m.taskSelIdx >= len(tList) {
				m.taskSelIdx = len(tList) - 1
			}
		}
	}
	return m, tea.Batch(cmds...)
}

func (m model) View() string {
	if m.width == 0 {
		return "Initializing..."
	}
	
	var ui string
	switch m.state {
	case stateMainMenu:
		menuItems := []string{"Schedule", "Info"}
		var boxes []string
		for i, text := range menuItems {
			style := baseStyle
			if i == m.mainSel {
				style = activeStyle
			}
			boxes = append(boxes, style.Width(24).Height(3).Render(text))
		}
		ui = lipgloss.JoinVertical(lipgloss.Center, boxes...)

	case stateSchedule:
		var dayBoxes []string
		dayNames := []string{"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"}
		for i := 0; i < 7; i++ {
			date := m.currentWeekSun.AddDate(0, 0, i)
			dStr := date.Format("02/01")
			cnt := len(m.tasksDB[date.Format("2006-01-02")])
			cntStr := ""
			if cnt > 0 {
				cntStr = fmt.Sprintf("[%d]", cnt)
			}
			
			content := fmt.Sprintf("%s\n%s\n%s", dayNames[i], dStr, cntStr)
			style := baseStyle.Copy().Width(9).Height(5)
			
			isToday := isSameDay(date, time.Now())
			if isToday {
				style = style.Foreground(red).BorderForeground(red)
			}

			if i == m.schedSelC { 
				if isToday {
					style = style.Border(lipgloss.ThickBorder()).BorderForeground(red).Foreground(black).Background(red).Bold(true)
				} else {
					style = style.Border(lipgloss.ThickBorder()).BorderForeground(white).Foreground(black).Background(white).Bold(true)
				}
			}
			dayBoxes = append(dayBoxes, style.Render(content))
		}
		grid := lipgloss.JoinHorizontal(lipgloss.Top, dayBoxes...)
		
		listUI := hlWhite.Render("Tasks for " + m.selectedDate) + "\n\n"
		tasks := m.tasksDB[m.selectedDate]
		if len(tasks) == 0 {
			listUI += dimText.Render("< No tasks for this day >") + "\n"
		} else {
			for i, t := range tasks {
				prefix := "  "
				if m.isTaskFocused && i == m.taskSelIdx {
					prefix = "> "
				}
				
				statusSym := "[ ]"
				if t.Status == 1 {
					statusSym = "[>]"
				} else if t.Status == 2 {
					statusSym = "[||]"
				} else if t.Status == 3 {
					statusSym = "[v]"
				} else if t.Status == 4 {
					statusSym = "[S]"
				}
				
				timeStr := "     "
				if t.HasStart {
					timeStr = formatTime24(t.StartMin)
				}
				
				sep := " │ "
				if m.isPomoActive && m.pomoTaskDate == m.selectedDate && m.pomoTaskIdx == i {
					sep = " P "
				}
				
				pct := 0.0
				if t.DurationMin > 0 {
					pct = (float64(getElapsedSec(t)) / float64(t.DurationMin*60)) * 100
				}
				row := fmt.Sprintf("%s%s %s%s%s [%5.1f%%]", prefix, statusSym, timeStr, sep, t.Name, pct)
				
				if m.isTaskFocused && i == m.taskSelIdx { 
					row = reverseText.Render(row) 
				} else if t.Status == 1 { 
					row = hlWhite.Render(row) 
				} else if t.Status == 3 || t.Status == 4 { 
					row = dimText.Render(row) 
				}
				listUI += row + "\n"
			}
		}
		ui = lipgloss.JoinVertical(lipgloss.Left, grid, "", listUI)

	case stateTaskEdit:
		t := m.tasksDB[m.selectedDate][m.taskSelIdx]
		
		// 1. Popup Modal Input Mode
		if m.isInsertMode {
			titles := map[int]string{
				0: "Edit Name", 
				1: "Edit Description", 
				4: "Edit Repeat Value", 
				7: "Edit Repeat Value", 
				8: "Edit Repeat Value",
			}
			title := titles[m.editSelIdx]
			
			boxContent := lipgloss.PlaceHorizontal(40, lipgloss.Center, hlWhite.Render(title)) + "\n\n" +
						  lipgloss.NewStyle().Padding(0, 1).Render(m.textInput.View()) + "\n\n" +
						  lipgloss.PlaceHorizontal(40, lipgloss.Center, dimText.Render("[Enter] Confirm   [Esc] Cancel"))

			modal := lipgloss.NewStyle().
				Border(lipgloss.RoundedBorder()).
				BorderForeground(white).
				Padding(1, 2).
				Render(boxContent)

			return lipgloss.Place(m.width, m.height, lipgloss.Center, lipgloss.Center, modal)
		}

		// 2. Main List Rendering Mode
		ui = lipgloss.PlaceHorizontal(54, lipgloss.Center, hlWhite.Render("=== Edit Task ===")) + "\n\n"
		
		renderRow := func(idx int, label, val string, isField bool) string {
			left := fmt.Sprintf("%20s", label)
			right := fmt.Sprintf("< %-26s >", val)
			if isField {
				right = fmt.Sprintf("[ %-26s ]", val)
			}

			rowStr := fmt.Sprintf("%s : %s", left, right)
			if m.editSelIdx == idx {
				return reverseText.Render(rowStr) + "\n"
			}
			return dimText.Render(rowStr) + "\n"
		}

		ui += renderRow(0, "Name", t.Name, true)
		ui += renderRow(1, "Description", t.Desc, true)
		ui += renderRow(2, "Has Start Time", map[bool]string{true: "Yes", false: "No"}[t.HasStart], true)
		
		if t.HasStart {
			ui += renderRow(3, "Start Time", formatTime24(t.StartMin), false)
			ui += renderRow(4, "Duration", formatTime24(t.DurationMin), false)
			ui += renderRow(5, "Custom Deadline", map[bool]string{true: "Yes", false: "No"}[t.HasCustomDeadline], true)
			if t.HasCustomDeadline {
				ui += renderRow(6, "Deadline Time", formatTime24(t.DeadlineMin), false)
			}
		}
		
		idxRep := 3
		if t.HasStart {
			idxRep = 6
			if t.HasCustomDeadline {
				idxRep = 7
			}
		}
		
		rStr := []string{"None", "Interval", "Weekly", "After Done"}[t.RepeatType]
		ui += "\n" + renderRow(idxRep, "Repeat Type", rStr, true)
		if t.RepeatType > 0 {
			ui += renderRow(idxRep+1, "Repeat Value", t.RepeatVal, true)
		}

		elap := getElapsedSec(t)
		timeStr := fmt.Sprintf("%02d:%02d:%02d", elap/3600, (elap%3600)/60, elap%60)
		ui += fmt.Sprintf("\n%s\n", lipgloss.PlaceHorizontal(54, lipgloss.Center, dimText.Render("Elapsed Time      : "+timeStr)))
		ui += lipgloss.PlaceHorizontal(54, lipgloss.Center, dimText.Render("[i/Enter] Edit  [Esc] Back  [h/l] +/-  [q] Save"))

	case statePomodoro:
		tName := m.tasksDB[m.pomoTaskDate][m.pomoTaskIdx].Name
		ui = hlWhite.Render("Task: "+tName) + "\n\n"
		items := []string{
			"Start Timer",
			fmt.Sprintf("Work: < %d > min", m.workTime),
			fmt.Sprintf("Break: < %d > min", m.breakTime),
			fmt.Sprintf("Eye Break: < %v >", map[bool]string{true: "ON", false: "OFF"}[m.eyeBreakEnabled]),
		}
		for i, text := range items {
			style := baseStyle.Copy().Width(30).Height(3)
			if i == m.pomoSel {
				style = activeStyle.Copy().Width(30).Height(3)
			}
			ui += style.Render(text) + "\n"
		}

	case statePomodoroRun:
		if m.isEyeBreakActive {
			ui = hlWhite.Render("EYE BREAK: LOOK 20 FEET AWAY") + "\n\n" + fmt.Sprintf("Resuming in %ds", m.eyeBreakRemaining)
		} else {
			phase := "WORK PHASE"
			if !m.isWorkPhase {
				phase = "BREAK PHASE"
			}
			if m.isPomoPaused {
				phase += " (PAUSED)"
			}
			
			h, mRem, s := m.timeRemaining/3600, (m.timeRemaining%3600)/60, m.timeRemaining%60
			ui = hlWhite.Render(phase) + "\n\n" + fmt.Sprintf("%02d:%02d:%02d", h, mRem, s) + "\n\n"
			
			total := m.workTime * 60
			if !m.isWorkPhase {
				total = m.breakTime * 60
			}
			
			pct := 1.0 - (float64(m.timeRemaining) / float64(max(1, total)))
			filled := int(pct * 40)
			bar := ""
			for i := 0; i < 40; i++ {
				if i < filled {
					bar += "█"
				} else {
					bar += "─"
				}
			}
			ui += dimText.Render("┌────────────────────────────────────────┐\n")
			ui += dimText.Render("│") + hlWhite.Render(bar) + dimText.Render("│\n")
			ui += dimText.Render("└────────────────────────────────────────┘\n\n")
			ui += dimText.Render("[s] Pause/Resume  [c] Cancel  [q] Background")
		}

	case stateInfo:
		contentWidth := 58 
		title := lipgloss.PlaceHorizontal(contentWidth, lipgloss.Center, hlWhite.Render("MANUAL & KEYBINDS")) + "\n\n"
		
		renderSection := func(name string, items [][]string) string {
			res := lipgloss.PlaceHorizontal(contentWidth, lipgloss.Center, hlWhite.Render(name)) + "\n"
			for _, item := range items {
				left := lipgloss.NewStyle().Width(24).Align(lipgloss.Right).Render(item[0])
				res += fmt.Sprintf("%s  :  %s\n", left, item[1])
			}
			return res + "\n"
		}

		content := title +
			renderSection("[ Navigation ]", [][]string{
				{"j / k", "Move Up / Down"},
				{"Enter / Space", "Select / Focus"},
				{"q / ESC", "Go Back / Exit"},
			}) +
			renderSection("[ Schedule & Task List ]", [][]string{
				{"h / l", "Change Day"},
				{"t", "Jump to Today"},
				{"a", "Add a new task"},
				{"s", "Start / Pause"},
				{"d / D / X", "Done / Skip / Del Series"},
				{"p", "Attach Pomodoro"},
			}) +
			renderSection("[ Editor & Pomodoro ]", [][]string{
				{"i / Enter", "Edit field"},
				{"h/H or l/L", "Adjust time"},
				{"s / c (Pomo)", "Pause / Cancel"},
			})

		ui = lipgloss.NewStyle().Border(lipgloss.NormalBorder()).BorderForeground(dimGray).Foreground(dimGray).Width(64).Padding(1, 2).Align(lipgloss.Left).Render(content)
	}

	return lipgloss.Place(m.width, m.height, lipgloss.Center, lipgloss.Center, ui)
}

func max(a, b int) int {
	if a > b {
		return a
	}
	return b
}

func (m model) startInsertMode() model {
	t := Task{CreatedDate: m.selectedDate, Name: "New Task", DurationMin: 60, StartMin: 480}
	m.tasksDB[m.selectedDate] = append(m.tasksDB[m.selectedDate], t)
	m.state = stateTaskEdit
	m.taskSelIdx = len(m.tasksDB[m.selectedDate]) - 1
	m.editSelIdx = 0
	
	// Khi Add Task mới, tự động pop-up nhập tên luôn cho mượt
	m.isInsertMode = true
	m.textInput.Reset()
	return m
}

func (m model) toggleTaskState() model {
	t := m.tasksDB[m.selectedDate][m.taskSelIdx]
	if t.Status == 0 || t.Status == 2 {
		for dKey, tList := range m.tasksDB {
			for i, otherT := range tList {
				if otherT.Status == 1 && (dKey != m.selectedDate || otherT.ID != t.ID) {
					otherT.Status = 2
					otherT.ElapsedSec += (time.Now().Unix() - otherT.LastStartTimestamp)
					otherT.LastStartTimestamp = 0
					saveInstance(otherT, dKey)
					m.tasksDB[dKey][i] = otherT
				}
			}
		}
		t.Status = 1
		t.LastStartTimestamp = time.Now().Unix()
		notifySend("low", "Task Started", "Working on: "+t.Name)
		if m.isPomoActive && m.pomoTaskDate == m.selectedDate && m.pomoTaskIdx == m.taskSelIdx {
			m.isPomoPaused = false
			m.pomoLastTick = time.Now()
		}
	} else if t.Status == 1 {
		t.Status = 2
		t.ElapsedSec += (time.Now().Unix() - t.LastStartTimestamp)
		t.LastStartTimestamp = 0
		if m.isPomoActive && m.pomoTaskDate == m.selectedDate && m.pomoTaskIdx == m.taskSelIdx {
			m.isPomoPaused = true
		}
	}
	m.tasksDB[m.selectedDate][m.taskSelIdx] = t
	saveInstance(t, m.selectedDate)
	return m.reloadTasks()
}

func (m model) markTaskDone() model {
	t := m.tasksDB[m.selectedDate][m.taskSelIdx]
	if t.Status == 1 {
		t.ElapsedSec += (time.Now().Unix() - t.LastStartTimestamp)
		t.LastStartTimestamp = 0
	}
	t.Status = 3
	saveInstance(t, m.selectedDate)
	if m.isPomoActive && m.pomoTaskDate == m.selectedDate && m.pomoTaskIdx == m.taskSelIdx {
		m.isPomoActive = false
	}
	
	if t.RepeatType == 3 {
		nDays, _ := strconv.Atoi(t.RepeatVal)
		if nDays <= 0 {
			nDays = 1
		}
		targetStr := m.currentWeekSun.AddDate(0, 0, m.schedSelC+nDays).Format("2006-01-02")
		clone := t
		clone.ID = 0
		clone.Status = 0
		clone.ElapsedSec = 0
		clone.LastStartTimestamp = 0
		clone.CreatedDate = targetStr
		insertSeries(&clone)
	}
	m.tasksDB[m.selectedDate][m.taskSelIdx] = t
	return m.reloadTasks()
}

func (m model) deleteTask(isSeries bool) model {
	t := m.tasksDB[m.selectedDate][m.taskSelIdx]
	if m.isPomoActive && m.pomoTaskDate == m.selectedDate && m.pomoTaskIdx == m.taskSelIdx {
		m.isPomoActive = false
	}

	if isSeries && t.ID != 0 && t.RepeatType > 0 {
		yest := time.Now().AddDate(0, 0, -1).Format("2006-01-02")
		if t.UntilDate == "" || t.UntilDate > yest {
			db.Exec("UPDATE series SET until_date = ? WHERE id = ?", yest, t.ID)
		}
	} else {
		if t.RepeatType > 0 {
			t.Status = 4
			saveInstance(t, m.selectedDate)
		} else {
			db.Exec("DELETE FROM series WHERE id=?", t.ID)
		}
	}
	return m.reloadTasks()
}
