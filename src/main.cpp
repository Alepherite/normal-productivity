#include <iostream>
#include <string>
#include <array>
#include <unistd.h>
#include <cstdlib>
#include <ctime>
#include <vector>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <sys/stat.h>
#include <map>
#include <clocale>
#include <cstdio> 

#include <ncurses.h>
#include <sqlite3.h>

#define CP_NORMAL   1
#define CP_SELECTED 2
#define CP_RED      3
#define CP_GREEN    4
#define CP_GRAY     5
#define CP_MAGENTA  6
#define CP_CYAN     7
#define CP_BLUE     8

enum class Page { MAIN_MENU, SCHEDULE, POMODORO, POMODORO_RUN, INFO, TASK_EDIT };

const int MENU_SIZE = 2;
const std::array<std::string, MENU_SIZE> MENU_ITEMS = { "Schedule", "Info" };

struct Task {
    int id = 0; 
    std::string name = "New Task";
    std::string desc = "";
    bool has_start = true;
    int start_min = 480; 
    int duration_min = 60; 
    bool has_custom_deadline = false;
    int deadline_min = 540; 
    int status = 0; // 0: Todo, 1: Running, 2: Paused, 3: Done, 4: Skipped
    
    long long last_start_timestamp = 0;
    long long elapsed_sec = 0; 
    bool is_notified = false;

    // Recurring Properties
    int repeat_type = 0;         // 0: None, 1: Interval, 2: Weekly, 3: After Done
    std::string repeat_val = ""; 
    std::string created_date = ""; 
    std::string until_date = "";   
};

// Global State
sqlite3* db;
std::map<std::string, std::vector<Task>> tasks_db; 
std::string selected_date_str = "";
int task_sel_idx = 0;
int edit_sel_idx = 0;
bool is_insert_mode = false;
std::string input_buffer = "";
Task original_task_state;

// Pomodoro State
std::string pomo_task_date = "";
int pomo_task_idx = -1;
int pomo_task_id = -1; // Anchor ID against vector shift
int session_time_elapsed = 0;
bool is_eye_break_active = false;
int eye_break_remaining = 0;
bool is_pomo_active = false; 
bool is_pomo_paused = false;
long long pomo_last_tick = 0; 

// ---------------------------------------------------------
void init_ncurses() {
    setlocale(LC_ALL, ""); 
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE); 
    curs_set(0);          
    
    start_color();
    use_default_colors(); 

    init_pair(CP_NORMAL, -1, -1);
    init_pair(CP_SELECTED, COLOR_BLACK, COLOR_WHITE);
    init_pair(CP_RED, COLOR_RED, -1);
    init_pair(CP_GREEN, COLOR_GREEN, -1);
    init_pair(CP_GRAY, COLOR_BLACK, -1); 
    init_pair(CP_MAGENTA, COLOR_MAGENTA, -1);
    init_pair(CP_CYAN, COLOR_CYAN, -1);
    init_pair(CP_BLUE, COLOR_BLUE, -1);
}

void cleanup_ncurses() { 
    endwin(); 
    if (db) sqlite3_close(db); 
}

// ---------------------------------------------------------
// Core Helpers
std::string sanitize_for_shell(const std::string& input) {
    std::string safe = input;
    size_t pos = 0;
    while ((pos = safe.find('\'', pos)) != std::string::npos) {
        safe.replace(pos, 1, "'\\''");
        pos += 4;
    }
    return safe;
}

bool has_task_changed(const Task& a, const Task& b) {
    return a.name != b.name || a.desc != b.desc || a.has_start != b.has_start ||
           a.start_min != b.start_min || a.duration_min != b.duration_min ||
           a.has_custom_deadline != b.has_custom_deadline || a.deadline_min != b.deadline_min ||
           a.repeat_type != b.repeat_type || a.repeat_val != b.repeat_val;
}

std::string format_time_24(int total_mins) {
    int h = (total_mins / 60) % 24;
    int m = total_mins % 60;
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
    return std::string(buf);
}

std::string format_date(const std::tm& date) {
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &date);
    return std::string(buf);
}

