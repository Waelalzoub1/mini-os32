/* Path joining, in-OS.  The same logic as user/libc.c so that a program
 * behaves identically whether it was built by gcc on the host or by cc here --
 * cc.c needs these, and cc.c compiling itself is the point. */
#include <stdio.h>
#include <string.h>
#include <path.h>

static char lib_cwd[PATH_MAX_LEN];
static int  lib_cwd_read = 0;

const char *cwd_get(void) {
    if (!lib_cwd_read) {
        lib_cwd[0] = 0;
        sys_getcwd(lib_cwd, PATH_MAX_LEN);
        lib_cwd_read = 1;
    }
    return lib_cwd;
}

static void path_pop(char *out, int *n) {
    int i = *n;
    if (i > 0 && out[i - 1] == '/') i--;
    while (i > 0 && out[i - 1] != '/') i--;
    *n = i;
    out[i] = 0;
}

/* Join `name` onto directory `base` ("" means root) and fold away "." and
 * "..".  A leading '/' on `name` ignores `base`.  0 on overflow or if empty. */
int path_resolve(const char *base, const char *name, char *out, int outsz) {
    if (!name || !*name) return 0;
    int n = 0;
    out[0] = 0;
    if (name[0] != '/' && base && *base) {
        n = strlen(base);
        if (n + 1 >= outsz) return 0;
        memcpy(out, base, n);
        if (out[n - 1] != '/') { out[n] = '/'; n++; }
        out[n] = 0;
    }
    const char *p = name;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *start = p;
        int len = 0;
        while (*p && *p != '/') { p++; len++; }
        if (len == 1 && start[0] == '.') continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') { path_pop(out, &n); continue; }
        if (n + len + 1 >= outsz) return 0;
        memcpy(out + n, start, len);
        n += len;
        out[n] = '/';
        n++;
        out[n] = 0;
    }
    if (n > 0 && out[n - 1] == '/') { n--; out[n] = 0; }   /* naming a file, not a dir */
    return n > 0;
}

/* As path_resolve, but also rejects keys the filesystem would truncate --
 * an over-long join would otherwise quietly hit some other file. */
int path_fs(const char *base, const char *name, char *out, int outsz) {
    if (!path_resolve(base, name, out, outsz)) return 0;
    return strlen(out) <= FS_NAME_MAX;
}

/* Directory part of a path, keeping the trailing '/' ("" when at root). */
void path_dir(const char *path, char *out, int outsz) {
    int n = strlen(path);
    while (n > 0 && path[n - 1] != '/') n--;
    if (n >= outsz) n = outsz - 1;
    memcpy(out, path, n);
    out[n] = 0;
}

const char *path_base(const char *path) {
    const char *b = path;
    const char *p = path;
    while (*p) {
        if (*p == '/') b = p + 1;
        p++;
    }
    return b;
}
