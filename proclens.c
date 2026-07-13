#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <ncurses.h>

typedef struct {
    int pid;
    unsigned long rss_kb;
    char name[256];
} Process;

static int parse_proc(int pid, unsigned long *rss, char *name) {
    char path[256], line[256];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    *rss = 0;
    name[0] = '\0';
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "Name:", 5) == 0)
            sscanf(line + 5, " %255s", name);
        else if (strncmp(line, "VmRSS:", 6) == 0)
            sscanf(line + 6, " %lu", rss);
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
    timeout(1000);
    keypad(stdscr, TRUE);

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

    Process *procs = NULL;
    int cap = 0;

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
        int count = 0;
        while ((entry = readdir(dp))) {
            int pid = atoi(entry->d_name);
            if (pid > 0) count++;
        }
        closedir(dp);

        if (count > cap) {
            cap = count + 64;
            Process *tmp = realloc(procs, cap * sizeof(Process));
            if (!tmp) break;
            procs = tmp;
        }

        dp = opendir("/proc");
        if (!dp) break;

        int n = 0;
        unsigned long total_ram = 0;
        while ((entry = readdir(dp))) {
            int pid = atoi(entry->d_name);
            if (pid <= 0) continue;
            if (parse_proc(pid, &procs[n].rss_kb, procs[n].name) == 0) {
                procs[n].pid = pid;
                total_ram += procs[n].rss_kb;
                n++;
            }
        }
        closedir(dp);

        qsort(procs, n, sizeof(Process), cmp_rss_desc);

        int ch = getch();
        if (ch == 'q') break;
        if (ch == KEY_RESIZE) continue;

        erase();

        attrset(COLOR_PAIR(1) | A_BOLD);
        mvprintw(0, 0, "\u250c ProcLens ");
        int x = 12;
        mvprintw(0, x, "Processes: %d", n);
        x += 14 + (n >= 1000 ? 4 : n >= 100 ? 3 : n >= 10 ? 2 : 1);
        char mem_fmt[32];
        fmt_mem(mem_fmt, sizeof(mem_fmt), total_ram);
        mvprintw(0, x, "Total: %s", mem_fmt);
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
        for (int i = 0; i < n && row < avail_rows; i++) {
            if (procs[i].rss_kb == 0) continue;

            if (i % 2 == 0)
                attrset(COLOR_PAIR(5));
            else
                attrset(COLOR_PAIR(3));

            if (procs[i].rss_kb > 1048576)
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

        refresh();
    }

    free(procs);
    endwin();
    return 0;
}