std::tm string_to_tm(const std::string& date_str) {
    std::tm tm = {};
    sscanf(date_str.c_str(), "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday);
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_hour = 12; 
    tm.tm_isdst = -1;
    std::mktime(&tm);
    return tm;
}

int parse_smart_time(const std::string& buf, int fallback) {
    if (buf.empty()) return fallback;
    int h = 0, m = 0;
    if (buf.find(':') != std::string::npos) {
        sscanf(buf.c_str(), "%d:%d", &h, &m);
        return h * 60 + m;
    } else if (buf.find(' ') != std::string::npos) {
        sscanf(buf.c_str(), "%d %d", &h, &m);
        return h * 60 + m;
    } else {
        try { 
            int val = std::stoi(buf); 
            if (val <= 24 && buf.length() <= 2) return val * 60;
            return val; 
        } 
        catch(...) { return fallback; }
    }
}

std::tm add_days(std::tm date, int days) {
    date.tm_mday += days; std::mktime(&date); return date;
}

bool is_same_day(const std::tm& d1, const std::tm& d2) { return (d1.tm_year == d2.tm_year && d1.tm_yday == d2.tm_yday); }

long long get_elapsed_sec(const Task& t) {
    long long total = t.elapsed_sec;
    // Continuous time logic: freely accrue elapsed time based on last timestamp
    if (t.status == 1 && t.last_start_timestamp > 0) {
        long long now = std::time(nullptr);
        if (now > t.last_start_timestamp) total += (now - t.last_start_timestamp);
    }
    return total;
}

// ---------------------------------------------------------
// SQLite Integration
void init_db() {
    std::string path = std::string(getenv("HOME")) + "/.local/normal-productivity";
    mkdir(path.c_str(), 0777);
    std::string db_path = path + "/tasks.db";
    sqlite3_open(db_path.c_str(), &db);

    const char* sql_series = 
        "CREATE TABLE IF NOT EXISTS series ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, desc TEXT, has_start INT, "
        "start_min INT, duration_min INT, has_custom_deadline INT, deadline_min INT, "
        "repeat_type INT, repeat_val TEXT, created_date TEXT, until_date TEXT);";
    
    const char* sql_instances = 
        "CREATE TABLE IF NOT EXISTS instances ("
        "series_id INT, date_str TEXT, status INT, last_start INT, elapsed_sec INT, is_notified INT, "
        "PRIMARY KEY(series_id, date_str));";

    sqlite3_exec(db, sql_series, 0, 0, 0);
    sqlite3_exec(db, sql_instances, 0, 0, 0);
}

void insert_series(Task& t) {
    std::string sql = "INSERT INTO series (name, desc, has_start, start_min, duration_min, has_custom_deadline, deadline_min, repeat_type, repeat_val, created_date, until_date) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, 0);
    sqlite3_bind_text(stmt, 1, t.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, t.desc.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, t.has_start);
    sqlite3_bind_int(stmt, 4, t.start_min);
    sqlite3_bind_int(stmt, 5, t.duration_min);
    sqlite3_bind_int(stmt, 6, t.has_custom_deadline);
    sqlite3_bind_int(stmt, 7, t.deadline_min);
    sqlite3_bind_int(stmt, 8, t.repeat_type);
    sqlite3_bind_text(stmt, 9, t.repeat_val.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, t.created_date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 11, t.until_date.c_str(), -1, SQLITE_TRANSIENT);
    
    sqlite3_step(stmt);
    t.id = sqlite3_last_insert_rowid(db); 
    sqlite3_finalize(stmt);
}

void update_series(const Task& t) {
    std::string sql = "UPDATE series SET name=?, desc=?, has_start=?, start_min=?, duration_min=?, has_custom_deadline=?, deadline_min=?, repeat_type=?, repeat_val=?, until_date=? WHERE id=?;";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, 0);
    sqlite3_bind_text(stmt, 1, t.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, t.desc.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, t.has_start);
    sqlite3_bind_int(stmt, 4, t.start_min);
    sqlite3_bind_int(stmt, 5, t.duration_min);
    sqlite3_bind_int(stmt, 6, t.has_custom_deadline);
    sqlite3_bind_int(stmt, 7, t.deadline_min);
    sqlite3_bind_int(stmt, 8, t.repeat_type);
    sqlite3_bind_text(stmt, 9, t.repeat_val.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, t.until_date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 11, t.id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void save_instance(const Task& t, const std::string& date) {
    if (t.id == 0) return; 
    std::string sql = "INSERT INTO instances (series_id, date_str, status, last_start, elapsed_sec, is_notified) VALUES (?, ?, ?, ?, ?, ?) "
                      "ON CONFLICT(series_id, date_str) DO UPDATE SET "
                      "status=excluded.status, last_start=excluded.last_start, elapsed_sec=excluded.elapsed_sec, is_notified=excluded.is_notified;";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, 0);
    sqlite3_bind_int(stmt, 1, t.id);
    sqlite3_bind_text(stmt, 2, date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, t.status);
    sqlite3_bind_int64(stmt, 4, t.last_start_timestamp);
    sqlite3_bind_int64(stmt, 5, t.elapsed_sec);
    sqlite3_bind_int(stmt, 6, t.is_notified ? 1 : 0);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

// Virtual Task Projection
void load_tasks(const std::tm& focus_date) {
    tasks_db.clear();
    
    // Performance limit: Only project tasks within a -30 to +60 days window
    std::tm window_start = add_days(focus_date, -30);
    std::tm window_end = add_days(focus_date, 60);
    std::string limit_future = format_date(window_end); 
    std::string limit_past = format_date(window_start);
    
    const char* q_series = "SELECT * FROM series;";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, q_series, -1, &stmt, 0);
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Task tmpl;
        tmpl.id = sqlite3_column_int(stmt, 0);
        tmpl.name = (const char*)sqlite3_column_text(stmt, 1);
        tmpl.desc = (const char*)sqlite3_column_text(stmt, 2);
        tmpl.has_start = sqlite3_column_int(stmt, 3);
        tmpl.start_min = sqlite3_column_int(stmt, 4);
        tmpl.duration_min = sqlite3_column_int(stmt, 5);
        tmpl.has_custom_deadline = sqlite3_column_int(stmt, 6);
        tmpl.deadline_min = sqlite3_column_int(stmt, 7);
        tmpl.repeat_type = sqlite3_column_int(stmt, 8);
        tmpl.repeat_val = (const char*)sqlite3_column_text(stmt, 9);
        tmpl.created_date = (const char*)sqlite3_column_text(stmt, 10);
        const char* ud = (const char*)sqlite3_column_text(stmt, 11);
        tmpl.until_date = ud ? std::string(ud) : "";

        std::string end_limit = (tmpl.until_date.empty() || tmpl.until_date > limit_future) ? limit_future : tmpl.until_date;
        std::tm curr_tm = string_to_tm(tmpl.created_date);

        int interval = 1; std::vector<int> week_days;
        if (tmpl.repeat_type == 1) {
            try { interval = std::stoi(tmpl.repeat_val); } catch(...) { interval = 1; }
            if (interval <= 0) interval = 1;
        } else if (tmpl.repeat_type == 2) {
            std::string s = tmpl.repeat_val.empty() ? std::to_string(curr_tm.tm_wday) : tmpl.repeat_val;
            size_t pos = 0;
            while ((pos = s.find(',')) != std::string::npos) {
                try { week_days.push_back(std::stoi(s.substr(0, pos))); } catch(...) {}
                s.erase(0, pos + 1);
            }
            if (!s.empty()) { try { week_days.push_back(std::stoi(s)); } catch(...) {} }
            if (week_days.empty()) week_days.push_back(curr_tm.tm_wday);
        }

        // Project memory shadows: Fast-forward logic to avoid excessive mktime calls
        int start_offset = 0;
        if (tmpl.created_date < limit_past) {
            std::tm tm_curr_copy = curr_tm;
            std::time_t t_curr = std::mktime(&tm_curr_copy);
            std::tm tm_win_start = window_start;
            std::time_t t_win = std::mktime(&tm_win_start);
            start_offset = (t_win - t_curr) / 86400;
            if (start_offset < 0) start_offset = 0;
        }

        // Limit iteration strictly to the 90-day projection window (+ padding)
        for (int i = start_offset; i <= start_offset + 95; ++i) {
            std::tm next_tm = add_days(curr_tm, i);
            std::string next_date = format_date(next_tm);
            if (next_date > end_limit) break;
            if (next_date < limit_past) continue; 

            bool should_spawn = false;
            if (tmpl.repeat_type == 0 || tmpl.repeat_type == 3) { 
                should_spawn = (i == 0);
            } else if (tmpl.repeat_type == 1 && (i % interval == 0)) {
                should_spawn = true;
            } else if (tmpl.repeat_type == 2) {
                for (int wd : week_days) {
                    if (next_tm.tm_wday == wd) { should_spawn = true; break; }
                }
            }

            if (should_spawn) tasks_db[next_date].push_back(tmpl);
            if (tmpl.repeat_type == 0 || tmpl.repeat_type == 3) break; 
        }
    }
    sqlite3_finalize(stmt);

    // Load state overrides (Exceptions)
    const char* q_inst = "SELECT * FROM instances;";
    sqlite3_prepare_v2(db, q_inst, -1, &stmt, 0);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int s_id = sqlite3_column_int(stmt, 0);
        std::string d_str = (const char*)sqlite3_column_text(stmt, 1);
        int status = sqlite3_column_int(stmt, 2);
        long long last_start = sqlite3_column_int64(stmt, 3);
        long long elapsed = sqlite3_column_int64(stmt, 4);
        bool notified = sqlite3_column_int(stmt, 5);

        for (auto& t : tasks_db[d_str]) {
            if (t.id == s_id) {
                t.status = status;
                t.last_start_timestamp = last_start;
                t.elapsed_sec = elapsed;
                t.is_notified = notified;
                break;
            }
        }
    }
    sqlite3_finalize(stmt);

    // Anchor check against Vector Shift
    if (is_pomo_active && !pomo_task_date.empty()) {
        bool found = false;
        auto& tasks = tasks_db[pomo_task_date];
        for (size_t i = 0; i < tasks.size(); ++i) {
            if (tasks[i].id == pomo_task_id) {
                pomo_task_idx = i; found = true; break;
            }
        }
        if (!found) is_pomo_active = false; 
    }
}

