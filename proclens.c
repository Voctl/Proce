#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <ncurses.h>
#include <signal.h>

#define NAME_MAX_LEN 15
#define FILTER_MAX 64

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

static volatile sig_atomic_t running = 1;

static void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

static const double intervals[] = {0.5, 1.0, 2.0, 5.0};

static int parse_proc(int pid, unsigned long *restrict rss, char *restrict name) {
    char path[24], line[96];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *fp = fopen(path, "r");
    if (unlikely(!fp)) return -1;
    *rss = 0;
    name[0] = '\0';
    int found = 0;
    while (fgets(line, sizeof(line), fp) && found < 2) {
        unsigned long tmp;
        if (sscanf(line, "Name: %15s", name) == 1)
            found++;
        else if (sscanf(line, "VmRSS: %lu", &tmp) == 1) {
            *rss = tmp;
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
    if (kb >= RSS_GIB)
        snprintf(buf, sz, "%.2f GiB", (double)kb / (double)RSS_GIB);
    else if (kb >= RSS_MIB)
        snprintf(buf, sz, "%.1f MiB", (double)kb / (double)RSS_MIB);
    else
        snprintf(buf, sz, "%lu KiB", kb);
}

static void init_ncurses(void) {
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    timeout(1000);

    if (!has_colors()) {
        endwin();
        fprintf(stderr, "Terminal does not support colors\n");
        exit(1);
    }
    start_color();
    use_default_colors();
    static const struct { int fg, bg; } cpairs[] = {
        {COLOR_GREEN, -1}, {COLOR_CYAN, -1}, {COLOR_YELLOW, -1},
        {COLOR_RED, -1}, {COLOR_WHITE, -1}, {COLOR_BLACK, COLOR_CYAN}
    };
    for (int i = 0; i < (int)ARRAY_SIZE(cpairs); i++)
        init_pair((short)(i + 1), (short)cpairs[i].fg, (short)cpairs[i].bg);
}

typedef struct {
    int count;
    unsigned long total_ram;
} ScanResult;

static ScanResult scan_processes(Process **procs, int *cap, const UIState *ui) {
    ScanResult result = {0, 0};
    DIR *dp = opendir("/proc");
    if (unlikely(!dp)) return result;

    struct dirent *entry;
    while ((entry = readdir(dp))) {
        const char *d = entry->d_name;
        if (d[0] < '1' || d[0] > '9') continue;
        int pid = (int)strtol(d, NULL, 10);
        if (pid <= 0) continue;
        if (result.count >= *cap) {
            *cap = *cap ? *cap * 2 : 64;
            Process *tmp = realloc(*procs, (size_t)*cap * sizeof(Process));
            if (unlikely(!tmp)) break;
            *procs = tmp;
        }
        if (parse_proc(pid, &(*procs)[result.count].rss_kb, (*procs)[result.count].name) == 0) {
            if ((*procs)[result.count].rss_kb == 0) continue;
            (*procs)[result.count].pid = pid;
            result.total_ram += (*procs)[result.count].rss_kb;
            result.count++;
        }
    }
    closedir(dp);

    qsort(*procs, (size_t)result.count, sizeof(Process), cmp_funcs[ui->sort_mode]);

    int display_n = 0;
    int filter_active = ui->filter[0] != '\0';
    for (int i = 0; i < result.count; i++) {
        if (filter_active && !strstr((*procs)[i].name, ui->filter)) continue;
        if (display_n != i)
            memmove(&(*procs)[display_n], &(*procs)[i], sizeof(Process));
        display_n++;
    }
    result.count = display_n;

    return result;
}

enum { INPUT_NONE, INPUT_BREAK, INPUT_CONTINUE, INPUT_QUIT };

static int handle_input(UIState *ui, const Process *procs, int display_n, int maxy) {
    int ch = 0;
    while (1) {
        ch = getch();
        if (likely(ch == ERR)) return INPUT_CONTINUE;

        if (ui->filter_active) {
            if (ch == KEY_ESC) {
                ui->filter_active = 0;
                ui->filter[0] = '\0';
                ui->filter_len = 0;
            } else if (ch == '\n' || ch == KEY_ENTER) {
                ui->filter_active = 0;
            } else if (ch == KEY_BACKSPACE || ch == KEY_BS1 || ch == KEY_BS2) {
                if (ui->filter_len > 0) {
                    ui->filter[--ui->filter_len] = '\0';
                }
            } else if (ch >= ASCII_MIN && ch <= ASCII_MAX) {
                if (ui->filter_len < (int)sizeof(ui->filter) - 1) {
                    ui->filter[ui->filter_len++] = (char)ch;
                    ui->filter[ui->filter_len] = '\0';
                }
            }
            return INPUT_BREAK;
        }

        if (ch == 'q')
            return INPUT_QUIT;
        if (ch == KEY_UP) {
            if (ui->selected > 0) ui->selected--;
            return INPUT_BREAK;
        }
        if (ch == KEY_DOWN || ch == 'j') {
            if (ui->selected < display_n - 1) ui->selected++;
            return INPUT_BREAK;
        }
        if (ch == 's') {
            ui->sort_mode = (ui->sort_mode + 1) % SORT_COUNT;
            return INPUT_BREAK;
        }
        if (ch == '/') {
            ui->filter_active = 1;
            ui->filter[0] = '\0';
            ui->filter_len = 0;
            curs_set(1);
            return INPUT_BREAK;
        }
        if (ch == 'k' && display_n > 0) {
            int target = procs[ui->selected].pid;
            curs_set(1);
            mvprintw(maxy - 1, 0, " Kill PID %d? (y/N): ", target);
            clrtoeol();
            refresh();
            int ans = getch();
            curs_set(0);
            if (ans == 'y' || ans == 'Y')
                kill(target, SIGTERM);
            return INPUT_BREAK;
        }
        if (ch == '+' || ch == '=') {
            if (ui->interval_idx < (int)ARRAY_SIZE(intervals) - 1) ui->interval_idx++;
            return INPUT_BREAK;
        }
        if (ch == '-') {
            if (ui->interval_idx > 0) ui->interval_idx--;
            return INPUT_BREAK;
        }
        if (ch == KEY_RESIZE) {
            if (ui->filter_active) curs_set(0);
            return INPUT_BREAK;
        }
        if (ch == '?') {
            ui->show_help = !ui->show_help;
            return INPUT_BREAK;
        }
    }
}

static void render_ui(const Process *procs, int display_n, unsigned long total_ram,
                      const UIState *ui, int maxy, int maxx) {
    erase();

    char mem_fmt[24];
    fmt_mem(mem_fmt, sizeof(mem_fmt), total_ram);

    attrset(COLOR_PAIR(CP_GREEN) | A_BOLD);
    mvprintw(0, 0, "\u250c ProcLens ");
    int hdr_len = 12;
    hdr_len += mvprintw(0, hdr_len, "Procs: %d", display_n);
    hdr_len += mvprintw(0, hdr_len, " Total: %s", mem_fmt);
    hdr_len += mvprintw(0, hdr_len, " Sort: %s", sort_labels[ui->sort_mode]);
    mvprintw(0, hdr_len, " %.1fs", intervals[ui->interval_idx]);
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
        if (i == ui->selected) {
            attrset(COLOR_PAIR(CP_HIGHLIGHT));
        } else if (i % 2 == 0) {
            attrset(COLOR_PAIR(CP_WHITE));
        } else {
            attrset(COLOR_PAIR(CP_YELLOW));
        }

        if (i != ui->selected && procs[i].rss_kb > RSS_GIB)
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
    for (int r = 1; r < avail_rows; r++) {
        mvaddch(r, 0, ACS_VLINE);
        mvaddch(r, maxx - 1, ACS_VLINE);
    }
    mvaddch(0, 0, ACS_ULCORNER);
    mvaddch(0, maxx - 1, ACS_URCORNER);

    if (ui->filter_active) {
        curs_set(1);
        mvprintw(avail_rows, 0, " Filter: %s", ui->filter);
        clrtoeol();
    }

    if (ui->show_help) {
        static const char * const help_lines[] = {
            " [?] Toggle help",  " [j/Down] Move down",
            " [Up]     Move up", " [s]      Cycle sort",
            " [/]      Filter",  " [+/-]    Change interval",
            " [k]      Kill process", " [q]      Quit"
        };
        enum { HELP_COUNT = 8 };
        int hy = maxy / 2 - HELP_COUNT / 2;
        int hx = maxx / 2 - 18;
        attrset(COLOR_PAIR(CP_HIGHLIGHT));
        for (int r = hy; r < hy + HELP_COUNT + 1 && r < avail_rows; r++)
            mvhline(r, hx, ' ', 36);
        for (int i = 0; i < HELP_COUNT; i++)
            mvprintw(hy + i, hx, "%s", help_lines[i]);
        attrset(A_NORMAL);
    }

    refresh();
}

int main(void) {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    init_ncurses();

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

    while (running) {
        int maxy, maxx;
        getmaxyx(stdscr, maxy, maxx);
        if (unlikely(maxx < MIN_COLS || maxy < MIN_ROWS)) {
            erase();
            mvprintw(0, 0, "Terminal too small (%dx%d), need at least %dx%d", maxx, maxy, MIN_COLS, MIN_ROWS);
            refresh();
            if (getch() == 'q') break;
            continue;
        }

        ScanResult scan = scan_processes(&procs, &cap, &ui);
        int display_n = scan.count;
        unsigned long total_ram = scan.total_ram;

        if (ui.selected >= display_n) ui.selected = display_n ? display_n - 1 : 0;
        if (ui.selected < 0) ui.selected = 0;

        timeout((int)(intervals[ui.interval_idx] * 1000));

        int action = handle_input(&ui, procs, display_n, maxy);

        if (action == INPUT_QUIT) break;
        if (action == INPUT_BREAK && ui.filter_active == 0) {
            /* check if it was a resize */
            continue;
        }

        if (action == INPUT_BREAK) continue;

        render_ui(procs, display_n, total_ram, &ui, maxy, maxx);
    }

    free(procs);
    endwin();
    return 0;
}
