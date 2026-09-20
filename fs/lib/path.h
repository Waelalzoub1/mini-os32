#ifndef PATH_H
#define PATH_H

/* The filesystem is flat -- "src/main.c" is one literal key -- so a relative
 * name only means something once joined to a base directory.  The base is the
 * shell's cwd, published via sys_setcwd on `cd`. */
#define PATH_MAX_LEN 64
#define FS_NAME_MAX  47   /* longer keys are silently truncated by the kernel */

const char *cwd_get(void);
int   path_resolve(const char *base, const char *name, char *out, int outsz);
int   path_fs(const char *base, const char *name, char *out, int outsz);
void  path_dir(const char *path, char *out, int outsz);
const char *path_base(const char *path);

#endif
