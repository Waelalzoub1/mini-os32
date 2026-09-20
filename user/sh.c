#include "libc.h"

#define NAME_MAX 47

static char cwd[64] = "";

static void prompt(void) {
    putc('/');
    if (cwd[0]) {
        int n = strlen(cwd);
        if (n > 0 && cwd[n - 1] == '/') n--;
        for (int i = 0; i < n; i++) putc(cwd[i]);
    }
    puts("> ");
}

static int char_eq_ci(char a, char b) {
    if (a >= 'a' && a <= 'z') a -= 32;
    if (b >= 'a' && b <= 'z') b -= 32;
    return a == b;
}

static int starts_with_ci(const char *s, const char *p) {
    while (*p) {
        if (*s == 0) return 0;
        if (!char_eq_ci(*s, *p)) return 0;
        s++; p++;
    }
    return 1;
}

static int name_eq_ci(const char *a, const char *b) {
    while (*a || *b) {
        if (!char_eq_ci(*a, *b)) return 0;
        if (*a == 0 || *b == 0) return 0;
        a++; b++;
    }
    return 1;
}

static int find_slash(const char *s) {
    for (int i = 0; s[i]; i++) if (s[i] == '/') return i;
    return -1;
}

static void entry_name(char *out, const char *buf, int idx) {
    const char *p = buf + idx * 52;
    int i = 0;
    while (i < 48 && p[i]) { out[i] = p[i]; i++; }
    out[i] = 0;
}

static void pop_component(char *out, int *n) {
    if (*n == 0) return;
    int i = *n;
    if (i > 0 && out[i - 1] == '/') i--;
    while (i > 0 && out[i - 1] != '/') i--;
    *n = i;
    out[*n] = 0;
}

static int build_path(const char *arg, char *out, int outsz, int want_dir) {
    if (!arg || !*arg) return 0;
    int n = 0;
    const char *p = arg;
    if (arg[0] != '/') {
        n = strlen(cwd);
        if (n >= outsz) return -1;
        memcpy(out, cwd, n);
        out[n] = 0;
    } else {
        p = arg + 1;
    }

    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *start = p;
        int len = 0;
        while (*p && *p != '/') { p++; len++; }
        if (len == 1 && start[0] == '.') continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') { pop_component(out, &n); continue; }
        if (n + len + 1 >= outsz) return -1;
        memcpy(out + n, start, len);
        n += len;
        out[n++] = '/';
        out[n] = 0;
    }

    if (want_dir) {
        if (n == 0) { out[0] = 0; return 1; }
        if (out[n - 1] != '/') { out[n++] = '/'; out[n] = 0; }
    } else {
        if (n > 0 && out[n - 1] == '/') { n--; out[n] = 0; }
        if (n == 0) return 0;
    }
    return 1;
}

/* Shared listing buffer, sized for a full directory: with 512 entries the
 * old 64-entry stack buffers silently truncated ls and cd. */
static char list_buf[52 * 512];

static int dir_exists(const char *path) {
    int n = sys_list(list_buf, sizeof(list_buf));
    char name[49];
    for (int i = 0; i < n; i++) {
        entry_name(name, list_buf, i);
        if (name_eq_ci(name, path)) return 1;
    }
    return 0;
}

static int dir_has_children(const char *path) {
    int n = sys_list(list_buf, sizeof(list_buf));
    char name[49];
    for (int i = 0; i < n; i++) {
        entry_name(name, list_buf, i);
        if (starts_with_ci(name, path) && !name_eq_ci(name, path)) return 1;
    }
    return 0;
}

/* Is `name` visible in the current directory?  Fills disp (the name shown,
 * without the cwd prefix) and is_dir (entry is an immediate subdirectory). */
static int ls_visible(const char *name, char *disp, int *is_dir) {
    const char *rest = name;
    if (cwd[0] != 0) {
        if (!starts_with_ci(name, cwd)) return 0;
        rest = name + strlen(cwd);
        if (*rest == 0) return 0;
    }
    int s = find_slash(rest);
    if (s < 0) {
        strcpy(disp, rest);
        *is_dir = 0;
        return 1;
    }
    if (rest[s + 1] == 0) {
        memcpy(disp, rest, s);
        disp[s] = 0;
        *is_dir = 1;
        return 1;
    }
    return 0;
}

/* One ls cell: "name/ " for dirs, "name size" for files, padded into
 * 26-char columns, three per line. */
#define LS_COLS 3
#define LS_COL_W 26

