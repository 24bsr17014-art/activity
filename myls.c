/*
 * myls.c - a small `ls` clone built directly on Linux system calls.
 *
 * Course  : 22CS3C05L Computer Science III Lab
 * Topic   : Directory and file command manipulation
 *
 * System calls used directly
 *   open(2)          open a directory (O_DIRECTORY)
 *   getdents64(2)    read raw directory entries (via syscall(2); glibc has no wrapper)
 *   lstat(2)/stat(2) fetch inode metadata (type, mode, links, uid/gid, size, times)
 *   readlink(2)      resolve symbolic link targets
 *   ioctl(2)         TIOCGWINSZ - terminal width for column output
 *   close(2), isatty(3)
 * Library helpers (not system calls, used only for formatting)
 *   getpwuid/getgrgid (name lookup), localtime/strftime, qsort, printf
 *
 * Supported options
 *   -a  show hidden entries (including . and ..)
 *   -d  list directories themselves, not their contents (ls -ld dir)
 *   -l  long listing (type+permissions, links, owner, group, size, mtime)
 *   -i  print inode number
 *   -R  recurse into subdirectories
 *   -1  one entry per line (default when stdout is not a terminal)
 *   -h  human readable sizes (with -l)
 *   -t  sort by modification time, newest first
 *   -S  sort by size, largest first
 *   -r  reverse the sort order
 *   -F  append indicator (/ * @ | =) to names
 *   -O  print the permission bits in octal (e.g. 4755) before the symbolic form
 *       (extension, makes permission verification easy)
 *
 * Exit status: 0 ok, 1 minor problem (e.g. unreadable sub-directory),
 *              2 serious problem (e.g. command-line argument not found)
 */
#define _GNU_SOURCE
#include <dirent.h>   /* only for the DT_* constants; no opendir/readdir used */
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ---------- raw kernel directory record (see getdents64(2)) ---------- */
struct linux_dirent64 {
    uint64_t       d_ino;
    int64_t        d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[];
};

/* ---------- options ---------- */
static int opt_a, opt_d, opt_l, opt_i, opt_R, opt_1, opt_h, opt_t, opt_S, opt_r, opt_F, opt_O;
static int exit_status = 0;
static const char *prog = "myls";

/* ---------- one directory entry plus its metadata ---------- */
typedef struct {
    char       *name;      /* name as shown                         */
    char       *path;      /* path used for lstat/readlink          */
    struct stat st;
    int         have_st;   /* 0 if lstat failed or was not needed   */
    uint64_t    ino;       /* inode number from the dirent          */
    unsigned char dtype;   /* d_type hint from getdents64           */
    char       *link;      /* symlink target (only with -l)         */
} entry;

/* ================= helpers ================= */

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { perror(prog); exit(2); }
    return p;
}

static char *xstrdup(const char *s)
{
    char *p = xmalloc(strlen(s) + 1);
    strcpy(p, s);
    return p;
}

/* Print an error without letting it interleave badly with buffered stdout. */
static void report(const char *what, const char *path)
{
    int e = errno;
    fflush(stdout);
    fprintf(stderr, "%s: %s '%s': %s\n", prog, what, path, strerror(e));
}

/* "-rwsr-xr-t" style permission string. out must hold 11 bytes. */
void mode_string(mode_t m, char *s)
{
    char t;
    switch (m & S_IFMT) {
    case S_IFREG:  t = '-'; break;
    case S_IFDIR:  t = 'd'; break;
    case S_IFLNK:  t = 'l'; break;
    case S_IFCHR:  t = 'c'; break;
    case S_IFBLK:  t = 'b'; break;
    case S_IFIFO:  t = 'p'; break;
    case S_IFSOCK: t = 's'; break;
    default:       t = '?'; break;
    }
    s[0] = t;
    s[1] = (m & S_IRUSR) ? 'r' : '-';
    s[2] = (m & S_IWUSR) ? 'w' : '-';
    s[3] = (m & S_ISUID) ? ((m & S_IXUSR) ? 's' : 'S') : ((m & S_IXUSR) ? 'x' : '-');
    s[4] = (m & S_IRGRP) ? 'r' : '-';
    s[5] = (m & S_IWGRP) ? 'w' : '-';
    s[6] = (m & S_ISGID) ? ((m & S_IXGRP) ? 's' : 'S') : ((m & S_IXGRP) ? 'x' : '-');
    s[7] = (m & S_IROTH) ? 'r' : '-';
    s[8] = (m & S_IWOTH) ? 'w' : '-';
    s[9] = (m & S_ISVTX) ? ((m & S_IXOTH) ? 't' : 'T') : ((m & S_IXOTH) ? 'x' : '-');
    s[10] = '\0';
}

