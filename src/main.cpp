#include <iostream>
#include <string>
#include <array>
#include <unistd.h>
#include <termios.h>
#include <poll.h>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <vector>
#include <iomanip>
#include <sys/ioctl.h>
#include <algorithm>
#include <cmath>
#include <cerrno>
#include <sys/stat.h>
#include <fstream>
#include <map>
// [MOD] Thêm nlohmann json
#include "json.hpp"

using json = nlohmann::json;

volatile sig_atomic_t g_winch = 0;

void sigwinch_handler(int) {
    g_winch = 1;
}

struct TerminalSession {
    static inline termios orig_termios;

    static void restore() {
        std::cout << "\033[0m\033[?25h\033[0 q\033[?1049l" << std::flush;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    }

    static void signal_handler(int) {
        restore();
        std::exit(0);
    }

    static void init() {
        tcgetattr(STDIN_FILENO, &orig_termios);
        std::atexit(restore);
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);

        struct sigaction sa;
        sa.sa_handler = sigwinch_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0; 
        sigaction(SIGWINCH, &sa, NULL);

        termios raw = orig_termios;
        raw.c_lflag &= ~(ECHO | ICANON | ISIG);
        raw.c_iflag &= ~(IXON | ICRNL);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

        std::cout << "\033[?1049h\033[2J\033[H\033[?25l" << std::flush;
    }
};

enum class Page { MAIN_MENU, SCHEDULE, POMODORO, POMODORO_RUN, INFO, TASK_EDIT };

const int MENU_SIZE = 2;
const std::array<std::string, MENU_SIZE> MENU_ITEMS = { "Schedule", "Info" };

struct TimeInterval {
    long long start;
    long long stop;
};

struct Task {
    int id;
    std::string name = "New Task";
    std::string desc = "";
    bool has_start = true;
    int start_min = 480; 
    int duration_min = 60; 
    bool has_custom_deadline = false;
    int deadline_min = 540; 
    int status = 0; 
    
    std::vector<TimeInterval> intervals;
    long long last_start_timestamp = 0;
    
    bool is_notified = false;
};

long long get_elapsed_sec(const Task& t) {
    long long total = 0;
    for (const auto& iv : t.intervals) {
        total += (iv.stop - iv.start);
    }
    if (t.status == 1 && t.last_start_timestamp > 0) {
        long long now = std::time(nullptr);
        if (now > t.last_start_timestamp) {
            total += (now - t.last_start_timestamp);
        }
    }
    return total;
}

std::map<std::string, std::vector<Task>> tasks_db;
std::string selected_date_str = "";
int task_sel_idx = 0;
int edit_sel_idx = 0;
bool is_insert_mode = false;
std::string input_buffer = "";
int next_task_id = 1;

// [MOD] Khai báo trạng thái toàn cục của Pomodoro
std::string pomo_task_date = "";
int pomo_task_idx = -1;
int session_time_elapsed = 0;
bool is_eye_break_active = false;
int eye_break_remaining = 0;
bool is_pomo_active = false; 
bool is_pomo_paused = false;

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
        try { return std::stoi(buf); } 
        catch(...) { return fallback; }
    }
}

std::string get_data_file() {
    std::string path = std::string(getenv("HOME")) + "/.local/normal-productivity";
    mkdir(path.c_str(), 0777);
    return path + "/tasks.json";
}

void save_tasks() {
    json j;
    for (const auto& [date, tasks] : tasks_db) {
        json j_tasks = json::array();
        for (const auto& t : tasks) {
            json jt;
            jt["id"] = t.id;
            jt["name"] = t.name;
            jt["desc"] = t.desc;
            jt["has_start"] = t.has_start;
            jt["start_min"] = t.start_min;
            jt["duration_min"] = t.duration_min;
            jt["has_custom_deadline"] = t.has_custom_deadline;
            jt["deadline_min"] = t.deadline_min;
            jt["status"] = t.status;
            jt["last_start"] = t.last_start_timestamp;
            jt["is_notified"] = t.is_notified;

            json j_iv = json::array();
            for (const auto& iv : t.intervals) {
                j_iv.push_back({{"start", iv.start}, {"stop", iv.stop}});
            }
            jt["intervals"] = j_iv;
            j_tasks.push_back(jt);
        }
        j[date] = j_tasks;
    }
    std::ofstream out(get_data_file());
    out << j.dump(2);
}

