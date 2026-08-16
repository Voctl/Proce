#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <ncurses.h>
#include <signal.h>

#define NAME_MAX_LEN 15
#define FILTER_MAX 128

typedef struct {
    int pid;
    unsigned long rss_kb;
    char name[NAME_MAX_LEN + 1];
} Process;

typedef struct {
    int sort_mode;
    char filter[FILTER_MAX];
    int filter_len;
    int filter_active;
    int selected;
    int interval_idx;
    int show_help;
} UIState;

enum { SORT_RSS, SORT_PID, SORT_NAME, SORT_COUNT };

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

enum {
    KEY_ESC = 27,
    KEY_BS1 = 127,
    KEY_BS2 = 8,
    MIN_COLS = 50,
    MIN_ROWS = 5,
    RSS_GIB = 1048576,
    RSS_MIB = 1024,
    ASCII_MIN = 32,
    ASCII_MAX = 126
};

enum {
    CP_GREEN = 1,
    CP_CYAN,
    CP_YELLOW,
    CP_RED,
    CP_WHITE,
    CP_HIGHLIGHT
};

static const double intervals[] = {0.5, 1.0, 2.0, 5.0};

static int parse_proc(int pid, unsigned long *rss, char *name) {
    char path[32], line[128];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *fp = fopen(path, "r");
    if (unlikely(!fp)) return -1;
    *rss = 0;
    name[0] = '\0';
    int found = 0;
    while (fgets(line, sizeof(line), fp) && found < 2) {
        if (strncmp(line, "Name:", 5) == 0) {
            char *p = line + 5;
            while (*p == ' ' || *p == '\t') p++;
            int i = 0;
            while (p[i] && p[i] != '\n' && p[i] != '\t' && i < NAME_MAX_LEN) {
                name[i] = p[i];
                i++;
            }
            name[i] = '\0';
            found++;
        } else if (strncmp(line, "VmRSS:", 6) == 0) {
            char *p = line + 6;
            while (*p == ' ' || *p == '\t') p++;
            unsigned long v = 0;
            while (*p >= '0' && *p <= '9') {
                v = v * 10 + (*p - '0');
                p++;
            }
            *rss = v;
            found++;
        }
    }
    fclose(fp);
    return 0;
}

static int cmp_rss_desc(const void *a, const void *b) {
    unsigned long ra = ((const Process *)a)->rss_kb;
    unsigned long rb = ((const Process *)b)->rss_kb;
    return (rb > ra) - (rb < ra);
}

static int cmp_pid_asc(const void *a, const void *b) {
    int pa = ((const Process *)a)->pid;
    int pb = ((const Process *)b)->pid;
    return (pa > pb) - (pa < pb);
}

static int cmp_name_asc(const void *a, const void *b) {
    return strcmp(((const Process *)a)->name, ((const Process *)b)->name);
}

static int (* const cmp_funcs[])(const void *, const void *) = {
    cmp_rss_desc, cmp_pid_asc, cmp_name_asc
};
static const char * const sort_labels[] = {"RSS", "PID", "Name"};

static void fmt_mem(char *buf, size_t sz, unsigned long kb) {
    int n;
    if (kb >= RSS_GIB)
        n = snprintf(buf, sz, "%.2f GiB", kb / (double)RSS_GIB);
    else if (kb >= RSS_MIB)
        n = snprintf(buf, sz, "%.1f MiB", kb / (double)RSS_MIB);
    else
        n = snprintf(buf, sz, "%lu KiB", kb);
    (void)n;
}