static void human_size(unsigned long long sz, char *out, size_t n)
{
    static const char suf[] = "KMGTPE";
    if (sz < 1024) { snprintf(out, n, "%llu", sz); return; }
    double v = (double)sz;
    int i = -1;
    while (v >= 1024.0 && i < 5) { v /= 1024.0; i++; }
    if (v < 10.0) {                         /* one decimal, rounded up like GNU ls */
        unsigned long t = (unsigned long)(v * 10.0);
        if ((double)t < v * 10.0) t++;
        if (t >= 100) snprintf(out, n, "10%c", suf[i]);
        else          snprintf(out, n, "%lu.%lu%c", t / 10, t % 10, suf[i]);
    } else {
        unsigned long t = (unsigned long)v;
        if ((double)t < v) t++;
        snprintf(out, n, "%lu%c", t, suf[i]);
    }
}

static char classify_mode(mode_t m)
{
    if (S_ISDIR(m))  return '/';
    if (S_ISLNK(m))  return '@';
    if (S_ISFIFO(m)) return '|';
    if (S_ISSOCK(m)) return '=';
    if (S_ISREG(m) && (m & (S_IXUSR | S_IXGRP | S_IXOTH))) return '*';
    return 0;
}

static char classify_char(const entry *e)
{
    return e->have_st ? classify_mode(e->st.st_mode) : 0;
}

/* ================= sorting ================= */

static int cmp_entry(const void *pa, const void *pb)
{
    const entry *a = pa, *b = pb;
    int r = 0;
    if (opt_S && a->have_st && b->have_st) {
        if (a->st.st_size != b->st.st_size)
            r = (a->st.st_size < b->st.st_size) ? 1 : -1;      /* big first */
    } else if (opt_t && a->have_st && b->have_st) {
        if (a->st.st_mtim.tv_sec != b->st.st_mtim.tv_sec)
            r = (a->st.st_mtim.tv_sec < b->st.st_mtim.tv_sec) ? 1 : -1; /* new first */
        else if (a->st.st_mtim.tv_nsec != b->st.st_mtim.tv_nsec)
            r = (a->st.st_mtim.tv_nsec < b->st.st_mtim.tv_nsec) ? 1 : -1;
    }
    if (r == 0) r = strcmp(a->name, b->name);
    return opt_r ? -r : r;
}

/* ================= printing ================= */

static int term_width(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}

static int ndigits(unsigned long long v)
{
    int d = 1;
    while (v >= 10) { v /= 10; d++; }
    return d;
}

static void print_name(const entry *e)
{
    fputs(e->name, stdout);
    if (opt_F) { char c = classify_char(e); if (c) putchar(c); }
}

/* Name lookups go through NSS and are slow; consecutive entries usually share owner/group, so cache the last one. */
static const char *user_name(uid_t uid, char *buf)
{
    static uid_t last = (uid_t)-1; static char name[32];
    if (uid != last) {
        struct passwd *pw = getpwuid(uid);
        if (pw) snprintf(name, sizeof name, "%s", pw->pw_name); else snprintf(name, sizeof name, "%u", uid);
        last = uid;
    }
    snprintf(buf, 32, "%s", name);
    return buf;
}

static const char *group_name(gid_t gid, char *buf)
{
    static gid_t last = (gid_t)-1; static char name[32];
    if (gid != last) {
        struct group *gr = getgrgid(gid);
        if (gr) snprintf(name, sizeof name, "%s", gr->gr_name); else snprintf(name, sizeof name, "%u", gid);
        last = gid;
    }
    snprintf(buf, 32, "%s", name);
    return buf;
}

/* Format owner, group and size columns for one entry. */
static void fmt_fields(const struct stat *s, char *own, char *grp, char *sz)
{
    user_name(s->st_uid, own);
    group_name(s->st_gid, grp);
    if (S_ISCHR(s->st_mode) || S_ISBLK(s->st_mode))
        snprintf(sz, 40, "%u, %u", major(s->st_rdev), minor(s->st_rdev));
    else if (opt_h)
        human_size((unsigned long long)s->st_size, sz, 40);
    else
        snprintf(sz, 40, "%lld", (long long)s->st_size);
}