void load_tasks() {
    std::ifstream in(get_data_file());
    if (!in.is_open()) return;
    try {
        json j;
        in >> j;
        for (auto& [date, j_tasks] : j.items()) {
            for (auto& jt : j_tasks) {
                Task t;
                t.id = jt.value("id", 0);
                t.name = jt.value("name", "");
                t.desc = jt.value("desc", "");
                t.has_start = jt.value("has_start", true);
                t.start_min = jt.value("start_min", 480);
                t.duration_min = jt.value("duration_min", 60);
                t.has_custom_deadline = jt.value("has_custom_deadline", false);
                t.deadline_min = jt.value("deadline_min", 540);
                t.status = jt.value("status", 0);
                t.last_start_timestamp = jt.value("last_start", 0LL);
                t.is_notified = jt.value("is_notified", false);

                if (jt.contains("intervals") && jt["intervals"].is_array()) {
                    for (auto& iv : jt["intervals"]) {
                        t.intervals.push_back({iv.value("start", 0LL), iv.value("stop", 0LL)});
                    }
                }
                if (t.id >= next_task_id) next_task_id = t.id + 1;
                tasks_db[date].push_back(t);
            }
        }
    } catch (...) {} 
}

std::tm add_days(std::tm date, int days) {
    date.tm_mday += days;
    std::mktime(&date);
    return date;
}

bool is_same_day(const std::tm& d1, const std::tm& d2) {
    return (d1.tm_year == d2.tm_year && d1.tm_yday == d2.tm_yday);
}

void get_term_size(int &rows, int &cols) {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == -1 || w.ws_col == 0) {
        rows = 24; cols = 80;
    } else {
        rows = w.ws_row; cols = w.ws_col;
    }
}

std::string center_text(const std::string& text, int width) {
    int pad_left = std::max(0, (width - (int)text.length()) / 2);
    int pad_right = std::max(0, width - (int)text.length() - pad_left);
    return std::string(pad_left, ' ') + text + std::string(pad_right, ' ');
}

std::string draw_box(const std::string& text, int width, bool selected, const std::string& lm) {
    std::string res = "";
    std::string color = selected ? "\033[7m" : "";
    std::string reset = "\033[0m";
    int p_l = std::max(0, (width - (int)text.length()) / 2);
    int p_r = std::max(0, width - (int)text.length() - p_l);
    
    std::string h_line = "";
    for (int i = 0; i < width; ++i) h_line += "─";
    
    res += lm + color + "┌" + h_line + "┐" + reset + "\033[K\n";
    res += lm + color + "│" + std::string(p_l, ' ') + text + std::string(p_r, ' ') + "│" + reset + "\033[K\n";
    res += lm + color + "└" + h_line + "┘" + reset + "\033[K\n";
    return res;
}

