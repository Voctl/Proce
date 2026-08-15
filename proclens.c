#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <ncurses.h>
#include <signal.h>

typedef struct {
    int pid;
    unsigned long rss_kb;
    char name[256];
} Process;

enum { SORT_RSS, SORT_PID, SORT_NAME };

static double intervals[] = {0.5, 1.0, 2.0, 5.0};
static int n_intervals = 4;

static int parse_proc(int pid, unsigned long *rss, char *name) {
    char path[64], line[256];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    *rss = 0;
    name[0] = '\0';
    int found = 0;
    while (fgets(line, sizeof(line), fp) && found < 2) {
        if (strncmp(line, "Name:", 5) == 0) {
            sscanf(line + 5, " %255s", name);
            found++;
        } else if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line + 6, " %lu", rss);
            found++;
        }
    }
    fclose(fp);
    return 0;
}

static int cmp_rss_desc(const void *a, const void *b) {
    unsigned long ra = ((const Process *)a)->rss_kb;
    unsigned long rb = ((const Process *)b)->rss_kb;
    if (ra < rb) return 1;
    if (ra > rb) return -1;
    return 0;
}

static int cmp_pid_asc(const void *a, const void *b) {
    int pa = ((const Process *)a)->pid;
    int pb = ((const Process *)b)->pid;
    if (pa < pb) return -1;
    if (pa > pb) return 1;
    return 0;
}

static int cmp_name_asc(const void *a, const void *b) {
    return strcmp(((const Process *)a)->name, ((const Process *)b)->name);
}