struct widths { int ino, nl, own, grp, sz, maj, min; };

static void widen(struct widths *w, const entry *e)
{
    if (!e->have_st) return;
    char own[32], grp[32], sz[40];
    fmt_fields(&e->st, own, grp, sz);
    if (ndigits(e->st.st_ino)   > w->ino) w->ino = ndigits(e->st.st_ino);
    if (ndigits(e->st.st_nlink) > w->nl)  w->nl  = ndigits(e->st.st_nlink);
    if ((int)strlen(own) > w->own) w->own = (int)strlen(own);
    if ((int)strlen(grp) > w->grp) w->grp = (int)strlen(grp);
    if (S_ISCHR(e->st.st_mode) || S_ISBLK(e->st.st_mode)) {
        if (ndigits(major(e->st.st_rdev)) > w->maj) w->maj = ndigits(major(e->st.st_rdev));
        if (ndigits(minor(e->st.st_rdev)) > w->min) w->min = ndigits(minor(e->st.st_rdev));
    } else if ((int)strlen(sz) > w->sz) {
        w->sz = (int)strlen(sz);
    }
}

/*
 * Long listing. `extra` are additional entries (the directory operands) that
 * GNU ls also counts when it computes column widths for the file operands.
 */
static void print_long(entry *v, size_t n, int show_total, const entry *extra, size_t nextra)
{
    struct widths w = {0, 0, 0, 0, 0, 0, 0};
    unsigned long long blocks = 0;
    for (size_t i = 0; i < n; i++)      { widen(&w, &v[i]); if (v[i].have_st) blocks += (unsigned long long)v[i].st.st_blocks; }
    for (size_t i = 0; i < nextra; i++) widen(&w, &extra[i]);
    if (w.maj || w.min) {                       /* device files present: "major, minor" must fit the size column */
        if (w.maj + w.min + 2 > w.sz) w.sz = w.maj + w.min + 2;
        else                          w.maj = w.sz - w.min - 2;
    }

    if (show_total) {
        unsigned long long kb = (blocks + 1) / 2;      /* 512-byte units -> 1K, rounded up */
        if (opt_h) { char b[40]; human_size(kb * 1024ULL, b, sizeof b); printf("total %s\n", b); }
        else       printf("total %llu\n", kb);
    }

    time_t now = time(NULL);
    for (size_t i = 0; i < n; i++) {
        entry *e = &v[i];
        if (!e->have_st) {
            printf("?????????? ? ? ? ? ? %s\n", e->name);
            continue;
        }
        const struct stat *s = &e->st;
        char ms[11], tbuf[32], own[32], grp[32], sz[40];
        mode_string(s->st_mode, ms);
        fmt_fields(s, own, grp, sz);

        struct tm tm;
        localtime_r(&s->st_mtim.tv_sec, &tm);
        long long age = (long long)now - (long long)s->st_mtim.tv_sec;
        /* GNU ls: files older than ~6 months (or in the future) show the year */
        if (age > 15778476LL || age < 0) strftime(tbuf, sizeof tbuf, "%b %e  %Y", &tm);
        else                              strftime(tbuf, sizeof tbuf, "%b %e %H:%M", &tm);

        if (opt_i) printf("%*llu ", w.ino, (unsigned long long)s->st_ino);
        if (opt_O) printf("%04o ", (unsigned)(s->st_mode & 07777));
        if (S_ISCHR(s->st_mode) || S_ISBLK(s->st_mode))
            snprintf(sz, sizeof sz, "%*u, %*u", w.maj, major(s->st_rdev), w.min, minor(s->st_rdev));
        printf("%s %*llu %-*s %-*s %*s %s ", ms,
               w.nl, (unsigned long long)s->st_nlink,
               w.own, own, w.grp, grp, w.sz, sz, tbuf);

        if (S_ISLNK(s->st_mode)) {
            fputs(e->name, stdout);                      /* no indicator on the link name itself */
            if (e->link) {
                printf(" -> %s", e->link);
                if (opt_F) {                             /* GNU ls classifies the link *target* */
                    struct stat ts;
                    if (stat(e->path, &ts) == 0) { char c = classify_mode(ts.st_mode); if (c) putchar(c); }
                }
            }
        } else {
            print_name(e);
        }
        putchar('\n');
    }
}

