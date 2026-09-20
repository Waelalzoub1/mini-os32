/* 64-bit supervisor: runs loader32 under ptrace and emulates the mini-os32
 * syscalls (write/read/load/save/exit) at each int 0x80.  On a fault it
 * prints EIP/ESP/EAX, the nearest symbol from syms.txt, and a stack scan of
 * plausible return addresses. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sys/uio.h>

#define BASE 0x20000000u
#define SIZE (24u * 1024 * 1024)

static pid_t child;

static struct { unsigned addr; char name[40]; } syms[4096];
static int nsyms;

static void load_syms(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        unsigned a;
        char n[64];
        if (sscanf(line, "SYM %u %63s", &a, n) == 2 && nsyms < 4096) {
            syms[nsyms].addr = a;
            strncpy(syms[nsyms].name, n, 39);
            nsyms++;
        }
    }
    fclose(f);
}

static const char *nearest(unsigned addr, unsigned *off) {
    const char *best = 0;
    unsigned besta = 0;
    for (int i = 0; i < nsyms; i++) {
        if (syms[i].addr <= addr && syms[i].addr >= besta) {
            besta = syms[i].addr;
            best = syms[i].name;
        }
    }
    if (best) *off = addr - besta;
    return best;
}

static int rmem(unsigned addr, void *buf, int len) {
    struct iovec l = { buf, (size_t)len };
    struct iovec r = { (void *)(unsigned long)addr, (size_t)len };
    return (int)process_vm_readv(child, &l, 1, &r, 1, 0);
}

static int wmem(unsigned addr, const void *buf, int len) {
    struct iovec l = { (void *)buf, (size_t)len };
    struct iovec r = { (void *)(unsigned long)addr, (size_t)len };
    return (int)process_vm_writev(child, &l, 1, &r, 1, 0);
}

static void crash_report(struct user_regs_struct *rg, int sig) {
    unsigned eip = (unsigned)rg->rip, esp = (unsigned)rg->rsp;
    unsigned off;
    printf("\n=== FAULT sig %d ===\n", sig);
    printf("EIP %08x ESP %08x EAX %08x EBX %08x ECX %08x EDX %08x ESI %08x EDI %08x EBP %08x\n",
           eip, esp, (unsigned)rg->rax, (unsigned)rg->rbx, (unsigned)rg->rcx,
           (unsigned)rg->rdx, (unsigned)rg->rsi, (unsigned)rg->rdi, (unsigned)rg->rbp);
    const char *s = nearest(eip, &off);
    if (s) printf("EIP in %s+%u\n", s, off);
    /* scan the stack for text addresses = likely return chain */
    unsigned buf[4096];
    int n = rmem(esp, buf, sizeof(buf));
    if (n > 0) {
        printf("stack scan:\n");
        int shown = 0;
        for (int i = 0; i < n / 4 && shown < 30; i++) {
            unsigned v = buf[i];
            if (v >= BASE + 0x54 && v < BASE + 0x60000) {   /* text-ish */
                const char *t = nearest(v, &off);
                printf("  [esp+%4d] %08x  %s+%u\n", i * 4, v, t ? t : "?", t ? off : 0);
                shown++;
            }
        }
    }
}

