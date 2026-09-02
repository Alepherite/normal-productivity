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

volatile sig_atomic_t g_winch = 0;

void sigwinch_handler(int) {
    g_winch = 1;
}

struct TerminalSession {
    static inline termios orig_termios;

    static void restore() {
        std::cout << "\033[0m\033[?25h\033[?1049l" << std::flush;
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

enum class Page { MAIN_MENU, SCHEDULE, POMODORO, POMODORO_RUN, INFO };

const int MENU_SIZE = 3;
const std::array<std::string, MENU_SIZE> MENU_ITEMS = { "Schedule", "Pomodoro", "Info" };

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

void render_ui(Page current_page, int sel_idx, int sched_sel_c, const std::tm& current_week_sun, const std::tm& today, 
               int work_time, int break_time, int time_remaining, bool is_work_phase) {
    int rows, cols;
    get_term_size(rows, cols);
    std::string out = "\033[H"; 

    if (current_page == Page::MAIN_MENU) {
        int menu_width = 20;
        int total_height = MENU_SIZE * 4 - 1;
        int top_pad = std::max(0, (rows - total_height) / 2);
        int left_pad = std::max(0, (cols - menu_width) / 2);
        std::string lm(left_pad, ' ');

        for (int i = 0; i < top_pad; ++i) out += "\033[K\n";

        for (int r = 0; r < MENU_SIZE; ++r) {
            bool is_selected = (r == sel_idx);
            std::string label = center_text(MENU_ITEMS[r], 18);
            std::string rev = is_selected ? "\033[7m" : "";
            std::string res = "\033[0m";

            out += lm + rev + "┌──────────────────┐" + res + "\033[K\n";
            out += lm + rev + "│" + label + "│" + res + "\033[K\n";
            out += lm + rev + "└──────────────────┘" + res + "\033[K\n";
            if (r < MENU_SIZE - 1) out += "\033[K\n";
        }
    } 
    else if (current_page == Page::SCHEDULE) {
        int cell_w = 8;
        int sched_width = 7 * cell_w + 8;
        int total_height = 14; 
        int top_pad = std::max(0, (rows - total_height) / 2);
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
        
        const std::array<std::string, 7> day_names = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

        for (int c = 0; c < 7; ++c) {
            std::tm day = add_days(current_week_sun, c);
            bool is_today = is_same_day(day, today);
            bool is_selected = (c == sched_sel_c);

            char date_buf[16];
            std::strftime(date_buf, sizeof(date_buf), "%d/%m", &day);
            
            std::string name_str = center_text(day_names[c], cell_w);
            std::string date_str = center_text(std::string(date_buf), cell_w);

            std::string color = is_today ? "\033[31m" : ""; 
            if (is_selected) color = "\033[7m" + color;
            std::string reset = "\033[0m";

            mid_names += color + name_str + reset + "│";
            mid_dates += color + date_str + reset + "│";
        }
        mid_names += "\033[K\n";
        mid_dates += "\033[K\n";

        std::string empty_row = line_builder("│", "│", "│", " ");

        out += top_border + mid_names + mid_dates + sep_border;
        for(int i = 0; i < 9; ++i) out += empty_row; 
        out += bot_border;
    }
    else if (current_page == Page::POMODORO) {
        int pomo_width = 24;
        int top_pad = std::max(0, (rows - 7) / 2);
        int left_pad = std::max(0, (cols - pomo_width) / 2);
        std::string lm(left_pad, ' ');

        for (int i = 0; i < top_pad; ++i) out += "\033[K\n";

        std::array<std::string, 4> pomo_items = {
            "Start Timer",
            "Reset Defaults",
            "Work: < " + std::to_string(work_time) + " > min",
            "Break: < " + std::to_string(break_time) + " > min"
        };

        for (int i = 0; i < 4; ++i) {
            bool is_selected = (i == sel_idx);
            std::string label = center_text(pomo_items[i], pomo_width);
            if (is_selected) out += lm + "\033[7m" + label + "\033[0m\033[K\n\033[K\n";
            else out += lm + label + "\033[K\n\033[K\n";
        }
    }
    else if (current_page == Page::POMODORO_RUN) {
        int bar_w = 40; // Chiều rộng của thanh tiến trình (số lượng block)
        int block_w = bar_w + 2; // Chiều rộng tổng bao gồm 2 viền trái phải
        int total_height = 8;
        
        int top_pad = std::max(0, (rows - total_height) / 2);
        int left_pad = std::max(0, (cols - block_w) / 2); 
        std::string lm(left_pad, ' ');

        for (int i = 0; i < top_pad; ++i) out += "\033[K\n";

        // Logic tính toán Progress
        int total_sec = (is_work_phase ? work_time : break_time) * 60;
        float progress = 1.0f - ((float)time_remaining / total_sec); 
        int filled_chars = std::max(0, std::min(bar_w, (int)(progress * bar_w))); // Giới hạn từ 0 đến bar_w
        
        // Chuẩn bị Text
        std::string phase_txt = is_work_phase ? "WORK PHASE" : "BREAK PHASE";
        std::string color = is_work_phase ? "\033[31;1m" : "\033[32;1m"; 
        
        int h = time_remaining / 3600;
        int m = (time_remaining % 3600) / 60;
        int s = time_remaining % 60;
        char time_buf[16];
        std::snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d", h, m, s);
        std::string time_str = std::string(time_buf);

        // Tính toán khoảng đệm để căn giữa (bỏ qua escape sequences khi đếm)
        int p_l = std::max(0, (block_w - (int)phase_txt.length()) / 2);
        int p_r = std::max(0, block_w - (int)phase_txt.length() - p_l);
        
        int t_l = std::max(0, (block_w - (int)time_str.length()) / 2);
        int t_r = std::max(0, block_w - (int)time_str.length() - t_l);

        // Render Phần Header (Text + Clock)
        out += lm + std::string(p_l, ' ') + color + phase_txt + "\033[0m" + std::string(p_r, ' ') + "\033[K\n";
        out += lm + std::string(t_l, ' ') + "\033[1m" + time_str + "\033[0m" + std::string(t_r, ' ') + "\033[K\n\033[K\n";

        // Render Phần Progress Bar (Hộp box drawing + Khối đặc)
        std::string bar_top = "┌"; for(int i=0; i<bar_w; ++i) bar_top += "─"; bar_top += "┐";
        std::string bar_bot = "└"; for(int i=0; i<bar_w; ++i) bar_bot += "─"; bar_bot += "┘";
        
        std::string bar_mid = "│" + color;
        for (int i = 0; i < filled_chars; ++i) bar_mid += "█";
        bar_mid += "\033[0m"; // Ngắt màu tại đây để đoạn rỗng trở về default
        for (int i = filled_chars; i < bar_w; ++i) bar_mid += " ";
        bar_mid += "│";

        out += lm + bar_top + "\033[K\n";
        out += lm + bar_mid + "\033[K\n";
        out += lm + bar_bot + "\033[K\n\033[K\n";

        // Căn lề thủ công cho Footer tránh lỗi offset do escape code
        std::string msg = "Press [q] to stop";
        int m_l = std::max(0, (block_w - (int)msg.length()) / 2);
        int m_r = std::max(0, block_w - (int)msg.length() - m_l);
        out += lm + std::string(m_l, ' ') + "\033[2m" + msg + "\033[0m" + std::string(m_r, ' ') + "\033[K\n";
    }
    else if (current_page == Page::INFO) {
        int info_width = 44;
        int total_height = 8;
        int top_pad = std::max(0, (rows - total_height) / 2);
        int left_pad = std::max(0, (cols - info_width) / 2);
        std::string lm(left_pad, ' ');

        for (int i = 0; i < top_pad; ++i) out += "\033[K\n";

        out += lm + "\033[1m=== MINIMAL TUI INFO ===\033[0m\033[K\n\033[K\n";
        out += lm + "Navigation : [h / j / k / l] or Arrows\033[K\n";
        out += lm + "Action     : [Enter] Select\033[K\n";
        out += lm + "Adjust     : [h / l] Dec/Inc time\033[K\n";
        out += lm + "Quit/Back  : [q / ESC / Ctrl+C]\033[K\n\033[K\n";
        out += lm + "\033[2mPress [q] to return.\033[0m\033[K\n";
    }

    out += "\033[J"; 
    std::cout << out << std::flush;
}

int main() {
    TerminalSession::init();

    Page current_page = Page::MAIN_MENU;
    int main_sel_r = 0, pomo_sel = 0;
    
    std::time_t t = std::time(nullptr);
    std::tm today = *std::localtime(&t);
    std::tm current_week_sun = add_days(today, -today.tm_wday);
    int sched_sel_c = today.tm_wday; 

    int work_time = 25, break_time = 5;
    int time_remaining = 0;
    bool is_work_phase = true;

    render_ui(current_page, main_sel_r, sched_sel_c, current_week_sun, today, work_time, break_time, time_remaining, is_work_phase);

    while (true) {
        int timeout_ms = (current_page == Page::POMODORO_RUN) ? 1000 : -1;
        struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
        
        int ret = poll(&pfd, 1, timeout_ms);

        if (ret == -1 && errno == EINTR) {
            if (g_winch) {
                g_winch = 0;
                render_ui(current_page, (current_page == Page::MAIN_MENU) ? main_sel_r : pomo_sel, 
                          sched_sel_c, current_week_sun, today, work_time, break_time, time_remaining, is_work_phase);
                continue;
            }
        } 
        else if (ret == 0) {
            if (current_page == Page::POMODORO_RUN && time_remaining > 0) {
                time_remaining--;
                if (time_remaining == 0) { 
                    is_work_phase = !is_work_phase;
                    time_remaining = (is_work_phase ? work_time : break_time) * 60;
                    std::cout << "\a" << std::flush; 
                }
                render_ui(current_page, pomo_sel, sched_sel_c, current_week_sun, today, work_time, break_time, time_remaining, is_work_phase);
            }
            continue;
        }

        char key = 0;
        if (read(STDIN_FILENO, &key, 1) > 0) {
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
                } else key = 'q'; 
            }
        }

        if (key == 3) break; 

        if (current_page == Page::MAIN_MENU) {
            if (key == 'q') break;
            switch (key) {
                case 'k': if (main_sel_r > 0) main_sel_r--; break;
                case 'j': if (main_sel_r < MENU_SIZE - 1) main_sel_r++; break;
                case '\r': case '\n': case ' ':
                    if (main_sel_r == 0) current_page = Page::SCHEDULE;
                    else if (main_sel_r == 1) current_page = Page::POMODORO;
                    else if (main_sel_r == 2) current_page = Page::INFO;
                    break;
            }
        } 
        else if (current_page == Page::SCHEDULE) {
            if (key == 'q') current_page = Page::MAIN_MENU;
            else if (key == 'h') {
                if (sched_sel_c > 0) sched_sel_c--;
                else { sched_sel_c = 6; current_week_sun = add_days(current_week_sun, -7); }
            }
            else if (key == 'l') {
                if (sched_sel_c < 6) sched_sel_c++;
                else { sched_sel_c = 0; current_week_sun = add_days(current_week_sun, 7); }
            }
        }
        else if (current_page == Page::POMODORO) {
            if (key == 'q') current_page = Page::MAIN_MENU;
            else {
                switch (key) {
                    case 'k': if (pomo_sel > 0) pomo_sel--; break;
                    case 'j': if (pomo_sel < 3) pomo_sel++; break;
                    case 'h': 
                        if (pomo_sel == 2 && work_time > 1) work_time--;
                        if (pomo_sel == 3 && break_time > 1) break_time--;
                        break;
                    case 'l': 
                        if (pomo_sel == 2 && work_time < 99) work_time++;
                        if (pomo_sel == 3 && break_time < 99) break_time++;
                        break;
                    case '\r': case '\n': case ' ':
                        if (pomo_sel == 0) { 
                            current_page = Page::POMODORO_RUN;
                            is_work_phase = true;
                            time_remaining = work_time * 60;
                        } else if (pomo_sel == 1) { 
                            work_time = 25; break_time = 5;
                        }
                        break;
                }
            }
        }
        else if (current_page == Page::POMODORO_RUN) {
            if (key == 'q') current_page = Page::POMODORO; 
        }
        else if (current_page == Page::INFO) {
            if (key == 'q' || key == '\r' || key == '\n') current_page = Page::MAIN_MENU;
        }

        render_ui(current_page, (current_page == Page::MAIN_MENU) ? main_sel_r : pomo_sel, 
                  sched_sel_c, current_week_sun, today, work_time, break_time, time_remaining, is_work_phase);
    }

    return 0;
}