int main(void) {
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    timeout(1000);

    if (!has_colors()) {
        endwin();
        fprintf(stderr, "Terminal does not support colors\n");
        return 1;
    }
    start_color();
    use_default_colors();
    static const struct { int fg, bg; } cpairs[] = {
        {COLOR_GREEN, -1}, {COLOR_CYAN, -1}, {COLOR_YELLOW, -1},
        {COLOR_RED, -1}, {COLOR_WHITE, -1}, {COLOR_BLACK, COLOR_CYAN}
    };
    for (int i = 0; i < (int)ARRAY_SIZE(cpairs); i++)
        init_pair(i + 1, cpairs[i].fg, cpairs[i].bg);

    Process *procs = NULL;
    int cap = 0;

    UIState ui = {
        .sort_mode = SORT_RSS,
        .filter = "",
        .filter_len = 0,
        .filter_active = 0,
        .selected = 0,
        .interval_idx = 1,
        .show_help = 0
    };

    while (1) {
        int maxy, maxx;
        getmaxyx(stdscr, maxy, maxx);
        if (unlikely(maxx < MIN_COLS || maxy < MIN_ROWS)) {
            erase();
            mvprintw(0, 0, "Terminal too small (%dx%d), need at least %dx%d", maxx, maxy, MIN_COLS, MIN_ROWS);
            refresh();
            if (getch() == 'q') break;
            continue;
        }

        DIR *dp = opendir("/proc");
        if (unlikely(!dp)) break;

        struct dirent *entry;
        int n = 0;
        unsigned long total_ram = 0;
        while ((entry = readdir(dp))) {
            int pid = (int)strtol(entry->d_name, NULL, 10);
            if (pid <= 0) continue;
            if (n >= cap) {
                cap = cap ? cap * 2 : 64;
                Process *tmp = realloc(procs, cap * sizeof(Process));
                if (unlikely(!tmp)) break;
                procs = tmp;
            }
            if (parse_proc(pid, &procs[n].rss_kb, procs[n].name) == 0) {
                procs[n].pid = pid;
                total_ram += procs[n].rss_kb;
                n++;
            }
        }
        closedir(dp);

        qsort(procs, n, sizeof(Process), cmp_funcs[ui.sort_mode]);

        int display_n = 0;
        int filter_active = ui.filter[0] != '\0';
        for (int i = 0; i < n; i++) {
            if (procs[i].rss_kb == 0) continue;
            if (filter_active && !strstr(procs[i].name, ui.filter)) continue;
            if (i != display_n)
                procs[display_n] = procs[i];
            display_n++;
        }

        if (ui.selected >= display_n) ui.selected = display_n ? display_n - 1 : 0;
        if (ui.selected < 0) ui.selected = 0;

        timeout((int)(intervals[ui.interval_idx] * 1000));

        int ch = 0;
        while (1) {
            ch = getch();
            if (likely(ch == ERR)) break;

            if (ui.filter_active) {
                if (ch == KEY_ESC) {
                    ui.filter_active = 0;
                    ui.filter[0] = '\0';
                    ui.filter_len = 0;
                } else if (ch == '\n' || ch == KEY_ENTER) {
                    ui.filter_active = 0;
                } else if (ch == KEY_BACKSPACE || ch == KEY_BS1 || ch == KEY_BS2) {
                    if (ui.filter_len > 0) {
                        ui.filter[--ui.filter_len] = '\0';
                    }
                } else if (ch >= ASCII_MIN && ch <= ASCII_MAX) {
                    if (ui.filter_len < (int)sizeof(ui.filter) - 2) {
                        ui.filter[ui.filter_len++] = (char)ch;
                        ui.filter[ui.filter_len] = '\0';
                    }
                }
                break;
            }

            if (ch == 'q') {
                free(procs);
                endwin();
                return 0;
            }
            if (ch == KEY_UP) {
                if (ui.selected > 0) ui.selected--;
                break;
            }
            if (ch == KEY_DOWN || ch == 'j') {
                if (ui.selected < display_n - 1) ui.selected++;
                break;
            }
            if (ch == 's') {
                ui.sort_mode = (ui.sort_mode + 1) % SORT_COUNT;
                break;
            }
            if (ch == '/') {
                ui.filter_active = 1;
                ui.filter[0] = '\0';
                ui.filter_len = 0;
                curs_set(1);
                break;
            }
            if (ch == 'k' && display_n > 0) {
                int target = procs[ui.selected].pid;
                curs_set(1);
                mvprintw(maxy - 1, 0, " Kill PID %d? (y/N): ", target);
                clrtoeol();
                refresh();
                int ans = getch();
                curs_set(0);
                if (ans == 'y' || ans == 'Y')
                    kill(target, SIGTERM);
                break;
            }
            if (ch == '+' || ch == '=') {
                if (ui.interval_idx < (int)ARRAY_SIZE(intervals) - 1) ui.interval_idx++;
                break;
            }
            if (ch == '-') {
                if (ui.interval_idx > 0) ui.interval_idx--;
                break;
            }
            if (ch == KEY_RESIZE) {
                if (ui.filter_active) curs_set(0);
                break;
            }
            if (ch == '?') {
                ui.show_help = !ui.show_help;
                break;
            }
        }

        if (ch == KEY_RESIZE) continue;

        erase();

        char mem_fmt[24];
        fmt_mem(mem_fmt, sizeof(mem_fmt), total_ram);

        attrset(COLOR_PAIR(CP_GREEN) | A_BOLD);
        mvprintw(0, 0, "\u250c ProcLens ");
        int hdr_len = 12;
        hdr_len += mvprintw(0, hdr_len, "Procs: %d", display_n);
        hdr_len += mvprintw(0, hdr_len, " Total: %s", mem_fmt);
        hdr_len += mvprintw(0, hdr_len, " Sort: %s", sort_labels[ui.sort_mode]);
        mvprintw(0, hdr_len, " %.1fs", intervals[ui.interval_idx]);
        attrset(COLOR_PAIR(CP_YELLOW));
        mvprintw(0, maxx - 11, "[q] quit");
        attrset(A_NORMAL);

        mvhline(1, 0, ACS_HLINE, maxx);
        mvaddch(1, 0, ACS_LTEE);
        mvaddch(1, maxx - 1, ACS_RTEE);

        attrset(COLOR_PAIR(CP_CYAN) | A_BOLD);
        mvprintw(1, 2, "%-7s %11s  %s", "PID", "RSS", "NAME");
        attrset(A_NORMAL);

        mvhline(2, 0, ACS_HLINE, maxx);
        mvaddch(2, 0, ACS_LTEE);
        mvaddch(2, maxx - 1, ACS_RTEE);

        int row = 3;
        int avail_rows = maxy - 1;
        for (int i = 0; i < display_n && row < avail_rows; i++) {
            if (i == ui.selected) {
                attrset(COLOR_PAIR(CP_HIGHLIGHT));
            } else if (i % 2 == 0) {
                attrset(COLOR_PAIR(CP_WHITE));
            } else {
                attrset(COLOR_PAIR(CP_YELLOW));
            }

            if (i != ui.selected && procs[i].rss_kb > RSS_GIB)
                attrset(COLOR_PAIR(CP_RED) | A_BOLD);

            char mem[24];
            fmt_mem(mem, sizeof(mem), procs[i].rss_kb);
            mvprintw(row, 2, "%-7d %11s  %s", procs[i].pid, mem, procs[i].name);
            row++;
        }

        attrset(A_NORMAL);
        mvhline(avail_rows, 0, ACS_HLINE, maxx);
        mvaddch(avail_rows, 0, ACS_LLCORNER);
        mvaddch(avail_rows, maxx - 1, ACS_LRCORNER);
        mvaddch(0, 0, ACS_ULCORNER);
        mvaddch(0, maxx - 1, ACS_URCORNER);

        for (int r = 2; r < avail_rows; r++) {
            mvaddch(r, 0, ACS_VLINE);
            mvaddch(r, maxx - 1, ACS_VLINE);
        }

        if (ui.filter_active) {
            curs_set(1);
            mvprintw(avail_rows, 0, " Filter: %s", ui.filter);
            clrtoeol();
        }

        if (ui.show_help) {
            static const char * const help_lines[] = {
                " [?] Toggle help",  " [j/Down] Move down",
                " [Up]     Move up", " [s]      Cycle sort",
                " [/]      Filter",  " [+/-]    Change interval",
                " [k]      Kill process", " [q]      Quit"
            };
            int hy = maxy / 2 - 3;
            int hx = maxx / 2 - 18;
            attrset(COLOR_PAIR(CP_HIGHLIGHT));
            for (int r = hy; r < hy + 9 && r < avail_rows; r++)
                mvhline(r, hx, ' ', 36);
            for (int i = 0; i < (int)ARRAY_SIZE(help_lines); i++)
                mvprintw(hy + i, hx, "%s", help_lines[i]);
            attrset(A_NORMAL);
        }

        refresh();
    }

    free(procs);
    endwin();
    return 0;
}