static void print_short(entry *v, size_t n)
{
    int tty = isatty(STDOUT_FILENO);
    if (opt_1 || !tty || n == 0) {
        for (size_t i = 0; i < n; i++) {
            if (opt_i) printf("%llu ", (unsigned long long)v[i].ino);
            print_name(&v[i]);
            putchar('\n');
        }
        return;
    }
    /* column layout, filled top-to-bottom like ls */
    size_t maxlen = 0;
    for (size_t i = 0; i < n; i++) {
        size_t l = strlen(v[i].name) + (opt_F && classify_char(&v[i]) ? 1 : 0);
        if (opt_i) l += (size_t)ndigits(v[i].ino) + 1;
        if (l > maxlen) maxlen = l;
    }
    size_t colw = maxlen + 2;
    size_t cols = (size_t)term_width() / colw;
    if (cols < 1) cols = 1;
    size_t rows = (n + cols - 1) / cols;
    for (size_t r = 0; r < rows; r++) {
        for (size_t c = 0; c < cols; c++) {
            size_t idx = c * rows + r;
            if (idx >= n) break;
            size_t l = strlen(v[idx].name) + (opt_F && classify_char(&v[idx]) ? 1 : 0);
            if (opt_i) { printf("%llu ", (unsigned long long)v[idx].ino);
                         l += (size_t)ndigits(v[idx].ino) + 1; }
            print_name(&v[idx]);
            if (idx + rows < n) for (size_t k = l; k < colw; k++) putchar(' ');
        }
        putchar('\n');
    }
}

/* ================= entry management ================= */

/* Record a failure: serious (2) for command-line operands, minor (1) otherwise. */
static void note_failure(int cmdline)
{
    if (cmdline) exit_status = 2;
    else if (!exit_status) exit_status = 1;
}

static void fill_entry(entry *e, const char *name, const char *path, int follow, int need_stat, int cmdline)
{
    e->name = xstrdup(name);
    e->path = xstrdup(path);
    e->link = NULL;
    e->have_st = 0;
    e->ino = 0;
    e->dtype = DT_UNKNOWN;
    if (!need_stat) return;               /* like GNU ls: no stat() unless required */
    e->have_st = (follow ? stat(path, &e->st) : lstat(path, &e->st)) == 0;
    if (!e->have_st) { report("cannot access", path); note_failure(cmdline); return; }
    e->ino = e->st.st_ino;
    if (opt_l && S_ISLNK(e->st.st_mode)) {
        char buf[4096];
        ssize_t k = readlink(path, buf, sizeof buf - 1);
        if (k >= 0) { buf[k] = '\0'; e->link = xstrdup(buf); }
        else { report("cannot read symbolic link", path); note_failure(cmdline); }
    }
}

static void free_entries(entry *v, size_t n)
{
    for (size_t i = 0; i < n; i++) { free(v[i].name); free(v[i].path); free(v[i].link); }
    free(v);
}

/* ================= directory listing ================= */

