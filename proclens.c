#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <ncurses.h>
#include <signal.h>

#define NAME_MAX_LEN 15
#define FILTER_MAX 64
#define VERSION "1.0.0"

typedef struct {
    int pid;
    unsigned long rss_kb;
    char name[NAME_MAX_LEN + 1];
} Process;

typedef struct {
    char filter[FILTER_MAX];
    int sort_mode;
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

static const char *match_key(const char *line, const char *key) {
    while (*key != '\0') {
        if (*line++ != *key++) return NULL;
    }
    return line;
}

static int parse_proc(int pid, unsigned long *restrict rss, char *restrict name) {
    char path[24], line[96];
    memcpy(path, "/proc/", 6);
    char *w = path + 6;
    unsigned u = (unsigned)pid;
    char digits[8];
    int nd = 0;
    do {
        digits[nd++] = (char)('0' + (u % 10u));
        u /= 10u;
    } while (u != 0);
    while (nd > 0)
        *w++ = digits[--nd];
    memcpy(w, "/status", 8);
    FILE *fp = fopen(path, "r");
    if (unlikely(!fp)) return -1;
    *rss = 0;
    name[0] = '\0';
    int found = 0;
    while (fgets(line, sizeof(line), fp) && found < 2) {
        const char *v = match_key(line, "Name:");
        if (v != NULL) {
            while (*v == ' ' || *v == '\t') v++;
            int n = 0;
            while (n < NAME_MAX_LEN && v[n] != '\0' && v[n] != '\n') {
                name[n] = v[n];
                n++;
            }
            if (n > 0) {
                name[n] = '\0';
                found++;
            }
        } else {
            const char *vs = match_key(line, "VmRSS:");
            if (vs != NULL) {
                char *end;
                unsigned long kb = strtoul(vs, &end, 10);
                if (end != vs) {
                    *rss = kb;
                    found++;
                }
            }
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
    set_escdelay(25);

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

    Process *arr = *procs;
    struct dirent *entry;
    while ((entry = readdir(dp))) {
        const char *d = entry->d_name;
        if (d[0] < '1' || d[0] > '9') continue;
        int pid = (int)strtol(d, NULL, 10);
        if (pid <= 0) continue;
        if (result.count >= *cap) {
            *cap = *cap ? *cap * 2 : 64;
            Process *tmp = realloc(arr, (size_t)*cap * sizeof(Process));
            if (unlikely(!tmp)) break;
            arr = tmp;
        }
        Process *p = &arr[result.count];
        if (parse_proc(pid, &p->rss_kb, p->name) == 0 && p->rss_kb != 0) {
            p->pid = pid;
            result.total_ram += p->rss_kb;
            result.count++;
        }
    }
    closedir(dp);
    *procs = arr;

    qsort(*procs, (size_t)result.count, sizeof(Process), cmp_funcs[ui->sort_mode]);

    int display_n = 0;
    int filter_active = ui->filter[0] != '\0';
    for (int i = 0; i < result.count; i++) {
        if (filter_active && !strstr((*procs)[i].name, ui->filter)) continue;
        if (display_n != i)
            (*procs)[display_n] = (*procs)[i];
        display_n++;
    }
    result.count = display_n;

    return result;
}

enum { INPUT_NONE, INPUT_BREAK, INPUT_CONTINUE, INPUT_QUIT, INPUT_REDRAW };

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
            return INPUT_REDRAW;
        }

        switch (ch) {
        case 'q':
            return INPUT_QUIT;
        case KEY_UP:
            if (ui->selected > 0) ui->selected--;
            return INPUT_REDRAW;
        case KEY_DOWN:
        case 'j':
            if (ui->selected < display_n - 1) ui->selected++;
            return INPUT_REDRAW;
        case 's':
            ui->sort_mode = (ui->sort_mode + 1) % SORT_COUNT;
            return INPUT_BREAK;
        case '/':
            ui->filter_active = 1;
            ui->filter[0] = '\0';
            ui->filter_len = 0;
            curs_set(1);
            return INPUT_REDRAW;
        case 'k':
            if (display_n > 0) {
                int target = procs[ui->selected].pid;
                curs_set(1);
                mvprintw(maxy - 1, 0, " Kill PID %d? (y/N): ", target);
                clrtoeol();
                refresh();
                int ans = getch();
                curs_set(0);
                if (ans == 'y' || ans == 'Y')
                    kill(target, SIGTERM);
            }
            return INPUT_BREAK;
        case '+':
        case '=':
            if (ui->interval_idx < (int)ARRAY_SIZE(intervals) - 1)
                ui->interval_idx++;
            return INPUT_REDRAW;
        case '-':
            if (ui->interval_idx > 0) ui->interval_idx--;
            return INPUT_REDRAW;
        case KEY_RESIZE:
            if (ui->filter_active) curs_set(0);
            return INPUT_BREAK;
        case '?':
            ui->show_help = !ui->show_help;
            return INPUT_REDRAW;
        }
    }
}