static void ls_print(const char *disp, int is_dir, int size, int *col) {
    int w = strlen(disp);
    puts(disp);
    if (is_dir) { puts("/"); w++; }
    else {
        putc(' '); w++;
        char num[12];
        int v = size; int j = 0;
        if (v == 0) { num[j++] = '0'; }
        while (v > 0 && j < 10) { num[j++] = '0' + (v % 10); v /= 10; }
        for (int k = j - 1; k >= 0; k--) { putc(num[k]); w++; }
    }
    (*col)++;
    if (*col >= LS_COLS) { puts("\n"); *col = 0; }
    else while (w < LS_COL_W) { putc(' '); w++; }
}

static void cmd_ls(void) {
    int n = sys_list(list_buf, sizeof(list_buf));
    int col = 0;
    /* two passes: directories first, then files */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < n; i++) {
            char name[49];
            entry_name(name, list_buf, i);
            if (name[0] == 0) continue;
            char disp[49];
            int is_dir = 0;
            if (!ls_visible(name, disp, &is_dir)) continue;
            if ((pass == 0) != (is_dir != 0)) continue;
            int size = *(int*)(list_buf + i * 52 + 48);
            ls_print(disp, is_dir, size, &col);
        }
    }
    if (col) puts("\n");
}

static void cmd_type(const char *name) {
    static char buf[65536];
    char path[64];
    int ok = build_path(name, path, sizeof(path), 0);
    if (ok <= 0 || strlen(path) > NAME_MAX) { puts("file?\n"); return; }
    int n = sys_load(path, buf, sizeof(buf));
    if (n <= 0) { puts("not found\n"); return; }
    sys_write(1, buf, n);
    puts("\n");
}

static void cmd_run(const char *name) {
    char path[64];
    int ok = build_path(name, path, sizeof(path), 0);
    if (ok <= 0 || strlen(path) > NAME_MAX) { puts("file?\n"); return; }
    int rc = sys_exec(path);
    puts("exit ");
    char num[12];
    int v = rc; int j = 0;
    if (v == 0) { num[j++] = '0'; }
    while (v > 0 && j < 10) { num[j++] = '0' + (v % 10); v /= 10; }
    for (int k = j - 1; k >= 0; k--) putc(num[k]);
    puts("\n");
}

#define KEY_UP 0x100
#define KEY_DOWN 0x101
#define KEY_LEFT 0x102
#define KEY_RIGHT 0x103
#define KEY_DEL 0x104

static void row_col_of_index(const char *buf, int len, int idx, int *row, int *col);
static int index_of_row_col(const char *buf, int len, int target_row, int target_col);

static int move_cursor_rows(const char *buf, int len, int cursor, int delta) {
    int row, col;
    row_col_of_index(buf, len, cursor, &row, &col);
    row = row + delta;
    if (row < 0) row = 0;
    return index_of_row_col(buf, len, row, col);
}

static void row_col_of_index(const char *buf, int len, int idx, int *row, int *col) {
    int r = 0, c = 0;
    for (int i = 0; i < idx && i < len; i++) {
        char ch = buf[i];
        if (ch == '\n') { r++; c = 0; }
        else { c++; if (c >= 80) { r++; c = 0; } }
    }
    *row = r;
    *col = c;
}

static int index_of_row(const char *buf, int len, int target_row) {
    int r = 0, c = 0;
    if (target_row <= 0) return 0;
    for (int i = 0; i < len; i++) {
        if (r == target_row) return i;
        char ch = buf[i];
        if (ch == '\n') { r++; c = 0; }
        else { c++; if (c >= 80) { r++; c = 0; } }
    }
    return len;
}

static int index_of_row_col(const char *buf, int len, int target_row, int target_col) {
    int r = 0, c = 0;
    if (target_row <= 0 && target_col <= 0) return 0;
    for (int i = 0; i < len; i++) {
        if (r == target_row && c == target_col) return i;
        char ch = buf[i];
        if (ch == '\n') {
            if (r == target_row) return i;
            r++; c = 0;
        } else {
            c++;
            if (c >= 80) {
                if (r == target_row) return i + 1;
                r++; c = 0;
            }
        }
    }
    return len;
}