int main(int argc, char **argv) {
    load_syms(argc > 1 ? argv[1] : "syms.txt");
    child = fork();
    if (child == 0) {
        execl("./loader32", "loader32", (char *)0);
        _exit(99);
    }
    int status;
    waitpid(child, &status, 0);          /* SIGSTOP from loader */
    if (!WIFSTOPPED(status)) { printf("loader died early: %x\n", status); return 1; }
    ptrace(PTRACE_SETOPTIONS, child, 0, PTRACE_O_TRACESYSGOOD);

    int in_syscall = 0;
    long nr = 0;
    unsigned a1 = 0, a2 = 0, a3 = 0;
    long result = 0;

    for (;;) {
        ptrace(PTRACE_SYSCALL, child, 0, 0);
        waitpid(child, &status, 0);
        if (WIFEXITED(status)) { printf("loader exited %d\n", WEXITSTATUS(status)); return 0; }
        if (!WIFSTOPPED(status)) continue;
        int sig = WSTOPSIG(status);
        if (sig == (SIGTRAP | 0x80)) {
            struct user_regs_struct rg;
            ptrace(PTRACE_GETREGS, child, 0, &rg);
            if (!in_syscall) {
                in_syscall = 1;
                nr = (long)rg.orig_rax;
                a1 = (unsigned)rg.rbx; a2 = (unsigned)rg.rcx; a3 = (unsigned)rg.rdx;
                /* loader's own setup syscalls run before SIGSTOP, so
                 * everything here is the OS program's.  Cancel the real
                 * syscall and emulate. */
                result = -1;
                switch (nr) {
                case 0: {  /* SYS_WRITE(fd, buf, len) */
                    static char buf[65536];
                    int len = (int)a3;
                    if (len > 0 && len <= (int)sizeof(buf) && rmem(a2, buf, len) == len) {
                        write(a1 == 2 ? 2 : 1, buf, len);
                        result = len;
                    }
                    break;
                }
                case 1: {  /* SYS_READ(fd, buf, max) -> console line, stripped */
                    char line[512];
                    if (fgets(line, sizeof(line), stdin)) {
                        int L = (int)strlen(line);
                        while (L > 0 && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = 0;
                        if (L >= (int)a3) L = (int)a3 - 1;
                        wmem(a2, line, L + 1);
                        result = L;
                    } else result = 0;
                    break;
                }
                case 3: {  /* SYS_LOAD(name, buf, max) */
                    char name[256];
                    if (rmem(a1, name, sizeof(name)) > 0) {
                        name[255] = 0;
                        int fd = open(name, O_RDONLY);
                        if (fd >= 0) {
                            static char fbuf[4 << 20];
                            int n = (int)read(fd, fbuf, sizeof(fbuf));
                            close(fd);
                            if (n >= 0 && n <= (int)a3) {
                                wmem(a2, fbuf, n);
                                result = n;
                            }
                        }
                        fprintf(stderr, "[load %s -> %ld]\n", name, result);
                    }
                    break;
                }
                case 4: {  /* SYS_SAVE(name, buf, size) */
                    char name[256];
                    if (rmem(a1, name, sizeof(name)) > 0) {
                        name[255] = 0;
                        static char fbuf[4 << 20];
                        int n = (int)a3;
                        if (n >= 0 && n <= (int)sizeof(fbuf) && rmem(a2, fbuf, n) == n) {
                            int fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                            if (fd >= 0) { write(fd, fbuf, n); close(fd); result = 0; }
                        }
                        fprintf(stderr, "[save %s %d -> %ld]\n", name, n, result);
                    }
                    break;
                }
                case 6:   /* SYS_EXIT(code) */
                    printf("\n[program exited %d]\n", (int)a1);
                    kill(child, SIGKILL);
                    return 0;
                default:
                    fprintf(stderr, "[unhandled OS syscall %ld (%u,%u,%u) eip=%llx]\n",
                            nr, a1, a2, a3, (unsigned long long)rg.rip);
                }
                rg.orig_rax = -1;        /* cancel the real syscall */
                ptrace(PTRACE_SETREGS, child, 0, &rg);
            } else {
                in_syscall = 0;
                rg.rax = (unsigned)result;
                ptrace(PTRACE_SETREGS, child, 0, &rg);
            }
        } else if (sig == SIGSEGV || sig == SIGILL || sig == SIGBUS || sig == SIGFPE || sig == SIGTRAP) {
            struct user_regs_struct rg;
            ptrace(PTRACE_GETREGS, child, 0, &rg);
            crash_report(&rg, sig);
            kill(child, SIGKILL);
            return 2;
        }
        /* other signals: swallow */
    }
}