static void render_ui(const Process *procs, int display_n, unsigned long total_ram,
                      const UIState *ui, int maxy, int maxx) {
    erase();

    char mem_fmt[24];
    fmt_mem(mem_fmt, sizeof(mem_fmt), total_ram);

    attrset(COLOR_PAIR(CP_GREEN) | A_BOLD);
    char hdr[128];
    int hdr_len = snprintf(hdr, sizeof(hdr),
                           "\u250c ProcLens v%s Procs: %d Total: %s Sort: %s %.1fs",
                           VERSION, display_n, mem_fmt,
                           sort_labels[ui->sort_mode], intervals[ui->interval_idx]);
    if (hdr_len > maxx - 12) hdr_len = maxx - 12;
    if (hdr_len < 0) hdr_len = 0;
    mvprintw(0, 0, "%.*s", hdr_len, hdr);
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
    char mem[24];
    for (int i = 0; i < display_n && row < avail_rows; i++) {
        const Process *p = &procs[i];
        int attr;
        if (i == ui->selected)
            attr = COLOR_PAIR(CP_HIGHLIGHT);
        else if (p->rss_kb > RSS_GIB)
            attr = COLOR_PAIR(CP_RED) | A_BOLD;
        else
            attr = (i % 2 == 0) ? COLOR_PAIR(CP_WHITE)
                                : COLOR_PAIR(CP_YELLOW);
        attrset(attr);

        fmt_mem(mem, sizeof(mem), p->rss_kb);
        mvprintw(row, 2, "%-7d %11s  %s", p->pid, mem, p->name);
        row++;
    }

    attrset(A_NORMAL);
    mvhline(avail_rows, 0, ACS_HLINE, maxx);
    mvaddch(avail_rows, 0, ACS_LLCORNER);
    mvaddch(avail_rows, maxx - 1, ACS_LRCORNER);
    if (avail_rows > 1) {
        mvvline(1, 0, ACS_VLINE, avail_rows - 1);
        mvvline(1, maxx - 1, ACS_VLINE, avail_rows - 1);
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
        if (hy < 1) hy = 1;
        int hx = maxx / 2 - 18;
        if (hx < 2) hx = 2;
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
    struct sigaction sa = { .sa_handler = handle_signal };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    init_ncurses();

    Process *procs = NULL;
    int cap = 0;

    UIState ui = {
        .sort_mode = SORT_RSS,
        .interval_idx = 1
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

        static int last_interval = -1;
        if (ui.interval_idx != last_interval) {
            last_interval = ui.interval_idx;
            timeout((int)(intervals[ui.interval_idx] * 1000));
        }

        int action = handle_input(&ui, procs, display_n, maxy);

        if (action == INPUT_QUIT) break;
        if (action == INPUT_REDRAW) {
            render_ui(procs, display_n, total_ram, &ui, maxy, maxx);
            continue;
        }
        if (action == INPUT_BREAK) continue;

        render_ui(procs, display_n, total_ram, &ui, maxy, maxx);
    }

    free(procs);
    endwin();
    return 0;
}