static void editor_redraw(const char *name, const char *buf, int len, int cursor, int top_row, int dirty, const char *status) {
    sys_cls();
    int idx = index_of_row(buf, len, top_row);
    int row = 0, col = 0;
    while (row < 24 && idx < len) {
        char ch = buf[idx++];
        if (ch == '\n') {
            sys_write(1, "\n", 1);
            row++; col = 0;
            continue;
        }
        sys_write(1, &ch, 1);
        col++;
        if (col >= 80) {
            sys_write(1, "\n", 1);
            row++; col = 0;
        }
    }
    while (row < 24) {
        sys_write(1, "\n", 1);
        row++;
    }
    sys_setcursor(0, 24);
    char line[80];
    int n = 0;
    line[n++] = dirty ? '*' : ' ';
    const char *p = name;
    while (*p && n < 70) line[n++] = *p++;
    line[n++] = ' ';
    if (status) {
        const char *s = status;
        while (*s && n < 79) line[n++] = *s++;
    }
    while (n < 79) line[n++] = ' ';
    line[n++] = 0;
    sys_write(1, line, strlen(line));

    int crow, ccol;
    row_col_of_index(buf, len, cursor, &crow, &ccol);
    int srow = crow - top_row;
    if (srow < 0) srow = 0;
    if (srow > 23) srow = 23;
    if (ccol > 79) ccol = 79;
    sys_setcursor(ccol, srow);
}

static void editor_draw_cursor(const char *buf, int len, int cursor, int top_row, int on) {
    int crow, ccol;
    row_col_of_index(buf, len, cursor, &crow, &ccol);
    int srow = crow - top_row;
    if (srow < 0 || srow > 23) return;
    if (ccol > 79) ccol = 79;
    char ch = ' ';
    if (cursor < len) {
        ch = buf[cursor];
        if (ch == '\n') ch = ' ';
    }
    char out = on ? '_' : ch;
    sys_setcursor(ccol, srow);
    sys_write(1, &out, 1);
    sys_setcursor(ccol, srow);
}

static void spin_delay(int n) {
    volatile int i = 0;
    while (i < n) i = i + 1;
}

static void cmd_edit(const char *name) {
    static char buf[131072];
    char path[64];
    int ok = build_path(name, path, sizeof(path), 0);
    if (ok <= 0 || strlen(path) > NAME_MAX) { puts("file?\n"); return; }
    int len = 0;
    int cursor = 0;
    int top_row = 0;
    int dirty = 0;
    int quit_pending = 0;
    const char *status = 0;
    int need_redraw = 1;
    int last_cursor = -1;
    int last_top_row = -1;
    int blink_on = 1;
    int blink_count = 0;

    int n = sys_load(path, buf, sizeof(buf));
    if (n > 0) len = n;
    else len = 0;

    for (;;) {
        int crow, ccol;
        int prev_top = top_row;
        row_col_of_index(buf, len, cursor, &crow, &ccol);
        if (crow < top_row) top_row = crow;
        if (crow >= top_row + 24) top_row = crow - 23;
        if (top_row != prev_top) need_redraw = 1;

        if (need_redraw) {
            editor_redraw(name, buf, len, cursor, top_row, dirty, status);
            status = 0;
            need_redraw = 0;
            blink_on = 1;
            blink_count = 0;
            editor_draw_cursor(buf, len, cursor, top_row, blink_on);
            last_cursor = cursor;
            last_top_row = top_row;
        } else {
            int srow = crow - top_row;
            if (srow < 0) srow = 0;
            if (srow > 23) srow = 23;
            if (ccol > 79) ccol = 79;
            sys_setcursor(ccol, srow);
            if (cursor != last_cursor || top_row != last_top_row) {
                if (last_cursor >= 0) editor_draw_cursor(buf, len, last_cursor, last_top_row, 0);
                editor_draw_cursor(buf, len, cursor, top_row, 1);
                last_cursor = cursor;
                last_top_row = top_row;
                blink_on = 1;
                blink_count = 0;
            }
        }

        int key = sys_getkey_nb();
        if (key == 0) {
            blink_count++;
            if (blink_count > 6400) {
                blink_on = !blink_on;
                editor_draw_cursor(buf, len, cursor, top_row, blink_on);
                blink_count = 0;
            }
            spin_delay(7500);
            continue;
        }
        blink_on = 1;
        blink_count = 0;

        if (key == 17) { /* Ctrl+Q */
            if (dirty && !quit_pending) {
                status = "Unsaved. Ctrl+Q again to quit.";
                quit_pending = 1;
                need_redraw = 1;
                continue;
            }
            sys_cls();
            return;
        } else if (key == 19) { /* Ctrl+S */
            if (sys_save(path, buf, len) != 0) status = "save fail";
            else {
                dirty = 0;
                /* flush to the persistent store too: work typed into the
                   editor should survive losing the VM or the power */
                status = sys_sync() == 0 ? "saved+synced" : "saved (no store)";
            }
            need_redraw = 1;
            continue;
        } else if (key == 21) { /* Ctrl+U: page up */
            cursor = move_cursor_rows(buf, len, cursor, -23);
            need_redraw = 1;
            continue;
        } else if (key == 4) { /* Ctrl+D: page down */
            cursor = move_cursor_rows(buf, len, cursor, 23);
            need_redraw = 1;
            continue;
        }

        quit_pending = 0;

        if (key == KEY_LEFT) {
            if (cursor > 0) cursor--;
        } else if (key == KEY_RIGHT) {
            if (cursor < len) cursor++;
        } else if (key == KEY_UP) {
            int row, col;
            row_col_of_index(buf, len, cursor, &row, &col);
            if (row > 0) cursor = index_of_row_col(buf, len, row - 1, col);
        } else if (key == KEY_DOWN) {
            int row, col;
            row_col_of_index(buf, len, cursor, &row, &col);
            cursor = index_of_row_col(buf, len, row + 1, col);
        } else if (key == KEY_DEL) {
            if (cursor < len) {
                memmove(buf + cursor, buf + cursor + 1, len - cursor - 1);
                len--;
                dirty = 1;
                need_redraw = 1;
            }
        } else if (key == 8) { /* backspace */
            if (cursor > 0) {
                memmove(buf + cursor - 1, buf + cursor, len - cursor);
                cursor--;
                len--;
                dirty = 1;
                need_redraw = 1;
            }
        } else if (key == '\r' || key == '\n') {
            if (len + 1 < (int)sizeof(buf)) {
                memmove(buf + cursor + 1, buf + cursor, len - cursor);
                buf[cursor++] = '\n';
                len++;
                dirty = 1;
                need_redraw = 1;
            }
        } else if (key >= 32 && key < 127) {
            if (len + 1 < (int)sizeof(buf)) {
                memmove(buf + cursor + 1, buf + cursor, len - cursor);
                buf[cursor++] = (char)key;
                len++;
                dirty = 1;
                need_redraw = 1;
            }
        }
    }
}