// ---------------------------------------------------------
// UI Rendering
std::string center_text(const std::string& text, int width) {
    int pad_left = std::max(0, (width - (int)text.length()) / 2);
    int pad_right = std::max(0, width - (int)text.length() - pad_left);
    return std::string(pad_left, ' ') + text + std::string(pad_right, ' ');
}

void draw_box_ncurses(int start_y, int start_x, const std::string& text, int width, bool selected) {
    if (selected) attron(COLOR_PAIR(CP_SELECTED));
    int p_l = std::max(0, (width - (int)text.length()) / 2);
    int p_r = std::max(0, width - (int)text.length() - p_l);
    std::string h_line = ""; for (int i = 0; i < width; ++i) h_line += "─"; 
    mvprintw(start_y, start_x, "┌%s┐", h_line.c_str());
    mvprintw(start_y + 1, start_x, "│%*s%s%*s│", p_l, "", text.c_str(), p_r, "");
    mvprintw(start_y + 2, start_x, "└%s┘", h_line.c_str());
    if (selected) attroff(COLOR_PAIR(CP_SELECTED));
}

void render_ui(Page current_page, int sel_idx, int sched_sel_c, const std::tm& current_week_sun, const std::tm& today, 
               int work_time, int break_time, bool eye_break_enabled, int time_remaining, bool is_work_phase, bool is_task_focused) {
    erase(); 
    std::string today_str = format_date(today);
    int now_min = today.tm_hour * 60 + today.tm_min;

    if (current_page == Page::MAIN_MENU) {
        int menu_width = 24; int total_height = MENU_SIZE * 4 - 1;
        int start_y = std::max(0, (LINES - total_height) / 2); int start_x = std::max(0, (COLS - menu_width - 2) / 2);
        for (int r = 0; r < MENU_SIZE; ++r) {
            draw_box_ncurses(start_y + (r * 4), start_x, MENU_ITEMS[r], menu_width, (r == sel_idx));
        }
    } 
    else if (current_page == Page::SCHEDULE) {
        int cell_w = 9; int sched_width = 7 * cell_w + 8;
        int start_y = std::max(0, (LINES - 22) / 2); int start_x = std::max(0, (COLS - sched_width) / 2);

        mvaddstr(start_y, start_x, "┌─────────┬─────────┬─────────┬─────────┬─────────┬─────────┬─────────┐");
        mvaddstr(start_y + 1, start_x, "│"); mvaddstr(start_y + 2, start_x, "│"); mvaddstr(start_y + 3, start_x, "│");

        const std::array<std::string, 7> day_names = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        int cur_x = start_x + 1;
        
        for (int c = 0; c < 7; ++c) {
            std::tm day = add_days(current_week_sun, c);
            bool is_today = is_same_day(day, today);
            bool is_selected = (c == sched_sel_c);

            char date_buf[16]; std::strftime(date_buf, sizeof(date_buf), "%d/%m", &day);
            std::string day_key = format_date(day);
            int t_cnt = tasks_db[day_key].size();
            
            std::string name_str = center_text(day_names[c], cell_w);
            std::string date_str = center_text(std::string(date_buf), cell_w);
            std::string cnt_str  = t_cnt > 0 ? center_text("[" + std::to_string(t_cnt) + "]", cell_w) : center_text("", cell_w);

            int color = is_today ? CP_RED : CP_NORMAL;
            if (is_selected) attron(A_REVERSE);
            attron(COLOR_PAIR(color));
            mvprintw(start_y + 1, cur_x, "%s", name_str.c_str());
            mvprintw(start_y + 2, cur_x, "%s", date_str.c_str());
            mvprintw(start_y + 3, cur_x, "%s", cnt_str.c_str());
            attroff(COLOR_PAIR(color));
            if (is_selected) attroff(A_REVERSE);
            
            mvaddstr(start_y + 1, cur_x + cell_w, "│"); mvaddstr(start_y + 2, cur_x + cell_w, "│"); mvaddstr(start_y + 3, cur_x + cell_w, "│");
            cur_x += cell_w + 1;
        }
        mvaddstr(start_y + 4, start_x, "└─────────┴─────────┴─────────┴─────────┴─────────┴─────────┴─────────┘");
        
        attron(A_BOLD); mvprintw(start_y + 6, start_x, "Tasks for %s", selected_date_str.c_str()); attroff(A_BOLD);

        int list_y = start_y + 8;
        auto& tasks = tasks_db[selected_date_str];
        
        if (tasks.empty()) {
            attron(COLOR_PAIR(CP_GRAY)); mvprintw(list_y, start_x, "< No tasks for this day >"); attroff(COLOR_PAIR(CP_GRAY));
        } else {
            for (size_t i = 0; i < tasks.size(); ++i) {
                auto& t = tasks[i];
                bool selected = (is_task_focused && (int)i == task_sel_idx);
                std::string prefix = selected ? "> " : "  ";
                
                int c_pair = CP_NORMAL; bool overdue = false;
                int dl = t.has_custom_deadline ? t.deadline_min : (t.start_min + t.duration_min);
                if (selected_date_str < today_str || (selected_date_str == today_str && now_min >= dl)) overdue = true;

                if (t.status == 3 || t.status == 4) c_pair = CP_GRAY; 
                else if (!t.has_start) c_pair = CP_NORMAL; 
                else if (t.status == 1) c_pair = overdue ? CP_MAGENTA : CP_GREEN; 
                else c_pair = overdue ? CP_RED : CP_BLUE; 

                long long elapsed = get_elapsed_sec(t);
                double pct = t.duration_min > 0 ? ((double)elapsed / (t.duration_min * 60.0)) * 100.0 : 0.0;
                char pct_buf[16]; snprintf(pct_buf, sizeof(pct_buf), "%5.1f%%", pct);

                std::string time_str = t.has_start ? format_time_24(t.start_min) : "     ";
                std::string status_sym = (t.status == 1) ? "[>]" : (t.status == 2) ? "[||]" : (t.status == 3) ? "[v]" : (t.status == 4) ? "[S]" : "[ ]";
                std::string t_name = t.name + (t.status == 4 ? " (Skipped)" : "");
                
                bool is_pomo_attached = (is_pomo_active && pomo_task_date == selected_date_str && pomo_task_idx == (int)i);
                std::string sep = is_pomo_attached ? " P " : " │ ";
                std::string row = prefix + status_sym + " " + time_str + sep + t_name + " [" + std::string(pct_buf) + "]";

                if (selected) attron(A_REVERSE);
                attron(COLOR_PAIR(c_pair)); mvprintw(list_y + i, start_x, "%s", row.c_str()); attroff(COLOR_PAIR(c_pair));
                if (selected) attroff(A_REVERSE);
            }
        }

        attron(A_DIM);
        if (is_task_focused) {
            mvprintw(list_y + std::max((int)tasks.size(), 1) + 2, start_x, "[j/k] Move  [a] Add  [s] Start/Pause  [d] Done  [D] Skip/Del  [X] Del Series  [Enter] Edit  [q] Back");
        } else {
            mvprintw(list_y + std::max((int)tasks.size(), 1) + 2, start_x, "[h/l] Change Day  [t] Today  [Enter] Focus Tasks  [a] Add Task  [q] Menu");
        }
        attroff(A_DIM);
    }
    else if (current_page == Page::TASK_EDIT) {
        int w = 54; int start_y = std::max(0, (LINES - 15) / 2); int start_x = std::max(0, (COLS - w) / 2);
        int cursor_y = 0, cursor_x = 0;
        Task& t = tasks_db[selected_date_str][task_sel_idx];
        
        attron(A_BOLD); mvprintw(start_y, start_x, "=== Edit Task ==="); attroff(A_BOLD);

        auto render_field = [&](int idx, std::string label, std::string val, int y_offset) {
            if (edit_sel_idx == idx) attron(A_REVERSE);
            mvprintw(start_y + y_offset, start_x, "%s: ", label.c_str());
            if (edit_sel_idx == idx && is_insert_mode) {
                printw("[%s ]", input_buffer.c_str());
                getyx(stdscr, cursor_y, cursor_x); cursor_x--; 
            } else { printw("%s", val.c_str()); }
            if (edit_sel_idx == idx) attroff(A_REVERSE);
        };

        render_field(0, "Name           ", t.name, 2);
        render_field(1, "Description    ", t.desc, 3);
        render_field(2, "Has Start Time ", t.has_start ? "Yes" : "No (No deadline)", 4);
        
        if (t.has_start) render_field(3, "Start Time     ", format_time_24(t.start_min), 5);
        else {
            attron(COLOR_PAIR(CP_GRAY)); mvprintw(start_y + 5, start_x, "Start Time     : --:--"); attroff(COLOR_PAIR(CP_GRAY));
        }
        
        render_field(4, "Duration       ", format_time_24(t.duration_min), 6);
        
        if (t.has_start) {
            render_field(5, "Custom Deadline", t.has_custom_deadline ? "Yes" : "No (Auto: Start+Dur)", 7);
            if (t.has_custom_deadline) render_field(6, "Deadline Time  ", format_time_24(t.deadline_min), 8);
            else {
                attron(COLOR_PAIR(CP_GRAY)); mvprintw(start_y + 8, start_x, "Deadline Time  : %s", format_time_24(t.start_min + t.duration_min).c_str()); attroff(COLOR_PAIR(CP_GRAY));
            }
        }

        int y_curr = 10;
        int idx_repeat_type = t.has_start ? (t.has_custom_deadline ? 7 : 6) : 5;
        
        std::string r_type_str = "None";
        if (t.repeat_type == 1) r_type_str = "Interval (Days)";
        else if (t.repeat_type == 2) r_type_str = "Weekly (0=Sun,1=Mon...)";
        else if (t.repeat_type == 3) r_type_str = "After Done (Days)";
        
        render_field(idx_repeat_type, "Repeat Type    ", "[ " + r_type_str + " ]", y_curr++);
        
        if (t.repeat_type > 0) {
            std::string fallback = (t.repeat_type == 2) ? "e.g. 1,3,5" : "e.g. 1";
            std::string val_disp = t.repeat_val.empty() ? fallback : t.repeat_val;
            render_field(idx_repeat_type + 1, "Repeat Value   ", val_disp, y_curr++);
        }
        y_curr++;

        long long elapsed = get_elapsed_sec(t);
        int eh = elapsed / 3600; int em = (elapsed % 3600) / 60; int es = elapsed % 60;
        attron(COLOR_PAIR(CP_GRAY)); mvprintw(start_y + y_curr++, start_x, "Elapsed Time   : %02d:%02d:%02d", eh, em, es); attroff(COLOR_PAIR(CP_GRAY));

        attron(A_DIM); mvprintw(start_y + y_curr + 1, start_x, "[i/Enter] Edit  [Esc] Normal Mode  [h/l] +/-  [q] Save"); attroff(A_DIM);
        if (is_insert_mode) { move(cursor_y, cursor_x); curs_set(1); }
    }
    else if (current_page == Page::POMODORO) {
        int menu_width = 30; int total_height = 4 * 4 - 1;
        int start_y = std::max(0, (LINES - total_height) / 2); int start_x = std::max(0, (COLS - menu_width - 2) / 2);
        std::string task_name = tasks_db[pomo_task_date][pomo_task_idx].name;
        if (task_name.length() > 20) task_name = task_name.substr(0, 17) + "...";
        
        attron(A_BOLD); mvprintw(start_y - 2, start_x, "Task: %s", task_name.c_str()); attroff(A_BOLD);
        std::array<std::string, 4> pomo_items = { "Start Timer", "Work: < " + std::to_string(work_time) + " > min", "Break: < " + std::to_string(break_time) + " > min", "Eye Break: < " + std::string(eye_break_enabled ? "ON" : "OFF") + " >" };
        for (int i = 0; i < 4; ++i) draw_box_ncurses(start_y + (i * 4), start_x, pomo_items[i], menu_width, (i == sel_idx));
        
        attron(A_DIM); mvprintw(start_y + (4 * 4), start_x, "[q] Back to Schedule"); attroff(A_DIM);
    }
    else if (current_page == Page::POMODORO_RUN) {
        int bar_w = 40; int block_w = bar_w + 2; int total_height = 8;
        int start_y = std::max(0, (LINES - total_height) / 2); int start_x = std::max(0, (COLS - block_w) / 2);

        if (is_eye_break_active) {
            std::string msg1 = "EYE BREAK: LOOK 20 FEET AWAY"; std::string msg2 = "Resuming in " + std::to_string(eye_break_remaining) + "s";
            attron(COLOR_PAIR(CP_CYAN) | A_BOLD); mvprintw(start_y + 2, start_x + (block_w - msg1.length()) / 2, "%s", msg1.c_str()); attroff(COLOR_PAIR(CP_CYAN));
            mvprintw(start_y + 4, start_x + (block_w - msg2.length()) / 2, "%s", msg2.c_str()); attroff(A_BOLD);
        } else {
            int total_sec = std::max(1, (is_work_phase ? work_time : break_time) * 60); 
            float progress = 1.0f - ((float)time_remaining / total_sec); 
            int filled_chars = std::max(0, std::min(bar_w, (int)(progress * bar_w))); 
            std::string phase_txt = is_work_phase ? "WORK PHASE" : "BREAK PHASE";
            if (is_pomo_paused) phase_txt += " (PAUSED)";
            
            int color = is_work_phase ? CP_RED : CP_GREEN; 
            int h = std::abs(time_remaining) / 3600; int m = (std::abs(time_remaining) % 3600) / 60; int s = std::abs(time_remaining) % 60;
            
            attron(COLOR_PAIR(color) | A_BOLD); mvprintw(start_y, start_x + (block_w - phase_txt.length()) / 2, "%s", phase_txt.c_str()); attroff(COLOR_PAIR(color));
            mvprintw(start_y + 1, start_x + (block_w - 8) / 2, "%02d:%02d:%02d", h, m, s); attroff(A_BOLD);

            std::string h_line = ""; for (int i = 0; i < bar_w; ++i) h_line += "─";
            mvprintw(start_y + 3, start_x, "┌%s┐", h_line.c_str());
            mvaddstr(start_y + 4, start_x, "│");
            attron(COLOR_PAIR(color) | A_REVERSE); for (int i = 0; i < filled_chars; ++i) addch(' '); attroff(A_REVERSE);
            for (int i = filled_chars; i < bar_w; ++i) addch(' '); attroff(COLOR_PAIR(color)); addstr("│");
            mvprintw(start_y + 5, start_x, "└%s┘", h_line.c_str());

            attron(A_DIM); std::string msg = "[s] Pause/Resume  [c] Cancel  [q] Background";
            mvprintw(start_y + 7, start_x + (block_w - msg.length()) / 2, "%s", msg.c_str()); attroff(A_DIM);
        }
    }
    else if (current_page == Page::INFO) {
        int info_w = 64; int info_h = 24;
        int start_y = std::max(0, (LINES - info_h) / 2); int start_x = std::max(0, (COLS - info_w) / 2);

        std::string h_line = ""; for (int i = 0; i < info_w; ++i) h_line += "─";
        mvprintw(start_y, start_x, "┌%s┐", h_line.c_str());
        for (int i = 1; i < info_h; ++i) mvprintw(start_y + i, start_x, "│%*s│", info_w, "");
        mvprintw(start_y + info_h, start_x, "└%s┘", h_line.c_str());

        attron(A_BOLD | COLOR_PAIR(CP_SELECTED));
        mvprintw(start_y + 1, start_x + (info_w - 20) / 2, " MANUAL & KEYBINDS ");
        attroff(A_BOLD | COLOR_PAIR(CP_SELECTED));

        int cur_y = start_y + 3;
        auto print_section = [&](const std::string& title) {
            attron(A_BOLD | COLOR_PAIR(CP_CYAN)); mvprintw(cur_y++, start_x + 3, "%s", title.c_str()); attroff(A_BOLD | COLOR_PAIR(CP_CYAN));
        };
        auto print_key = [&](const std::string& keys, const std::string& desc) {
            attron(COLOR_PAIR(CP_MAGENTA)); mvprintw(cur_y, start_x + 5, "%-16s", keys.c_str()); attroff(COLOR_PAIR(CP_MAGENTA));
            mvprintw(cur_y++, start_x + 21, ": %s", desc.c_str());
        };

        print_section("[ Navigation & General ]");
        print_key("j / k", "Move Up / Down in lists");
        print_key("Enter / Space", "Select / Confirm / Focus");
        print_key("q / ESC", "Go Back / Exit menu / Quit app"); cur_y++;

        print_section("[ Schedule & Task List ]");
        print_key("h / l", "Change Day (Prev / Next)");
        print_key("t", "Jump to Today");
        print_key("a", "Add a new task");
        print_key("s", "Start / Pause focused task");
        print_key("d / D", "Mark Done / Skip (Delete)");
        print_key("X", "Delete entire recurring series");
        print_key("p", "Attach Pomodoro timer to task"); cur_y++;

        print_section("[ Task Editor & Pomodoro ]");
        print_key("i / Enter", "Edit text field / Toggle options");
        print_key("h/H or l/L", "Decrease / Increase time values");
        print_key("s / c (Pomo)", "Pause / Cancel Pomodoro session");
        print_key("q (Pomo)", "Run Pomodoro in background"); 

        attron(A_DIM); std::string footer = "Press [q] or [ESC] to return";
        mvprintw(start_y + info_h - 1, start_x + (info_w - footer.length()) / 2, "%s", footer.c_str()); 
        attroff(A_DIM);
    }
    
    if (!(current_page == Page::TASK_EDIT && is_insert_mode)) curs_set(0);
    refresh(); 
}

