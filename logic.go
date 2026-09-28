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

type WorkoutPlan struct {
	ID         int
	Name       string
	DaysOfWeek string
	SortOrder  int
}

type WorkoutExercise struct {
	ID           int
	PlanID       int
	Name         string
	TargetSets   int
	TargetWeight float64 // [MỚI]
	TargetReps   int     // [MỚI]
	RestSec      int
	SortOrder    int
}

type WorkoutLog struct {
	ID          int
	ExerciseID  int
	DateStr     string
	SetIndex    int
	WeightKg    float64
	Reps        int
	CompletedAt int64
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

	db.Exec(`CREATE TABLE IF NOT EXISTS workout_plans (
        id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, days_of_week TEXT, sort_order INT);`)

	db.Exec(`CREATE TABLE IF NOT EXISTS workout_exercises (
        id INTEGER PRIMARY KEY AUTOINCREMENT, plan_id INT, name TEXT, target_sets INT, rest_sec INT, sort_order INT);`)

	// [MỚI] Tự động nâng cấp CSDL nếu đang dùng bản cũ (SQLite sẽ bỏ qua lỗi nếu cột đã tồn tại)
	db.Exec(`ALTER TABLE workout_exercises ADD COLUMN target_weight REAL DEFAULT 0;`)
	db.Exec(`ALTER TABLE workout_exercises ADD COLUMN target_reps INT DEFAULT 0;`)

	db.Exec(`CREATE TABLE IF NOT EXISTS workout_logs (
        id INTEGER PRIMARY KEY AUTOINCREMENT, exercise_id INT, date_str TEXT, set_index INT, 
        weight_kg REAL, reps INT, completed_at INT, 
        UNIQUE(exercise_id, date_str, set_index));`)
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
	if buf == "" { return fallback }
	buf = strings.ReplaceAll(buf, " ", ":")
	parts := strings.Split(buf, ":")
	if len(parts) >= 2 {
		h, _ := strconv.Atoi(parts[0])
		m, _ := strconv.Atoi(parts[1])
		return h*60 + m
	}
	val, err := strconv.Atoi(buf)
	if err == nil {
		if val <= 24 && len(buf) <= 2 { return val * 60 }
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
	if t.ID == 0 { return }
	db.Exec(`INSERT INTO instances (series_id, date_str, status, last_start, elapsed_sec, is_notified) VALUES (?, ?, ?, ?, ?, ?) 
        ON CONFLICT(series_id, date_str) DO UPDATE SET status=excluded.status, last_start=excluded.last_start, elapsed_sec=excluded.elapsed_sec, is_notified=excluded.is_notified`,
		t.ID, dateStr, t.Status, t.LastStartTimestamp, t.ElapsedSec, t.IsNotified)
}

func insertWorkoutPlan(p *WorkoutPlan) {
	res, _ := db.Exec(`INSERT INTO workout_plans (name, days_of_week, sort_order) VALUES (?, ?, ?)`,
		p.Name, p.DaysOfWeek, p.SortOrder)
	if res != nil {
		id, _ := res.LastInsertId()
		p.ID = int(id)
	}
}

func updateWorkoutPlan(p WorkoutPlan) {
	db.Exec(`UPDATE workout_plans SET name=?, days_of_week=?, sort_order=? WHERE id=?`,
		p.Name, p.DaysOfWeek, p.SortOrder, p.ID)
}

func deleteWorkoutPlan(id int) {
	db.Exec(`DELETE FROM workout_plans WHERE id=?`, id)
	db.Exec(`DELETE FROM workout_exercises WHERE plan_id=?`, id)
}

func insertWorkoutExercise(e *WorkoutExercise) {
	res, _ := db.Exec(`INSERT INTO workout_exercises (plan_id, name, target_sets, rest_sec, sort_order, target_weight, target_reps) VALUES (?, ?, ?, ?, ?, ?, ?)`,
		e.PlanID, e.Name, e.TargetSets, e.RestSec, e.SortOrder, e.TargetWeight, e.TargetReps)
	if res != nil {
		id, _ := res.LastInsertId()
		e.ID = int(id)
	}
}

func updateWorkoutExercise(e WorkoutExercise) {
	db.Exec(`UPDATE workout_exercises SET plan_id=?, name=?, target_sets=?, rest_sec=?, sort_order=?, target_weight=?, target_reps=? WHERE id=?`,
		e.PlanID, e.Name, e.TargetSets, e.RestSec, e.SortOrder, e.TargetWeight, e.TargetReps, e.ID)
}

func deleteWorkoutExercise(id int) {
	db.Exec(`DELETE FROM workout_exercises WHERE id=?`, id)
}

func saveWorkoutLog(l *WorkoutLog) {
	res, _ := db.Exec(`INSERT INTO workout_logs (exercise_id, date_str, set_index, weight_kg, reps, completed_at) 
        VALUES (?, ?, ?, ?, ?, ?) 
        ON CONFLICT(exercise_id, date_str, set_index) DO UPDATE SET weight_kg=excluded.weight_kg, reps=excluded.reps, completed_at=excluded.completed_at`,
		l.ExerciseID, l.DateStr, l.SetIndex, l.WeightKg, l.Reps, l.CompletedAt)
	if res != nil && l.ID == 0 {
		id, _ := res.LastInsertId()
		l.ID = int(id)
	}
}

func deleteWorkoutLog(exerciseID int, dateStr string, setIndex int) {
	db.Exec(`DELETE FROM workout_logs WHERE exercise_id=? AND date_str=? AND set_index=?`, exerciseID, dateStr, setIndex)
}

func deleteLastWorkoutLog(exerciseID int, dateStr string) {
	var maxSet int
	err := db.QueryRow(`SELECT COALESCE(MAX(set_index), 0) FROM workout_logs WHERE exercise_id=? AND date_str=?`, exerciseID, dateStr).Scan(&maxSet)
	if err == nil && maxSet > 0 {
		db.Exec(`DELETE FROM workout_logs WHERE exercise_id=? AND date_str=? AND set_index=?`, exerciseID, dateStr, maxSet)
	}
}

func resetWorkoutLogs(exerciseID int, dateStr string) {
	db.Exec(`DELETE FROM workout_logs WHERE exercise_id=? AND date_str=?`, exerciseID, dateStr)
}