/* Publish the cwd to the kernel so launched programs (cc) resolve relative
 * names against the same directory the prompt shows. */
static void set_cwd(const char *path) {
    strcpy(cwd, path);
    sys_setcwd(cwd);
}

static void cmd_cd(const char *arg) {
    char path[64];
    int ok = build_path(arg, path, sizeof(path), 1);
    if (ok <= 0) { puts("cd <dir>\n"); return; }
    if (strlen(path) > NAME_MAX) { puts("name too long\n"); return; }
    if (path[0] == 0) { set_cwd(""); return; }
    if (!dir_exists(path)) { puts("no such dir\n"); return; }
    set_cwd(path);
}

static void cmd_mkdir(const char *arg) {
    char path[64];
    int ok = build_path(arg, path, sizeof(path), 1);
    if (ok <= 0) { puts("mkdir <dir>\n"); return; }
    if (path[0] == 0) { puts("mkdir: root\n"); return; }
    if (strlen(path) > NAME_MAX) { puts("name too long\n"); return; }
    if (dir_exists(path)) { puts("exists\n"); return; }

    /* ensure parent exists */
    int n = strlen(path);
    int i = n - 2;
    while (i >= 0 && path[i] != '/') i--;
    if (i >= 0) {
        char parent[64];
        memcpy(parent, path, i + 1);
        parent[i + 1] = 0;
        if (parent[0] && !dir_exists(parent)) { puts("no such dir\n"); return; }
    }

    char zero = 0;
    if (sys_save(path, &zero, 0) != 0) puts("mkdir fail\n");
}

static void cmd_rmdir(const char *arg) {
    char path[64];
    int ok = build_path(arg, path, sizeof(path), 1);
    if (ok <= 0) { puts("rmdir <dir>\n"); return; }
    if (path[0] == 0) { puts("rmdir: root\n"); return; }
    if (strlen(path) > NAME_MAX) { puts("name too long\n"); return; }
    if (!dir_exists(path)) { puts("no such dir\n"); return; }
    if (dir_has_children(path)) { puts("not empty\n"); return; }
    if (sys_delete(path) != 0) puts("rmdir fail\n");
}

static void help(void) {
    puts("ls type edit run mv rm mkdir rmdir cd cc as sync shutdown help\n");
}