void render_ui(Page current_page, int sel_idx, int sched_sel_c, const std::tm& current_week_sun, const std::tm& today, 
               int work_time, int break_time, bool eye_break_enabled, int time_remaining, bool is_work_phase, bool is_task_focused) {
    int rows, cols;
    get_term_size(rows, cols);
    
    // [MOD] Thêm \033[0m ngay từ đầu để triệt tiêu vệt màu nền bị kẹt lại từ \033[u
    std::string out = "\033[0m\033[H"; 

    std::string today_str = format_date(today);
    int now_min = today.tm_hour * 60 + today.tm_min;

    if (current_page == Page::MAIN_MENU) {
        int menu_width = 24;
        int total_height = MENU_SIZE * 4 - 1;
        int top_pad = std::max(0, (rows - total_height) / 2);
        int left_pad = std::max(0, (cols - menu_width - 2) / 2);
        std::string lm(left_pad, ' ');

        for (int i = 0; i < top_pad; ++i) out += "\033[K\n";
        for (int r = 0; r < MENU_SIZE; ++r) {
            out += draw_box(MENU_ITEMS[r], menu_width, (r == sel_idx), lm);
            if (r < MENU_SIZE - 1) out += "\033[K\n";
        }
    } 
    else if (current_page == Page::SCHEDULE) {
        int cell_w = 9;
        int sched_width = 7 * cell_w + 8;
        int top_pad = std::max(0, (rows - 22) / 2);
        int left_pad = std::max(0, (cols - sched_width) / 2);
        std::string lm(left_pad, ' ');

        for (int i = 0; i < top_pad; ++i) out += "\033[K\n";

        auto line_builder = [&](std::string l, std::string m, std::string r, std::string fill) {
            std::string s = lm + l;
            for(int i = 0; i < 7; ++i) {
                for(int j=0; j<cell_w; ++j) s += fill;
                if (i < 6) s += m;
            }
            s += r + "\033[K\n";
            return s;
        };

        std::string top_border = line_builder("┌", "┬", "┐", "─");
        std::string sep_border = line_builder("├", "┼", "┤", "─");
        std::string bot_border = line_builder("└", "┴", "┘", "─");

        std::string mid_names = lm + "│";
        std::string mid_dates = lm + "│";
        std::string mid_tasks = lm + "│";

        const std::array<std::string, 7> day_names = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

        for (int c = 0; c < 7; ++c) {
            std::tm day = add_days(current_week_sun, c);
            bool is_today = is_same_day(day, today);
            bool is_selected = (c == sched_sel_c);

            char date_buf[16];
            std::strftime(date_buf, sizeof(date_buf), "%d/%m", &day);

            std::string day_key = format_date(day);
            int t_cnt = tasks_db[day_key].size();
            
            std::string name_str = center_text(day_names[c], cell_w);
            std::string date_str = center_text(std::string(date_buf), cell_w);
            std::string cnt_str  = t_cnt > 0 ? center_text("[" + std::to_string(t_cnt) + "]", cell_w) : center_text("", cell_w);

            std::string color = is_today ? "\033[31m" : ""; 
            if (is_selected) color = "\033[7m" + color;
            std::string reset = "\033[0m";

            mid_names += color + name_str + reset + "│";
            mid_dates += color + date_str + reset + "│";
            mid_tasks += color + cnt_str + reset + "│";
        }
        mid_names += "\033[K\n";
        mid_dates += "\033[K\n";
        mid_tasks += "\033[K\n";

        out += top_border + mid_names + mid_dates + mid_tasks + bot_border + "\033[K\n";
        out += lm + "\033[1mTasks for " + selected_date_str + "\033[0m\033[K\n\033[K\n";

        auto& tasks = tasks_db[selected_date_str];
        if (tasks.empty()) {
            out += lm + "\033[90m< No tasks for this day >\033[0m\033[K\n";
        } else {
            for (size_t i = 0; i < tasks.size(); ++i) {
                auto& t = tasks[i];
                bool selected = (is_task_focused && (int)i == task_sel_idx);
                std::string prefix = selected ? "> " : "  ";
                
                std::string color = "\033[0m"; 
                bool overdue = false;
                
                int dl = t.has_custom_deadline ? t.deadline_min : (t.start_min + t.duration_min);
                if (selected_date_str < today_str || (selected_date_str == today_str && now_min >= dl)) {
                    overdue = true;
                }

                if (t.status == 3) color = "\033[90m"; 
                else if (!t.has_start) color = "\033[37m"; 
                else if (t.status == 1) color = overdue ? "\033[35m" : "\033[32m"; 
                else color = overdue ? "\033[31m" : "\033[34m"; 

                long long elapsed = get_elapsed_sec(t);
                double pct = 0.0;
                if (t.duration_min > 0) {
                    pct = ((double)elapsed / (t.duration_min * 60.0)) * 100.0;
                }
                char pct_buf[16];
                snprintf(pct_buf, sizeof(pct_buf), "%5.1f%%", pct);

                std::string time_str = t.has_start ? format_time_24(t.start_min) : "     ";
                std::string status_sym = (t.status == 1) ? "[>]" : (t.status == 2) ? "[||]" : (t.status == 3) ? "[v]" : "[ ]";
                
                // [MOD] Dùng chữ P thay cho đường phân tách '|' nếu task này đang được gắn Pomodoro
                bool is_pomo_attached = (is_pomo_active && pomo_task_date == selected_date_str && pomo_task_idx == (int)i);
                std::string sep = is_pomo_attached ? " P " : " | ";

                std::string row = prefix + status_sym + " " + time_str + sep + t.name + " [" + std::string(pct_buf) + "]";
                if (selected) out += lm + "\033[7m" + color + row + "\033[0m\033[K\n";
                else out += lm + color + row + "\033[0m\033[K\n";
            }
        }

        out += "\033[K\n";
        if (is_task_focused) {
            out += lm + "\033[2m[j/k] Move  [p] Pomodoro  [a] Add  [s] Start/Pause  [d/D] Done/Del  [Enter] Edit  [q] Back\033[0m\033[K\n";
        } else {
            out += lm + "\033[2m[h/l] Change Day  [t] Today  [Enter] Focus Tasks  [a] Add Task  [q] Menu\033[0m\033[K\n";
        }
    }
    else if (current_page == Page::TASK_EDIT) {
        int w = 50;
        int top_pad = std::max(0, (rows - 12) / 2);
        int left_pad = std::max(0, (cols - w) / 2);
        std::string lm(left_pad, ' ');
        for (int i = 0; i < top_pad; ++i) out += "\033[K\n";

        Task& t = tasks_db[selected_date_str][task_sel_idx];
        out += lm + "\033[1m=== Edit Task ===\033[0m\033[K\n\033[K\n";

        auto render_field = [&](int idx, std::string label, std::string val) {
            std::string res = lm;
            if (edit_sel_idx == idx) res += "\033[7m";
            res += label + ": ";
            if (edit_sel_idx == idx && is_insert_mode) {
                res += "[" + input_buffer + "\033[s ]";
            } else res += val;
            res += "\033[0m\033[K\n";
            out += res;
        };

        render_field(0, "Name           ", t.name);
        render_field(1, "Description    ", t.desc);
        render_field(2, "Has Start Time ", t.has_start ? "Yes" : "No (No deadline)");
        
        if (t.has_start) render_field(3, "Start Time     ", format_time_24(t.start_min));
        else out += lm + "\033[90mStart Time     : --:--\033[0m\033[K\n";
        
        render_field(4, "Duration       ", format_time_24(t.duration_min));
        
        if (t.has_start) {
            render_field(5, "Custom Deadline", t.has_custom_deadline ? "Yes" : "No (Auto: Start+Dur)");
            if (t.has_custom_deadline) render_field(6, "Deadline Time  ", format_time_24(t.deadline_min));
            else out += lm + "\033[90mDeadline Time  : " + format_time_24(t.start_min + t.duration_min) + "\033[0m\033[K\n";
        }

        long long elapsed = get_elapsed_sec(t);
        int eh = elapsed / 3600;
        int em = (elapsed % 3600) / 60;
        int es = elapsed % 60;
        char ebuf[64];
        snprintf(ebuf, sizeof(ebuf), "%02d:%02d:%02d", eh, em, es);
        out += lm + "\033[90mElapsed Time   : " + std::string(ebuf) + "\033[0m\033[K\n";

        out += "\033[K\n" + lm + "\033[2m[i/Enter] Input  [Esc] Normal Mode  [h/l] +/- 5m  [q] Save\033[0m\033[K\n";
    }
    else if (current_page == Page::POMODORO) {
        int menu_width = 30;
        int total_height = 4 * 4 - 1;
        int top_pad = std::max(0, (rows - total_height) / 2);
        int left_pad = std::max(0, (cols - menu_width - 2) / 2);
        std::string lm(left_pad, ' ');

        for (int i = 0; i < top_pad; ++i) out += "\033[K\n";

        std::string task_name = tasks_db[pomo_task_date][pomo_task_idx].name;
        if (task_name.length() > 20) task_name = task_name.substr(0, 17) + "...";
        out += lm + "\033[1mTask: " + task_name + "\033[0m\033[K\n\033[K\n";

        std::array<std::string, 4> pomo_items = {
            "Start Timer",
            "Work: < " + std::to_string(work_time) + " > min",
            "Break: < " + std::to_string(break_time) + " > min",
            "Eye Break: < " + std::string(eye_break_enabled ? "ON" : "OFF") + " >"
        };

        for (int i = 0; i < 4; ++i) {
            out += draw_box(pomo_items[i], menu_width, (i == sel_idx), lm);
            if (i < 3) out += "\033[K\n";
        }
        out += "\033[K\n" + lm + "\033[2m[q] Back to Schedule\033[0m\033[K\n";
    }
    else if (current_page == Page::POMODORO_RUN) {
        int bar_w = 40; 
        int block_w = bar_w + 2; 
        int total_height = 8;
        
        int top_pad = std::max(0, (rows - total_height) / 2);
        int left_pad = std::max(0, (cols - block_w) / 2); 
        std::string lm(left_pad, ' ');

        for (int i = 0; i < top_pad; ++i) out += "\033[K\n";

        if (is_eye_break_active) {
            std::string msg1 = "EYE BREAK: LOOK 20 FEET AWAY";
            std::string msg2 = "Resuming in " + std::to_string(eye_break_remaining) + "s";
            
            int m1_l = std::max(0, (block_w - (int)msg1.length()) / 2);
            int m1_r = std::max(0, block_w - (int)msg1.length() - m1_l);
            int m2_l = std::max(0, (block_w - (int)msg2.length()) / 2);
            int m2_r = std::max(0, block_w - (int)msg2.length() - m2_l);

            out += "\033[K\n\033[K\n";
            out += lm + std::string(m1_l, ' ') + "\033[36;1m" + msg1 + "\033[0m" + std::string(m1_r, ' ') + "\033[K\n";
            out += lm + std::string(m2_l, ' ') + "\033[1m" + msg2 + "\033[0m" + std::string(m2_r, ' ') + "\033[K\n\033[K\n";
            out += "\033[K\n\033[K\n\033[K\n";
        } 
        else {
            int total_sec = (is_work_phase ? work_time : break_time) * 60;
            float progress = 1.0f - ((float)time_remaining / total_sec); 
            int filled_chars = std::max(0, std::min(bar_w, (int)(progress * bar_w))); 
            
            std::string phase_txt = is_work_phase ? "WORK PHASE" : "BREAK PHASE";
            if (is_pomo_paused) phase_txt += " (PAUSED)";
            
            std::string color = is_work_phase ? "\033[31;1m" : "\033[32;1m"; 
            
            int h = time_remaining / 3600;
            int m = (time_remaining % 3600) / 60;
            int s = time_remaining % 60;
            char time_buf[16];
            std::snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d", h, m, s);
            std::string time_str = std::string(time_buf);

            int p_l = std::max(0, (block_w - (int)phase_txt.length()) / 2);
            int p_r = std::max(0, block_w - (int)phase_txt.length() - p_l);
            int t_l = std::max(0, (block_w - (int)time_str.length()) / 2);
            int t_r = std::max(0, block_w - (int)time_str.length() - t_l);

            out += lm + std::string(p_l, ' ') + color + phase_txt + "\033[0m" + std::string(p_r, ' ') + "\033[K\n";
            out += lm + std::string(t_l, ' ') + "\033[1m" + time_str + "\033[0m" + std::string(t_r, ' ') + "\033[K\n\033[K\n";

            std::string bar_top = "┌"; for(int i=0; i<bar_w; ++i) bar_top += "─"; bar_top += "┐";
            std::string bar_bot = "└"; for(int i=0; i<bar_w; ++i) bar_bot += "─"; bar_bot += "┘";
            std::string bar_mid = "│" + color;
            for (int i = 0; i < filled_chars; ++i) bar_mid += "█";
            bar_mid += "\033[0m"; 
            for (int i = filled_chars; i < bar_w; ++i) bar_mid += " ";
            bar_mid += "│";

            out += lm + bar_top + "\033[K\n";
            out += lm + bar_mid + "\033[K\n";
            out += lm + bar_bot + "\033[K\n\033[K\n";

            // [MOD] Phím điều khiển Pomodoro mới
            std::string msg = "[s] Pause/Resume  [c] Cancel  [q] Background";
            int m_l = std::max(0, (block_w - (int)msg.length()) / 2);
            int m_r = std::max(0, block_w - (int)msg.length() - m_l);
            out += lm + std::string(m_l, ' ') + "\033[2m" + msg + "\033[0m" + std::string(m_r, ' ') + "\033[K\n";
        }
    }
    else if (current_page == Page::INFO) {
        int info_width = 44;
        int total_height = 8;
        int top_pad = std::max(0, (rows - total_height) / 2);
        int left_pad = std::max(0, (cols - info_width) / 2);
        std::string lm(left_pad, ' ');

        for (int i = 0; i < top_pad; ++i) out += "\033[K\n";

        out += lm + "\033[1m=== MINIMAL TUI INFO ===\033[0m\033[K\n\033[K\n";
        out += lm + "Navigation : [j / k] Up/Down\033[K\n";
        out += lm + "Action     : [Enter] Select/Input\033[K\n";
        out += lm + "Task Keys  : [a] Add [s] Start [d/D] Done/Del\033[K\n";
        out += lm + "Quit/Back  : [q / ESC]\033[K\n\033[K\n";
        out += lm + "\033[2mPress [q] to return.\033[0m\033[K\n";
    }

    out += "\033[J"; 
    
    if (current_page == Page::TASK_EDIT && is_insert_mode) {
        out += "\033[u\033[?25h\033[5 q";
    } else {
        out += "\033[?25l";
    }
    
    std::cout << out << std::flush;
}