// ---------------------------------------------------------
int main() {
    init_ncurses();
    init_db();
    std::atexit(cleanup_ncurses);
    
    std::time_t init_t = std::time(nullptr);
    std::tm init_tm = *std::localtime(&init_t);
    std::tm current_week_sun = add_days(init_tm, -init_tm.tm_wday);
    
    load_tasks(current_week_sun); 

    Page current_page = Page::MAIN_MENU;
    int main_sel_r = 0, pomo_sel = 0;
    int sched_sel_c = init_tm.tm_wday; 
    bool is_task_focused = false;   

    int work_time = 25, break_time = 5;
    bool eye_break_enabled = true;
    int time_remaining = 0;
    bool is_work_phase = true;
    
    bool needs_redraw = true;
    int last_min = -1;

    while (true) {
        std::time_t current_unix = std::time(nullptr);
        std::tm* now = std::localtime(&current_unix);
        std::string today_str = format_date(*now);
        int now_min = now->tm_hour * 60 + now->tm_min;

        if (now_min != last_min) {
            last_min = now_min;
            needs_redraw = true; 
            
            std::vector<std::string> check_dates = { format_date(add_days(*now, -1)), today_str };
            for (const auto& d_key : check_dates) {
                if (tasks_db.count(d_key)) {
                    for (auto& task : tasks_db[d_key]) {
                        if (task.has_start && task.status == 0 && !task.is_notified) {
                            std::tm date_tm = string_to_tm(d_key);
                            date_tm.tm_hour = 0; date_tm.tm_min = 0; date_tm.tm_sec = 0;
                            long long task_epoch = std::mktime(&date_tm) + task.start_min * 60;
                            
                            if (current_unix >= task_epoch && current_unix - task_epoch < 7200) {
                                task.is_notified = true;
                                save_instance(task, d_key);
                                std::string safe_name = sanitize_for_shell(task.name);
                                std::string cmd = "notify-send -u critical 'Task Reminder' 'Time to work: " + safe_name + "' &";
                                system(cmd.c_str());
                            }
                        }
                    }
                }
            }
        }

        std::tm target_day = add_days(current_week_sun, sched_sel_c);
        selected_date_str = format_date(target_day);

        if (needs_redraw) {
            render_ui(current_page, (current_page == Page::MAIN_MENU) ? main_sel_r : (current_page == Page::POMODORO ? pomo_sel : task_sel_idx), 
                      sched_sel_c, current_week_sun, *now, work_time, break_time, eye_break_enabled, time_remaining, is_work_phase, is_task_focused);
            needs_redraw = false;
        }

        bool any_task_running = false;
        for (const auto& [d_key, t_list] : tasks_db) {
            for (const auto& tk : t_list) {
                if (tk.status == 1) { any_task_running = true; break; }
            }
            if (any_task_running) break;
        }

        int timeout_ms = -1;
        if (is_pomo_active || any_task_running) timeout_ms = 1000;
        else {
            timeout_ms = (60 - (current_unix % 60)) * 1000; 
            if (timeout_ms <= 0) timeout_ms = 60000; 
        }

        timeout(timeout_ms);
        int key = getch();

        if (key == ERR) {
            if (any_task_running) needs_redraw = true; 

            if (is_pomo_active && !is_pomo_paused) {
                int delta = current_unix - pomo_last_tick;
                
                if (delta > 0) {
                    time_remaining -= delta;
                    session_time_elapsed += delta;
                    pomo_last_tick = current_unix;
                    
                    // Eye break evaluation crossing 20m marks
                    if (eye_break_enabled) {
                        int prev_elapsed = session_time_elapsed - delta;
                        if (prev_elapsed >= 0 && (session_time_elapsed / 1200) > (prev_elapsed / 1200)) {
                            is_eye_break_active = true; eye_break_remaining = 20;
                            if (current_page != Page::POMODORO_RUN) current_page = Page::POMODORO_RUN;
                        }
                    }

                    if (is_eye_break_active) {
                        eye_break_remaining -= delta; 
                        if (eye_break_remaining <= 0) is_eye_break_active = false;
                    }

                    // Phase flip (continuous time handling implicitly flips once and dumps huge delta)
                    if (time_remaining <= 0) { 
                        is_work_phase = !is_work_phase;
                        time_remaining = (is_work_phase ? work_time : break_time) * 60;
                        session_time_elapsed = 0; is_eye_break_active = false;
                        
                        Task& t = tasks_db[pomo_task_date][pomo_task_idx];
                        if (is_work_phase) {
                            if (t.status != 1) { t.status = 1; t.last_start_timestamp = std::time(nullptr); }
                            system("notify-send -u critical 'Pomodoro' 'Work phase started!' &");
                        } else {
                            if (t.status == 1) {
                                t.status = 2; 
                                long long real_added = std::time(nullptr) - t.last_start_timestamp;
                                t.elapsed_sec += real_added;
                                t.last_start_timestamp = 0;
                            }
                            system("notify-send -u critical 'Pomodoro' 'Break phase started!' &");
                        }
                        
                        save_instance(t, pomo_task_date);
                        beep(); 
                    }
                    needs_redraw = true; 
                }
            } else {
                pomo_last_tick = current_unix; 
            }
            continue; 
        }

        if (key == KEY_RESIZE) { needs_redraw = true; continue; }

        needs_redraw = true; 
        if (key == 3) break; 

        if (current_page == Page::MAIN_MENU) {
            if (key == 'q' || key == 27) break;
            switch (key) {
                case 'k': case KEY_UP: if (main_sel_r > 0) main_sel_r--; break;
                case 'j': case KEY_DOWN: if (main_sel_r < MENU_SIZE - 1) main_sel_r++; break;
                case '\r': case '\n': case KEY_ENTER: case ' ':
                    if (main_sel_r == 0) { current_page = Page::SCHEDULE; is_task_focused = false; }
                    else if (main_sel_r == 1) current_page = Page::INFO;
                    break;
            }
        } 
        else if (current_page == Page::SCHEDULE) {
            if (!is_task_focused) { 
                if (key == 'q' || key == 27) current_page = Page::MAIN_MENU;
                else if (key == 'h' || key == KEY_LEFT) {
                    if (sched_sel_c > 0) sched_sel_c--;
                    else { 
                        sched_sel_c = 6; 
                        current_week_sun = add_days(current_week_sun, -7); 
                        load_tasks(current_week_sun);
                    }
                }
                else if (key == 'l' || key == KEY_RIGHT) {
                    if (sched_sel_c < 6) sched_sel_c++;
                    else { 
                        sched_sel_c = 0; 
                        current_week_sun = add_days(current_week_sun, 7); 
                        load_tasks(current_week_sun);
                    }
                }
                else if (key == 't') {
                    current_week_sun = add_days(*now, -now->tm_wday);
                    sched_sel_c = now->tm_wday;
                    load_tasks(current_week_sun);
                }
                else if (key == '\r' || key == '\n' || key == KEY_ENTER || key == ' ') {
                    if (!tasks_db[selected_date_str].empty()) { is_task_focused = true; task_sel_idx = 0; }
                }
                else if (key == 'a') {
                    Task new_t; new_t.id = 0; 
                    new_t.created_date = selected_date_str;
                    tasks_db[selected_date_str].push_back(new_t);
                    current_page = Page::TASK_EDIT; task_sel_idx = tasks_db[selected_date_str].size() - 1;
                    original_task_state = new_t;
                    edit_sel_idx = 0; is_insert_mode = true; input_buffer = "";
                }
            } else { 
                if (key == 'q' || key == 27) { is_task_focused = false; }
                else if (key == 'j' || key == KEY_DOWN) { if (task_sel_idx < (int)tasks_db[selected_date_str].size() - 1) task_sel_idx++; }
                else if (key == 'k' || key == KEY_UP) { if (task_sel_idx > 0) task_sel_idx--; }
                else if (key == 'a') {
                    Task new_t; new_t.id = 0; 
                    new_t.created_date = selected_date_str;
                    tasks_db[selected_date_str].push_back(new_t);
                    current_page = Page::TASK_EDIT; task_sel_idx = tasks_db[selected_date_str].size() - 1;
                    original_task_state = new_t;
                    edit_sel_idx = 0; is_insert_mode = true; input_buffer = "";
                }
                else if (key == '\r' || key == '\n' || key == KEY_ENTER) {
                    if (!tasks_db[selected_date_str].empty()) {
                        current_page = Page::TASK_EDIT;
                        original_task_state = tasks_db[selected_date_str][task_sel_idx];
                        edit_sel_idx = 0;
                    }
                }
                else if (key == 'p') { 
                    if (!tasks_db[selected_date_str].empty()) {
                        if (is_pomo_active && pomo_task_date == selected_date_str && pomo_task_idx == task_sel_idx) {
                            current_page = Page::POMODORO_RUN;
                        } else {
                            pomo_task_date = selected_date_str; pomo_task_idx = task_sel_idx;
                            pomo_task_id = tasks_db[selected_date_str][task_sel_idx].id; 
                            current_page = Page::POMODORO; pomo_sel = 0;
                        }
                    }
                }
                else if (key == 's') { 
                    if (!tasks_db[selected_date_str].empty()) {
                        Task& t = tasks_db[selected_date_str][task_sel_idx];
                        if (t.status == 0 || t.status == 2) {
                            // Tự động tạm dừng các task khác đang chạy để tránh làm nhiều việc cùng lúc
                            for (auto& [d_key, t_list] : tasks_db) {
                                for (auto& other_t : t_list) {
                                    if (other_t.status == 1 && (d_key != selected_date_str || other_t.id != t.id)) {
                                        other_t.status = 2;
                                        other_t.elapsed_sec += (std::time(nullptr) - other_t.last_start_timestamp);
                                        other_t.last_start_timestamp = 0;
                                        save_instance(other_t, d_key);
                                    }
                                }
                            }
                            
                            t.status = 1; t.last_start_timestamp = std::time(nullptr);
                            system(("notify-send -u low 'Task Started' 'Working on: " + sanitize_for_shell(t.name) + "' &").c_str());
                            
                            if (is_pomo_active && pomo_task_date == selected_date_str && pomo_task_idx == task_sel_idx) {
                                is_pomo_paused = false;
                                pomo_last_tick = std::time(nullptr); 
                                if (!is_work_phase) { is_work_phase = true; time_remaining = work_time * 60; session_time_elapsed = 0; }
                            } else if (is_pomo_active) {
                                is_pomo_paused = true; Task& old_t = tasks_db[pomo_task_date][pomo_task_idx];
                                if (old_t.status == 1) {
                                    old_t.status = 2; 
                                    old_t.elapsed_sec += (std::time(nullptr) - old_t.last_start_timestamp);
                                    old_t.last_start_timestamp = 0;
                                    save_instance(old_t, pomo_task_date);
                                }
                            }
                        } else if (t.status == 1) {
                            t.status = 2; 
                            t.elapsed_sec += (std::time(nullptr) - t.last_start_timestamp);
                            t.last_start_timestamp = 0;
                            if (is_pomo_active && pomo_task_date == selected_date_str && pomo_task_idx == task_sel_idx) is_pomo_paused = true;
                        }
                        save_instance(t, selected_date_str);
                    }
                }
                else if (key == 'd') { 
                    if (!tasks_db[selected_date_str].empty()) {
                        Task& t = tasks_db[selected_date_str][task_sel_idx];
                        if (t.status == 1) {
                            t.elapsed_sec += (std::time(nullptr) - t.last_start_timestamp);
                            t.last_start_timestamp = 0;
                        }
                        t.status = 3; 
                        save_instance(t, selected_date_str);

                        // Tự động ngắt Pomodoro nếu task này đã hoàn thành
                        if (is_pomo_active && pomo_task_date == selected_date_str && pomo_task_idx == task_sel_idx) {
                            is_pomo_active = false;
                        }

                        // Repeat After Done mapping
                        if (t.repeat_type == 3) {
                            int n_days = 1; 
                            try { n_days = std::stoi(t.repeat_val); } catch(...) { n_days = 1; } 
                            if (n_days <= 0) n_days = 1;
                            
                            std::string target_str = format_date(add_days(string_to_tm(selected_date_str), n_days));
                            
                            Task clone = t; 
                            clone.status = 0; clone.elapsed_sec = 0; clone.last_start_timestamp = 0; clone.is_notified = false;
                            clone.created_date = target_str;
                            insert_series(clone); 
                            load_tasks(current_week_sun); 
                        }
                    }
                }
                else if (key == 'D') { 
                    if (!tasks_db[selected_date_str].empty()) {
                        Task& t = tasks_db[selected_date_str][task_sel_idx];
                        
                        if (is_pomo_active && pomo_task_date == selected_date_str && pomo_task_idx == task_sel_idx) {
                            is_pomo_active = false; 
                        }

                        if (t.repeat_type > 0) {
                            t.status = 4; 
                            save_instance(t, selected_date_str);
                        } else {
                            std::string sql = "DELETE FROM series WHERE id=" + std::to_string(t.id) + ";";
                            sqlite3_exec(db, sql.c_str(), 0, 0, 0);
                        }

                        load_tasks(current_week_sun);
                        if (tasks_db[selected_date_str].empty()) is_task_focused = false; 
                        else if (task_sel_idx >= (int)tasks_db[selected_date_str].size()) task_sel_idx--;
                    }
                }
                else if (key == 'X') {
                    if (!tasks_db[selected_date_str].empty()) {
                        Task& t = tasks_db[selected_date_str][task_sel_idx];

                        if (is_pomo_active && pomo_task_date == selected_date_str && pomo_task_idx == task_sel_idx) {
                            is_pomo_active = false; 
                        }

                        if (t.id != 0 && t.repeat_type > 0) {
                            std::string yesterday = format_date(add_days(string_to_tm(selected_date_str), -1));
                            
                            if (t.until_date.empty() || t.until_date > yesterday) {
                                std::string sql = "UPDATE series SET until_date = '" + yesterday + "' WHERE id = " + std::to_string(t.id) + ";";
                                sqlite3_exec(db, sql.c_str(), 0, 0, 0);
                            }
                            
                            load_tasks(current_week_sun);
                            if (task_sel_idx >= (int)tasks_db[selected_date_str].size()) task_sel_idx = std::max(0, (int)tasks_db[selected_date_str].size() - 1);
                            if (tasks_db[selected_date_str].empty()) is_task_focused = false;
                        }
                    }
                }
            }
        }
        else if (current_page == Page::TASK_EDIT) {
            Task& t = tasks_db[selected_date_str][task_sel_idx];
            int max_fields = t.has_start ? (t.has_custom_deadline ? 7 : 6) : 5;
            int idx_repeat_type = max_fields; max_fields += (t.repeat_type > 0 ? 2 : 1);

            if (is_insert_mode) {
                if (key == 27) { is_insert_mode = false; }
                else if (key == '\r' || key == '\n' || key == KEY_ENTER) { 
                    is_insert_mode = false;
                    if (edit_sel_idx == 0 && !input_buffer.empty()) t.name = input_buffer;
                    if (edit_sel_idx == 1) t.desc = input_buffer;
                    if (edit_sel_idx == 3) t.start_min = parse_smart_time(input_buffer, t.start_min);
                    if (edit_sel_idx == 4) t.duration_min = parse_smart_time(input_buffer, t.duration_min);
                    if (edit_sel_idx == 6) t.deadline_min = parse_smart_time(input_buffer, t.deadline_min);
                    if (edit_sel_idx == idx_repeat_type + 1) t.repeat_val = input_buffer;
                } 
                else if (key == KEY_BACKSPACE || key == 127 || key == '\b') { 
                    if (!input_buffer.empty()) {
                        while (!input_buffer.empty() && (input_buffer.back() & 0xC0) == 0x80) input_buffer.pop_back();
                        if (!input_buffer.empty()) input_buffer.pop_back();
                    }
                } 
                else if (key >= 32 && key <= 255) { input_buffer += (char)key; }
            } 
            else { 
                if (key == 'q' || key == 27) {
                    bool is_changed = has_task_changed(original_task_state, t);

                    if (t.id == 0) { 
                        if (t.name != "New Task" || t.desc != "") {
                            insert_series(t);
                            // Cập nhật lại mỏ neo pomo_task_id nếu vừa lưu task mới (id từ 0 thành >0)
                            if (is_pomo_active && pomo_task_date == selected_date_str && pomo_task_idx == task_sel_idx) {
                                pomo_task_id = t.id;
                            }
                        } else {
                            tasks_db[selected_date_str].erase(tasks_db[selected_date_str].begin() + task_sel_idx);
                        }
                    } 
                    else if (is_changed) {
                        if (t.repeat_type > 0) {
                            attron(COLOR_PAIR(CP_SELECTED));
                            mvprintw(LINES/2 + 2, (COLS - 42)/2, "┌────────────────────────────────────────┐");
                            mvprintw(LINES/2 + 3, (COLS - 42)/2, "│ Apply changes to future tasks? [y/N]   │");
                            mvprintw(LINES/2 + 4, (COLS - 42)/2, "└────────────────────────────────────────┘");
                            attroff(COLOR_PAIR(CP_SELECTED));
                            refresh();
                            
                            std::time_t wait_start = std::time(nullptr);
                            timeout(-1); 
                            int ans;
                            while (true) {
                                ans = getch();
                                if (ans == 'y' || ans == 'Y' || ans == 'n' || ans == 'N' || ans == 27 || ans == '\n' || ans == '\r') break;
                            }
                            if (is_pomo_active && !is_pomo_paused) {
                                int time_passed = (int)(std::time(nullptr) - wait_start);
                                time_remaining -= time_passed;
                                session_time_elapsed += time_passed;
                            }
                            
                            // Splitting timeline based on user choice
                            if (ans == 'y' || ans == 'Y') { 
                                std::string yesterday = format_date(add_days(string_to_tm(selected_date_str), -1));
                                if (t.until_date.empty() || t.until_date > yesterday) {
                                    std::string sql = "UPDATE series SET until_date = '" + yesterday + "' WHERE id = " + std::to_string(t.id) + ";";
                                    sqlite3_exec(db, sql.c_str(), 0, 0, 0);
                                }
                                
                                t.created_date = selected_date_str;
                                t.until_date = "";
                                insert_series(t); 
                            }
                            else { 
                                std::string sql_skip = "INSERT INTO instances (series_id, date_str, status) VALUES (" + std::to_string(t.id) + ", '" + selected_date_str + "', 4) ON CONFLICT(series_id, date_str) DO UPDATE SET status=4;";
                                sqlite3_exec(db, sql_skip.c_str(), 0, 0, 0);

                                t.repeat_type = 0; 
                                t.created_date = selected_date_str;
                                insert_series(t);
                            } 
                        } else {
                            update_series(t); 
                        }
                    } 
                    
                    load_tasks(current_week_sun); 
                    current_page = Page::SCHEDULE;
                    
                    if (tasks_db[selected_date_str].empty()) is_task_focused = false;
                    else if (task_sel_idx >= (int)tasks_db[selected_date_str].size()) task_sel_idx--;
                }
                else if (key == 'j' || key == KEY_DOWN) { if (edit_sel_idx < max_fields - 1) edit_sel_idx++; }
                else if (key == 'k' || key == KEY_UP) { if (edit_sel_idx > 0) edit_sel_idx--; }
                else if (key == 'i') { 
                    if (edit_sel_idx == 0) { is_insert_mode = true; input_buffer = t.name; }
                    if (edit_sel_idx == 1) { is_insert_mode = true; input_buffer = t.desc; }
                    if (edit_sel_idx == 3) { is_insert_mode = true; input_buffer = ""; }
                    if (edit_sel_idx == 4) { is_insert_mode = true; input_buffer = ""; }
                    if (edit_sel_idx == 6) { is_insert_mode = true; input_buffer = ""; }
                    if (edit_sel_idx == idx_repeat_type + 1) { is_insert_mode = true; input_buffer = t.repeat_val; }
                }
                else if (key == '\r' || key == '\n' || key == KEY_ENTER || key == ' ') { 
                    if (edit_sel_idx == 2) { t.has_start = !t.has_start; }
                    if (edit_sel_idx == 5) { t.has_custom_deadline = !t.has_custom_deadline; }
                    if (edit_sel_idx == idx_repeat_type) { 
                        t.repeat_type = (t.repeat_type + 1) % 4; 
                        if (t.repeat_type == 0) t.repeat_val = "";
                    }
                }
                else if (key == 'h' || key == 'H' || key == 'l' || key == 'L' || key == KEY_LEFT || key == KEY_RIGHT) {
                    int delta = (key == 'h' || key == 'l' || key == KEY_LEFT || key == KEY_RIGHT) ? 5 : 60;
                    int sign = (key == 'h' || key == 'H' || key == KEY_LEFT) ? -1 : 1;
                    int shift = delta * sign;
                    if (edit_sel_idx == 3) { t.start_min = std::max(0, t.start_min + shift); }
                    if (edit_sel_idx == 4) { t.duration_min = std::max(0, t.duration_min + shift); }
                    if (edit_sel_idx == 6) { t.deadline_min = std::max(0, t.deadline_min + shift); }
                }
            }
        }
        else if (current_page == Page::POMODORO) {
            if (key == 'q' || key == 27) current_page = Page::SCHEDULE;
            else {
                switch (key) {
                    case 'k': case KEY_UP: if (pomo_sel > 0) pomo_sel--; break;
                    case 'j': case KEY_DOWN: if (pomo_sel < 3) pomo_sel++; break;
                    case 'h': case KEY_LEFT: 
                        if (pomo_sel == 1 && work_time > 1) work_time--;
                        if (pomo_sel == 2 && break_time > 1) break_time--;
                        break;
                    case 'l': case KEY_RIGHT: 
                        if (pomo_sel == 1 && work_time < 99) work_time++;
                        if (pomo_sel == 2 && break_time < 99) break_time++;
                        break;
                    case '\r': case '\n': case KEY_ENTER: case ' ':
                        if (pomo_sel == 0) { 
                            if (is_pomo_active && (pomo_task_date != selected_date_str || pomo_task_idx != task_sel_idx)) {
                                 Task& old_t = tasks_db[pomo_task_date][pomo_task_idx];
                                 if (old_t.status == 1) {
                                     old_t.status = 2; 
                                     old_t.elapsed_sec += (std::time(nullptr) - old_t.last_start_timestamp);
                                     old_t.last_start_timestamp = 0;
                                     save_instance(old_t, pomo_task_date);
                                 }
                            }
                            is_pomo_active = true; is_pomo_paused = false; current_page = Page::POMODORO_RUN;
                            pomo_last_tick = std::time(nullptr); 
                            is_work_phase = true; time_remaining = work_time * 60; session_time_elapsed = 0; is_eye_break_active = false;
                            
                            Task& t = tasks_db[pomo_task_date][pomo_task_idx];
                            if (t.status != 1) { t.status = 1; t.last_start_timestamp = std::time(nullptr); }
                            save_instance(t, pomo_task_date);
                        } else if (pomo_sel == 3) { eye_break_enabled = !eye_break_enabled; }
                        break;
                }
            }
        }
        else if (current_page == Page::POMODORO_RUN) {
            if (key == 'q' || key == 27) { current_page = Page::SCHEDULE; }
            else if (key == 's') {
                is_pomo_paused = !is_pomo_paused;
                if (!is_pomo_paused) pomo_last_tick = std::time(nullptr); 
                Task& t = tasks_db[pomo_task_date][pomo_task_idx];
                if (is_pomo_paused) {
                    if (t.status == 1) {
                        t.status = 2; 
                        t.elapsed_sec += (std::time(nullptr) - t.last_start_timestamp);
                        t.last_start_timestamp = 0;
                    }
                } else { if (is_work_phase) { t.status = 1; t.last_start_timestamp = std::time(nullptr); } }
                save_instance(t, pomo_task_date);
            }
            else if (key == 'c') {
                is_pomo_active = false;
                Task& t = tasks_db[pomo_task_date][pomo_task_idx];
                if (t.status == 1) {
                    t.status = 2; 
                    t.elapsed_sec += (std::time(nullptr) - t.last_start_timestamp);
                    t.last_start_timestamp = 0;
                }
                save_instance(t, pomo_task_date); current_page = Page::SCHEDULE;
            }
        }
        else if (current_page == Page::INFO) {
            if (key == 'q' || key == 27 || key == '\r' || key == '\n' || key == KEY_ENTER) current_page = Page::MAIN_MENU;
        }
    }

    return 0;
}