int main(void) {
    char line[128];
    /* `run` replaces the shell process, and a fresh shell starts here when
       the program exits.  The kernel remembers the cwd the previous shell
       published on `cd`, so read it back rather than resetting to root. */
    if (sys_getcwd(cwd, sizeof(cwd)) < 0) set_cwd("");
    for (;;) {
        prompt();
        int n = readline(line, sizeof(line));
        if (n <= 0) continue;
        char *cmd = line;
        while (*cmd == ' ') cmd++;
        if (cmd[0] == 0) continue;
        if (!strncmp(cmd, "ls", 2) && (cmd[2] == 0 || cmd[2] == ' ')) {
            cmd_ls();
        } else if (!strncmp(cmd, "type", 4) && (cmd[4] == 0 || cmd[4] == ' ')) {
            char *arg = cmd + 4; while (*arg == ' ') arg++;
            if (*arg) cmd_type(arg); else puts("file?\n");
        } else if (!strncmp(cmd, "edit", 4) && (cmd[4] == 0 || cmd[4] == ' ')) {
            char *arg = cmd + 4; while (*arg == ' ') arg++;
            if (*arg) cmd_edit(arg); else puts("file?\n");
        } else if (!strncmp(cmd, "run", 3) && (cmd[3] == 0 || cmd[3] == ' ')) {
            char *arg = cmd + 3; while (*arg == ' ') arg++;
            if (*arg) cmd_run(arg); else puts("file?\n");
        } else if (!strncmp(cmd, "mv", 2) && (cmd[2] == 0 || cmd[2] == ' ')) {
            char *arg = cmd + 2; while (*arg == ' ') arg++;
            char *arg2 = arg; while (*arg2 && *arg2 != ' ') arg2++;
            if (*arg2) { *arg2++ = 0; while (*arg2 == ' ') arg2++; }
            if (*arg && *arg2) {
                char p1[64], p2[64];
                int ok1 = build_path(arg, p1, sizeof(p1), 0);
                int ok2 = build_path(arg2, p2, sizeof(p2), 0);
                if (ok1 <= 0 || ok2 <= 0 || strlen(p1) > NAME_MAX || strlen(p2) > NAME_MAX) {
                    puts("mv <old> <new>\n");
                } else if (sys_rename(p1, p2) != 0) {
                    puts("rename fail\n");
                }
            } else {
                puts("mv <old> <new>\n");
            }
        } else if (!strncmp(cmd, "rm", 2) && (cmd[2] == 0 || cmd[2] == ' ')) {
            char *arg = cmd + 2; while (*arg == ' ') arg++;
            if (*arg) {
                char path[64];
                int ok = build_path(arg, path, sizeof(path), 0);
                if (ok <= 0 || strlen(path) > NAME_MAX) puts("rm <file>\n");
                else if (sys_delete(path) != 0) puts("delete fail\n");
            } else {
                puts("rm <file>\n");
            }
        } else if (!strncmp(cmd, "mkdir", 5) && (cmd[5] == 0 || cmd[5] == ' ')) {
            char *arg = cmd + 5; while (*arg == ' ') arg++;
            cmd_mkdir(arg);
        } else if (!strncmp(cmd, "rmdir", 5) && (cmd[5] == 0 || cmd[5] == ' ')) {
            char *arg = cmd + 5; while (*arg == ' ') arg++;
            cmd_rmdir(arg);
        } else if (!strncmp(cmd, "cd", 2) && (cmd[2] == 0 || cmd[2] == ' ')) {
            char *arg = cmd + 2; while (*arg == ' ') arg++;
            if (*arg) cmd_cd(arg); else cmd_cd("/");
        } else if (!strncmp(cmd, "cc", 2) && (cmd[2] == 0 || cmd[2] == ' ')) {
            cmd_run("/cc");
        } else if (!strncmp(cmd, "as", 2) && (cmd[2] == 0 || cmd[2] == ' ')) {
            cmd_run("/as");
        } else if (!strncmp(cmd, "sync", 4) && (cmd[4] == 0 || cmd[4] == ' ')) {
            if (sys_sync() != 0) puts("no persistent store\n");
            else puts("synced\n");
        } else if (!strncmp(cmd, "shutdown", 8) || !strncmp(cmd, "poweroff", 8)) {
            puts("shutting down\n");
            sys_poweroff();
        } else if (!strncmp(cmd, "help", 4)) {
            help();
        } else {
            puts("?\n");
        }
    }
    return 0;
}
