package main

import (
	"database/sql"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	_ "github.com/mattn/go-sqlite3"
)

type Task struct {
	ID                 int
	Name               string
	Desc               string
	HasStart           bool
	StartMin           int
	DurationMin        int
	HasCustomDeadline  bool
	DeadlineMin        int
	Status             int
	LastStartTimestamp int64
	ElapsedSec         int64
	IsNotified         bool
	RepeatType         int
	RepeatVal          string
	CreatedDate        string
	UntilDate          string
}

var db *sql.DB

func initDB() {
	home, _ := os.UserHomeDir()
	dir := filepath.Join(home, ".local", "share", "normal-productivity")
	os.MkdirAll(dir, 0777)

	dbPath := filepath.Join(dir, "tasks.db")
	var err error
	db, err = sql.Open("sqlite3", dbPath)
	if err != nil {
		panic(err)
	}

	db.Exec(`CREATE TABLE IF NOT EXISTS series (
		id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, desc TEXT, has_start INT, 
		start_min INT, duration_min INT, has_custom_deadline INT, deadline_min INT, 
		repeat_type INT, repeat_val TEXT, created_date TEXT, until_date TEXT);`)

	db.Exec(`CREATE TABLE IF NOT EXISTS instances (
		series_id INT, date_str TEXT, status INT, last_start INT, elapsed_sec INT, is_notified INT, 
		PRIMARY KEY(series_id, date_str));`)
}

func notifySend(urgency, title, msg string) {
	cmd := exec.Command("notify-send", "-u", urgency, title, msg)
	cmd.Start()
}

func getElapsedSec(t Task) int64 {
	total := t.ElapsedSec
	if t.Status == 1 && t.LastStartTimestamp > 0 {
		now := time.Now().Unix()
		if now > t.LastStartTimestamp {
			total += (now - t.LastStartTimestamp)
		}
	}
	return total
}

func parseSmartTime(buf string, fallback int) int {
	if buf == "" {
		return fallback
	}
	buf = strings.ReplaceAll(buf, " ", ":")
	parts := strings.Split(buf, ":")
	if len(parts) >= 2 {
		h, _ := strconv.Atoi(parts[0])
		m, _ := strconv.Atoi(parts[1])
		return h*60 + m
	}
	val, err := strconv.Atoi(buf)
	if err == nil {
		if val <= 24 && len(buf) <= 2 {
			return val * 60
		}
		return val
	}
	return fallback
}

func formatTime24(totalMins int) string {
	h := (totalMins / 60) % 24
	m := totalMins % 60
	return fmt.Sprintf("%02d:%02d", h, m)
}

func isSameDay(t1, t2 time.Time) bool {
	return t1.Year() == t2.Year() && t1.YearDay() == t2.YearDay()
}

func insertSeries(t *Task) {
	res, _ := db.Exec(`INSERT INTO series (name, desc, has_start, start_min, duration_min, has_custom_deadline, deadline_min, repeat_type, repeat_val, created_date, until_date) 
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
		t.Name, t.Desc, t.HasStart, t.StartMin, t.DurationMin, t.HasCustomDeadline, t.DeadlineMin, t.RepeatType, t.RepeatVal, t.CreatedDate, t.UntilDate)
	if res != nil {
		id, _ := res.LastInsertId()
		t.ID = int(id)
	}
}

func updateSeries(t Task) {
	db.Exec(`UPDATE series SET name=?, desc=?, has_start=?, start_min=?, duration_min=?, has_custom_deadline=?, deadline_min=?, repeat_type=?, repeat_val=?, until_date=? WHERE id=?`,
		t.Name, t.Desc, t.HasStart, t.StartMin, t.DurationMin, t.HasCustomDeadline, t.DeadlineMin, t.RepeatType, t.RepeatVal, t.UntilDate, t.ID)
}

func saveInstance(t Task, dateStr string) {
	if t.ID == 0 {
		return
	}
	db.Exec(`INSERT INTO instances (series_id, date_str, status, last_start, elapsed_sec, is_notified) VALUES (?, ?, ?, ?, ?, ?) 
		ON CONFLICT(series_id, date_str) DO UPDATE SET status=excluded.status, last_start=excluded.last_start, elapsed_sec=excluded.elapsed_sec, is_notified=excluded.is_notified`,
		t.ID, dateStr, t.Status, t.LastStartTimestamp, t.ElapsedSec, t.IsNotified)
}