static void fmt_mem(char *buf, size_t sz, unsigned long kb) {
    if (kb >= 1048576)
        snprintf(buf, sz, "%.2f GiB", kb / 1048576.0);
    else if (kb >= 1024)
        snprintf(buf, sz, "%.1f MiB", kb / 1024.0);
    else
        snprintf(buf, sz, "%lu KiB", kb);
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
    init_pair(1, COLOR_GREEN, -1);
    init_pair(2, COLOR_CYAN, -1);
    init_pair(3, COLOR_YELLOW, -1);
    init_pair(4, COLOR_RED, -1);
    init_pair(5, COLOR_WHITE, -1);
    init_pair(6, COLOR_BLACK, COLOR_CYAN);

    Process *procs = NULL;
    int cap = 0;

    int sort_mode = SORT_RSS;
    char filter[128] = "";
    int filter_active = 0;
    int selected = 0;
    int interval_idx = 1;

    while (1) {
        int maxy, maxx;
        getmaxyx(stdscr, maxy, maxx);
        if (maxx < 50 || maxy < 5) {
            erase();
            mvprintw(0, 0, "Terminal too small (%dx%d), need at least 50x5", maxx, maxy);
            refresh();
            if (getch() == 'q') break;
            continue;
        }

        DIR *dp = opendir("/proc");
        if (!dp) break;

        struct dirent *entry;
        int n = 0;
        unsigned long total_ram = 0;
        while ((entry = readdir(dp))) {
            int pid = atoi(entry->d_name);
            if (pid <= 0) continue;
            if (n >= cap) {
                cap = n + 64;
                Process *tmp = realloc(procs, cap * sizeof(Process));
                if (!tmp) break;
                procs = tmp;
            }
            if (parse_proc(pid, &procs[n].rss_kb, procs[n].name) == 0) {
                procs[n].pid = pid;
                total_ram += procs[n].rss_kb;
                n++;
            }
        }
        closedir(dp);

        int (*cmp)(const void *, const void *) = cmp_rss_desc;
        char sort_label[8] = "RSS";
        if (sort_mode == SORT_PID)  { cmp = cmp_pid_asc;  strcpy(sort_label, "PID"); }
        if (sort_mode == SORT_NAME) { cmp = cmp_name_asc; strcpy(sort_label, "Name"); }
        qsort(procs, n, sizeof(Process), cmp);

        int display_n = 0;
        for (int i = 0; i < n; i++) {
            if (procs[i].rss_kb == 0) continue;
            if (filter[0] && !strstr(procs[i].name, filter)) continue;
            if (i != display_n)
                procs[display_n] = procs[i];
            display_n++;
        }

        if (selected >= display_n) selected = display_n ? display_n - 1 : 0;
        if (selected < 0) selected = 0;

        timeout((int)(intervals[interval_idx] * 1000));

        int ch = 0;
        while (1) {
            ch = getch();
            if (ch == ERR) break;

            if (filter_active) {
                if (ch == 27) {
                    filter_active = 0;
                    filter[0] = '\0';
                } else if (ch == '\n' || ch == KEY_ENTER) {
                    filter_active = 0;
                } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
                    int len = strlen(filter);
                    if (len > 0) filter[len - 1] = '\0';
                } else if (ch >= 32 && ch <= 126) {
                    size_t len = strlen(filter);
                    if (len < sizeof(filter) - 1) {
                        filter[len] = (char)ch;
                        filter[len + 1] = '\0';
                    }
                }
                break;
            }

            if (ch == 'q') {
                free(procs);
                endwin();
                return 0;
            }
            if (ch == KEY_UP || ch == 'k') {
                if (selected > 0) selected--;
                break;
            }
            if (ch == KEY_DOWN || ch == 'j') {
                if (selected < display_n - 1) selected++;
                break;
            }
            if (ch == 's') {
                sort_mode = (sort_mode + 1) % 3;
                break;
            }
            if (ch == '/') {
                filter_active = 1;
                filter[0] = '\0';
                curs_set(1);
                break;
            }
            if (ch == 'k' && display_n > 0) {
                int target = procs[selected].pid;
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
                if (interval_idx < n_intervals - 1) interval_idx++;
                break;
            }
            if (ch == '-') {
                if (interval_idx > 0) interval_idx--;
                break;
            }
            if (ch == KEY_RESIZE) {
                if (filter_active) curs_set(0);
                goto skip_draw;
            }
        }

skip_draw:
        if (ch == KEY_RESIZE) continue;

        if (filter_active)
            curs_set(0);

        erase();

        char mem_fmt[32];
        fmt_mem(mem_fmt, sizeof(mem_fmt), total_ram);

        attrset(COLOR_PAIR(1) | A_BOLD);
        mvprintw(0, 0, "\u250c ProcLens ");
        int x = 12;
        mvprintw(0, x, "Procs: %d", display_n);
        x += 9 + (display_n >= 1000 ? 4 : display_n >= 100 ? 3 : display_n >= 10 ? 2 : 1);
        mvprintw(0, x, "Total: %s", mem_fmt);
        x += 7 + (int)strlen(mem_fmt);
        mvprintw(0, x, " Sort: %s", sort_label);
        x += 7 + (int)strlen(sort_label);
        mvprintw(0, x, " %.1fs", intervals[interval_idx]);
        attrset(COLOR_PAIR(3));
        mvprintw(0, maxx - 11, "[q] quit");
        attrset(A_NORMAL);

        mvhline(1, 0, ACS_HLINE, maxx);
        mvaddch(1, 0, ACS_LTEE);
        mvaddch(1, maxx - 1, ACS_RTEE);

        attrset(COLOR_PAIR(2) | A_BOLD);
        mvprintw(1, 2, "%-7s %11s  %s", "PID", "RSS", "NAME");
        attrset(A_NORMAL);

        mvhline(2, 0, ACS_HLINE, maxx);
        mvaddch(2, 0, ACS_LTEE);
        mvaddch(2, maxx - 1, ACS_RTEE);

        int row = 3;
        int avail_rows = maxy - 1;
        for (int i = 0; i < display_n && row < avail_rows; i++) {
            if (i == selected) {
                attrset(COLOR_PAIR(6));
            } else if (i % 2 == 0) {
                attrset(COLOR_PAIR(5));
            } else {
                attrset(COLOR_PAIR(3));
            }

            if (i != selected && procs[i].rss_kb > 1048576)
                attrset(COLOR_PAIR(4) | A_BOLD);

            char mem[32];
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

        for (int r = 1; r < avail_rows; r++) {
            if (mvinch(r, 0) == ' ') {
                mvaddch(r, 0, ACS_VLINE);
                mvaddch(r, maxx - 1, ACS_VLINE);
            }
        }

        if (filter_active) {
            curs_set(1);
            mvprintw(avail_rows, 0, " Filter: %s", filter);
            clrtoeol();
        }

        refresh();
    }

    free(procs);
    endwin();
    return 0;
}