int main() {
    TerminalSession::init();
    load_tasks(); 

    Page current_page = Page::MAIN_MENU;
    int main_sel_r = 0, pomo_sel = 0;
    
    std::time_t init_t = std::time(nullptr);
    std::tm init_tm = *std::localtime(&init_t);
    std::tm current_week_sun = add_days(init_tm, -init_tm.tm_wday);
    
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
            
            if (tasks_db.count(today_str)) {
                for (auto& task : tasks_db[today_str]) {
                    if (task.has_start && task.status == 0 && !task.is_notified) {
                        if (now_min >= task.start_min) {
                            task.is_notified = true;
                            std::string cmd = "notify-send -u critical 'Task Reminder' 'Time to work: " + task.name + "' &";
                            system(cmd.c_str());
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
                if (tk.status == 1) { 
                    any_task_running = true; 
                    break; 
                }
            }
            if (any_task_running) break;
        }

        int timeout_ms = 1000;
        // [MOD] Timeout luôn là 1s nếu Pomodoro đang chạy (để đếm ngược background)
        if (!is_pomo_active && !any_task_running) {
            timeout_ms = (60 - (current_unix % 60)) * 1000;
            if (timeout_ms <= 0) timeout_ms = 1000; 
        }

        struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
        int ret = poll(&pfd, 1, timeout_ms);

        if (ret == -1 && errno == EINTR) {
            if (g_winch) {
                g_winch = 0;
                needs_redraw = true; 
                continue;
            }
        } 
        else if (ret == 0) {
            if (any_task_running) needs_redraw = true; 

            // [MOD] Pomodoro Background Tick
            if (is_pomo_active && !is_pomo_paused) {
                if (time_remaining > 0) time_remaining--;
                session_time_elapsed++;
                
                // Trỗi dậy ép xem màn hình Eye Break
                if (eye_break_enabled && session_time_elapsed > 0 && session_time_elapsed % 1200 == 0) {
                    is_eye_break_active = true;
                    eye_break_remaining = 20;
                    if (current_page != Page::POMODORO_RUN) {
                        current_page = Page::POMODORO_RUN;
                    }
                }

                if (is_eye_break_active) {
                    if (eye_break_remaining > 0) eye_break_remaining--;
                    if (eye_break_remaining <= 0) is_eye_break_active = false;
                }

                if (time_remaining == 0) {
                    is_work_phase = !is_work_phase;
                    time_remaining = (is_work_phase ? work_time : break_time) * 60;
                    session_time_elapsed = 0;
                    is_eye_break_active = false;
                    
                    Task& t = tasks_db[pomo_task_date][pomo_task_idx];
                    if (is_work_phase) {
                        if (t.status != 1) {
                            t.status = 1; 
                            t.last_start_timestamp = std::time(nullptr);
                        }
                        system("notify-send -u critical 'Pomodoro' 'Work phase started!' &");
                    } else {
                        if (t.status == 1) {
                            t.status = 2;
                            t.intervals.push_back({t.last_start_timestamp, (long long)std::time(nullptr)});
                            t.last_start_timestamp = 0;
                        }
                        system("notify-send -u critical 'Pomodoro' 'Break phase started!' &");
                    }
                    save_tasks();
                    std::cout << "\a" << std::flush; 
                }
                needs_redraw = true; 
            }
            continue; 
        }

        char key = 0;
        if (read(STDIN_FILENO, &key, 1) > 0) {
            needs_redraw = true; 
            if (key == '\033') { 
                struct pollfd pfd2 = { STDIN_FILENO, POLLIN, 0 };
                if (poll(&pfd2, 1, 25) > 0) {
                    char seq[2];
                    if (read(STDIN_FILENO, &seq[0], 1) > 0 && read(STDIN_FILENO, &seq[1], 1) > 0) {
                        if (seq[0] == '[') {
                            if (seq[1] == 'A') key = 'k';
                            else if (seq[1] == 'B') key = 'j';
                            else if (seq[1] == 'C') key = 'l';
                            else if (seq[1] == 'D') key = 'h';
                        }
                    }
                } else key = 27; 
            }
        }

        if (key == 3) break; 

        if (current_page == Page::MAIN_MENU) {
            if (key == 'q' || key == 27) break;
            switch (key) {
                case 'k': if (main_sel_r > 0) main_sel_r--; break;
                case 'j': if (main_sel_r < MENU_SIZE - 1) main_sel_r++; break;
                case '\r': case '\n': case ' ':
                    if (main_sel_r == 0) { current_page = Page::SCHEDULE; is_task_focused = false; }
                    else if (main_sel_r == 1) current_page = Page::INFO;
                    break;
            }
        } 
        else if (current_page == Page::SCHEDULE) {
            if (!is_task_focused) { 
                if (key == 'q' || key == 27) current_page = Page::MAIN_MENU;
                else if (key == 'h') {
                    if (sched_sel_c > 0) sched_sel_c--;
                    else { sched_sel_c = 6; current_week_sun = add_days(current_week_sun, -7); }
                }
                else if (key == 'l') {
                    if (sched_sel_c < 6) sched_sel_c++;
                    else { sched_sel_c = 0; current_week_sun = add_days(current_week_sun, 7); }
                }
                else if (key == 't') {
                    current_week_sun = add_days(*now, -now->tm_wday);
                    sched_sel_c = now->tm_wday;
                }
                else if (key == '\r' || key == '\n' || key == ' ') {
                    if (!tasks_db[selected_date_str].empty()) {
                        is_task_focused = true;
                        task_sel_idx = 0;
                    }
                }
                else if (key == 'a') {
                    Task new_t;
                    new_t.id = next_task_id++;
                    tasks_db[selected_date_str].push_back(new_t);
                    current_page = Page::TASK_EDIT;
                    task_sel_idx = tasks_db[selected_date_str].size() - 1;
                    edit_sel_idx = 0;
                    is_insert_mode = true;
                    input_buffer = "";
                }
            } else { 
                if (key == 'q' || key == 27) {
                    is_task_focused = false;
                }
                else if (key == 'j') {
                    if (task_sel_idx < (int)tasks_db[selected_date_str].size() - 1) task_sel_idx++;
                }
                else if (key == 'k') {
                    if (task_sel_idx > 0) task_sel_idx--;
                }
                else if (key == 'a') {
                    Task new_t;
                    new_t.id = next_task_id++;
                    tasks_db[selected_date_str].push_back(new_t);
                    current_page = Page::TASK_EDIT;
                    task_sel_idx = tasks_db[selected_date_str].size() - 1;
                    edit_sel_idx = 0;
                    is_insert_mode = true;
                    input_buffer = "";
                }
                else if (key == '\r' || key == '\n') {
                    if (!tasks_db[selected_date_str].empty()) {
                        current_page = Page::TASK_EDIT;
                        edit_sel_idx = 0;
                    }
                }
                else if (key == 'p') { 
                    if (!tasks_db[selected_date_str].empty()) {
                        if (is_pomo_active && pomo_task_date == selected_date_str && pomo_task_idx == task_sel_idx) {
                            current_page = Page::POMODORO_RUN;
                        } else {
                            pomo_task_date = selected_date_str;
                            pomo_task_idx = task_sel_idx;
                            current_page = Page::POMODORO;
                            pomo_sel = 0;
                        }
                    }
                }
                else if (key == 's') { 
                    // [MOD] Start/Pause Task từ ngoài Schedule đồng thời điều hướng cả Pomodoro
                    if (!tasks_db[selected_date_str].empty()) {
                        Task& t = tasks_db[selected_date_str][task_sel_idx];
                        if (t.status == 0 || t.status == 2) {
                            t.status = 1; 
                            t.last_start_timestamp = std::time(nullptr);
                            
                            if (is_pomo_active && pomo_task_date == selected_date_str && pomo_task_idx == task_sel_idx) {
                                is_pomo_paused = false;
                                if (!is_work_phase) {
                                    is_work_phase = true;
                                    time_remaining = work_time * 60;
                                    session_time_elapsed = 0;
                                }
                            } else if (is_pomo_active) {
                                is_pomo_paused = true;
                                Task& old_t = tasks_db[pomo_task_date][pomo_task_idx];
                                if (old_t.status == 1) {
                                    old_t.status = 2;
                                    old_t.intervals.push_back({old_t.last_start_timestamp, (long long)std::time(nullptr)});
                                    old_t.last_start_timestamp = 0;
                                }
                            }
                        } else if (t.status == 1) {
                            t.status = 2; 
                            t.intervals.push_back({t.last_start_timestamp, (long long)std::time(nullptr)});
                            t.last_start_timestamp = 0;
                            
                            if (is_pomo_active && pomo_task_date == selected_date_str && pomo_task_idx == task_sel_idx) {
                                is_pomo_paused = true;
                            }
                        }
                        save_tasks();
                    }
                }
                else if (key == 'd') { 
                    if (!tasks_db[selected_date_str].empty()) {
                        Task& t = tasks_db[selected_date_str][task_sel_idx];
                        if (t.status == 1) {
                            t.intervals.push_back({t.last_start_timestamp, (long long)std::time(nullptr)});
                            t.last_start_timestamp = 0;
                        }
                        t.status = 3;
                        save_tasks();
                    }
                }
                else if (key == 'D') { 
                    if (!tasks_db[selected_date_str].empty()) {
                        tasks_db[selected_date_str].erase(tasks_db[selected_date_str].begin() + task_sel_idx);
                        if (tasks_db[selected_date_str].empty()) {
                            is_task_focused = false; 
                        } else if (task_sel_idx >= (int)tasks_db[selected_date_str].size()) {
                            task_sel_idx = tasks_db[selected_date_str].size() - 1;
                        }
                        save_tasks();
                    }
                }
            }
        }
        else if (current_page == Page::TASK_EDIT) {
            Task& t = tasks_db[selected_date_str][task_sel_idx];
            int max_fields = t.has_start ? (t.has_custom_deadline ? 7 : 6) : 5;

            if (is_insert_mode) {
                if (key == 27) { 
                    is_insert_mode = false;
                }
                else if (key == '\r' || key == '\n') { 
                    is_insert_mode = false;
                    if (edit_sel_idx == 0 && !input_buffer.empty()) t.name = input_buffer;
                    if (edit_sel_idx == 1) t.desc = input_buffer;
                    if (edit_sel_idx == 3) t.start_min = parse_smart_time(input_buffer, t.start_min);
                    if (edit_sel_idx == 4) t.duration_min = parse_smart_time(input_buffer, t.duration_min);
                    if (edit_sel_idx == 6) t.deadline_min = parse_smart_time(input_buffer, t.deadline_min);
                } 
                else if (key == 127 || key == '\b') { 
                    if (!input_buffer.empty()) {
                        while (!input_buffer.empty() && (input_buffer.back() & 0xC0) == 0x80) {
                            input_buffer.pop_back();
                        }
                        if (!input_buffer.empty()) input_buffer.pop_back();
                    }
                } 
                else if (isprint(key)) {
                    input_buffer += key;
                }
            } 
            else { 
                if (key == 'q' || key == 27) {
                    save_tasks();
                    current_page = Page::SCHEDULE;
                    is_task_focused = true; 
                }
                else if (key == 'j') { if (edit_sel_idx < max_fields - 1) edit_sel_idx++; }
                else if (key == 'k') { if (edit_sel_idx > 0) edit_sel_idx--; }
                else if (key == 'i') { 
                    if (edit_sel_idx == 0) { is_insert_mode = true; input_buffer = t.name; }
                    if (edit_sel_idx == 1) { is_insert_mode = true; input_buffer = t.desc; }
                    if (edit_sel_idx == 3) { is_insert_mode = true; input_buffer = ""; }
                    if (edit_sel_idx == 4) { is_insert_mode = true; input_buffer = ""; }
                    if (edit_sel_idx == 6) { is_insert_mode = true; input_buffer = ""; }
                }
                else if (key == '\r' || key == '\n') {
                    if (edit_sel_idx == 2) { t.has_start = !t.has_start; }
                    if (edit_sel_idx == 5) { t.has_custom_deadline = !t.has_custom_deadline; }
                }
                else if (key == 'h' || key == 'H' || key == 'l' || key == 'L') {
                    int delta = (key == 'h' || key == 'l') ? 5 : 60;
                    int sign = (key == 'h' || key == 'H') ? -1 : 1;
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
                    case 'k': if (pomo_sel > 0) pomo_sel--; break;
                    case 'j': if (pomo_sel < 3) pomo_sel++; break;
                    case 'h': 
                        if (pomo_sel == 1 && work_time > 1) work_time--;
                        if (pomo_sel == 2 && break_time > 1) break_time--;
                        break;
                    case 'l': 
                        if (pomo_sel == 1 && work_time < 99) work_time++;
                        if (pomo_sel == 2 && break_time < 99) break_time++;
                        break;
                    case '\r': case '\n': case ' ':
                        if (pomo_sel == 0) { 
                            if (is_pomo_active && (pomo_task_date != selected_date_str || pomo_task_idx != task_sel_idx)) {
                                 Task& old_t = tasks_db[pomo_task_date][pomo_task_idx];
                                 if (old_t.status == 1) {
                                     old_t.status = 2;
                                     old_t.intervals.push_back({old_t.last_start_timestamp, (long long)std::time(nullptr)});
                                     old_t.last_start_timestamp = 0;
                                 }
                            }
                            
                            is_pomo_active = true;
                            is_pomo_paused = false;
                            current_page = Page::POMODORO_RUN;
                            is_work_phase = true;
                            time_remaining = work_time * 60;
                            session_time_elapsed = 0;
                            is_eye_break_active = false;
                            
                            Task& t = tasks_db[pomo_task_date][pomo_task_idx];
                            if (t.status != 1) {
                                t.status = 1;
                                t.last_start_timestamp = std::time(nullptr);
                            }
                            save_tasks();
                        } else if (pomo_sel == 3) { 
                            eye_break_enabled = !eye_break_enabled;
                        }
                        break;
                }
            }
        }
        else if (current_page == Page::POMODORO_RUN) {
            if (key == 'q' || key == 27) {
                // [MOD] Ẩn Pomodoro xuống background để trở về Schedule (Không hủy)
                current_page = Page::SCHEDULE; 
            }
            else if (key == 's') {
                // [MOD] Pause/Resume độc lập ngay trong màn hình Pomodoro
                is_pomo_paused = !is_pomo_paused;
                Task& t = tasks_db[pomo_task_date][pomo_task_idx];
                if (is_pomo_paused) {
                    if (t.status == 1) {
                        t.status = 2;
                        t.intervals.push_back({t.last_start_timestamp, (long long)std::time(nullptr)});
                        t.last_start_timestamp = 0;
                    }
                } else {
                    if (is_work_phase) {
                        t.status = 1;
                        t.last_start_timestamp = std::time(nullptr);
                    }
                }
                save_tasks();
            }
            else if (key == 'c') {
                // [MOD] Chủ động Hủy Pomodoro hoàn toàn
                is_pomo_active = false;
                Task& t = tasks_db[pomo_task_date][pomo_task_idx];
                if (t.status == 1) {
                    t.status = 2;
                    t.intervals.push_back({t.last_start_timestamp, (long long)std::time(nullptr)});
                    t.last_start_timestamp = 0;
                }
                save_tasks();
                current_page = Page::SCHEDULE;
            }
        }
        else if (current_page == Page::INFO) {
            if (key == 'q' || key == 27 || key == '\r' || key == '\n') current_page = Page::MAIN_MENU;
        }
    }

    return 0;
}