static void list_directory(const char *dir, int print_header, int *first, int cmdline)
{
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        report("cannot open directory", dir);
        if (cmdline) exit_status = 2;                         /* serious: user asked for it */
        else if (!exit_status) exit_status = 1;               /* minor: found while recursing */
        return;
    }

    int need_stat = opt_l || opt_t || opt_S || opt_F;
    entry  *v = NULL;
    size_t  n = 0, cap = 0;
    char    buf[32768];
    long    nread;

    while ((nread = syscall(SYS_getdents64, fd, buf, sizeof buf)) > 0) {
        for (long off = 0; off < nread; ) {
            struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + off);
            off += d->d_reclen;
            if (!opt_a && d->d_name[0] == '.') continue;      /* hide dotfiles */

            if (n == cap) { cap = cap ? cap * 2 : 64; v = realloc(v, cap * sizeof *v); if (!v) { perror(prog); exit(2); } }
            size_t plen = strlen(dir) + strlen(d->d_name) + 2;
            char *path = xmalloc(plen);
            if (dir[0] != '\0' && dir[strlen(dir) - 1] == '/') snprintf(path, plen, "%s%s", dir, d->d_name);
            else                                               snprintf(path, plen, "%s/%s", dir, d->d_name);
            fill_entry(&v[n], d->d_name, path, 0, need_stat, 0);
            if (!v[n].ino) v[n].ino = d->d_ino;
            v[n].dtype = d->d_type;
            free(path);
            n++;
        }
    }
    if (nread < 0) { report("error reading directory", dir); exit_status = exit_status ? exit_status : 1; }
    close(fd);

    if (n > 1) qsort(v, n, sizeof *v, cmp_entry);

    if (print_header) { if (!*first) putchar('\n'); printf("%s:\n", dir); }
    *first = 0;

    if (opt_l) print_long(v, n, 1, NULL, 0);
    else       print_short(v, n);

    if (opt_R) {
        for (size_t i = 0; i < n; i++) {
            int isdir = v[i].have_st ? S_ISDIR(v[i].st.st_mode) : v[i].dtype == DT_DIR;
            if (v[i].dtype == DT_UNKNOWN && !v[i].have_st) {          /* filesystem gave no type hint */
                struct stat tmp;
                isdir = lstat(v[i].path, &tmp) == 0 && S_ISDIR(tmp.st_mode);
            }
            if (!isdir) continue;
            if (!strcmp(v[i].name, ".") || !strcmp(v[i].name, "..")) continue;
            list_directory(v[i].path, 1, first, 0);
        }
    }
    free_entries(v, n);
}

/* ================= main ================= */

int main(int argc, char **argv)
{
    int c;
    prog = "myls";
    opterr = 0;                                   /* we print our own option diagnostics */
    while ((c = getopt(argc, argv, "adlRiShtr1FO")) != -1) {
        switch (c) {
        case 'a': opt_a = 1; break;
        case 'd': opt_d = 1; break;
        case 'l': opt_l = 1; break;
        case 'R': opt_R = 1; break;
        case 'i': opt_i = 1; break;
        case 'S': opt_S = 1; break;
        case 'h': opt_h = 1; break;
        case 't': opt_t = 1; break;
        case 'r': opt_r = 1; break;
        case '1': opt_1 = 1; break;
        case 'F': opt_F = 1; break;
        case 'O': opt_O = 1; opt_l = 1; break;
        default:
            fprintf(stderr, "%s: invalid option -- '%c'\nusage: %s [-adlRiShtr1FO] [file ...]\n", prog, optopt ? optopt : '?', prog);
            return 2;
        }
    }

    int nargs = argc - optind;
    char **args = argv + optind;
    static char *dot[] = { ".", NULL };
    if (nargs == 0) { args = dot; nargs = 1; }

    /* Split command-line operands into files and directories (like GNU ls). */
    entry *files = xmalloc((size_t)nargs * sizeof *files);
    entry *dirs  = xmalloc((size_t)nargs * sizeof *dirs);
    size_t nf = 0, nd = 0;

    for (int i = 0; i < nargs; i++) {
        struct stat st;
        if (lstat(args[i], &st) != 0) {
            report("cannot access", args[i]);
            exit_status = 2;
            continue;
        }
        /* Without -l, a symlink to a directory is followed (as in GNU ls). */
        int follow = S_ISLNK(st.st_mode) && !opt_l && !opt_d && stat(args[i], &st) == 0 && S_ISDIR(st.st_mode);
        entry tmp;
        fill_entry(&tmp, args[i], args[i], follow, 1, 1);
        if (!tmp.have_st) continue;
        if (S_ISDIR(tmp.st.st_mode) && !opt_d) dirs[nd++] = tmp;
        else                         files[nf++] = tmp;
    }

    if (nf > 1) qsort(files, nf, sizeof *files, cmp_entry);
    if (nd > 1) qsort(dirs,  nd, sizeof *dirs,  cmp_entry);

    int first = 1;
    if (nf) {
        if (opt_l) print_long(files, nf, 0, dirs, nd);
        else       print_short(files, nf);
        first = 0;
    }
    int header = (nargs > 1) || opt_R;
    for (size_t i = 0; i < nd; i++)
        list_directory(dirs[i].path, header, &first, 1);

    free_entries(files, nf);
    free_entries(dirs, nd);
    fflush(stdout);
    return exit_status;
}
