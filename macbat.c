/*
 * macbat - battery charge limiter and battery status CLI for Apple laptops.
 *
 *   Linux on Intel Macs (applesmc): hardware limit via SMC BCLM, auto-discharge via ACEN / CH0I
 *   macOS on Apple Silicon:         limit via CHTE (or CH0B/CH0C), auto-discharge via CHIE / CH0I
 *   macOS on Intel:                 BCLM / CH0I (experimental)
 *   fans: applesmc sysfs on Linux, SMC F<n>Ac/Mn/Mx/Tg/Md + FS! on macOS
 *
 * Single file, no dependencies.  Build:  make   (see Makefile)
 * SPDX-License-Identifier: MIT
 */
#define MACBAT_VERSION "0.6.0"
#if defined(__linux__)
#  define _GNU_SOURCE /* struct ucred (SO_PEERCRED) */
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <langinfo.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <pwd.h>
#include <grp.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#  if !defined(__x86_64__) && !defined(__i386__)
#    error "Linux support is for Intel Macs (x86) only"
#  endif
#  include <dirent.h>
#  include <sys/io.h>
#  define OS_LINUX 1
#elif defined(__APPLE__)
#  include <CoreFoundation/CoreFoundation.h>
#  include <IOKit/IOKitLib.h>
#  include <IOKit/IOMessage.h>
#  include <IOKit/pwr_mgt/IOPMLib.h>
#  include <mach-o/dyld.h>
#  include <mach/mach.h>
#  include <sys/sysctl.h>
#  define OS_MAC 1
#else
#  error "unsupported platform"
#endif

extern char **environ;

#define CONF_PATH "/etc/macbat.conf"
#ifdef OS_LINUX
#  define STATE_PATH "/run/macbat.state"
#  define LOCK_PATH "/run/macbat.lock"
#else
#  define STATE_PATH "/var/run/macbat.state"
#  define LOCK_PATH "/var/run/macbat.lock"
#endif
#define SAFETY_FLOOR 10
#define LOCAL_BIN "/usr/local/bin/macbat"
#define SYSTEMD_UNIT "/etc/systemd/system/macbat.service"
#define SLEEP_HOOK "/usr/lib/systemd/system-sleep/macbat"
#define LAUNCHD_LABEL "com.neuralfoundry.macbat"
#define LAUNCHD_PLIST "/Library/LaunchDaemons/" LAUNCHD_LABEL ".plist"

/* ------------------------------------------------------------------ small helpers */
static bool g_force = false, g_quiet = false, g_plain = false;

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("macbat: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static void logmsg(const char *fmt, ...) {
    char ts[32];
    time_t t = time(NULL);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
    printf("%s ", ts);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

static void need_root(const char *what) {
    if (geteuid() != 0) die("'%s' needs root - try: sudo macbat %s", what, what);
}

static bool read_text(const char *path, char *buf, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    size_t r = fread(buf, 1, n - 1, f);
    fclose(f);
    buf[r] = 0;
    while (r && (buf[r - 1] == '\n' || buf[r - 1] == ' ')) buf[--r] = 0;
    return true;
}

__attribute__((unused)) static long read_long(const char *path, long dflt) {
    char b[64];
    if (!read_text(path, b, sizeof b) || !*b) return dflt;
    char *e;
    long v = strtol(b, &e, 10);
    return e == b ? dflt : v;
}

static bool write_text(const char *path, const char *text, mode_t mode) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    fputs(text, f);
    if (fclose(f) != 0) return false;
    chmod(tmp, mode);
    return rename(tmp, path) == 0;
}

__attribute__((unused)) static bool write_file(const char *path, const char *val) {
    int fd = open(path, O_WRONLY | O_NOCTTY);
    if (fd < 0) return false;
    size_t len = strlen(val);
    ssize_t n = write(fd, val, len);
    close(fd);
    return n == (ssize_t)len;
}

static long iround(double x) { return x < 0 ? -(long)(-x + 0.5) : (long)(x + 0.5); }
__attribute__((unused)) static double dabs(double x) { return x < 0 ? -x : x; }

static bool exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

/* run a command, stdout/stderr discarded (or captured into out). returns exit status, -1 on failure */
static int run_cmd(char *out, size_t outn, const char *const argv[]) {
    int pfd[2] = {-1, -1};
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (out && pipe(pfd) == 0) {
        posix_spawn_file_actions_adddup2(&fa, pfd[1], 1);
        posix_spawn_file_actions_addclose(&fa, pfd[0]);
    } else {
        posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    }
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], &fa, NULL, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (out) {
        if (pfd[1] >= 0) close(pfd[1]);
        size_t got = 0;
        if (rc == 0 && pfd[0] >= 0) {
            ssize_t r;
            while (got + 1 < outn && (r = read(pfd[0], out + got, outn - 1 - got)) > 0) got += (size_t)r;
        }
        out[got] = 0;
        if (pfd[0] >= 0) close(pfd[0]);
    }
    if (rc != 0) return -1;
    int st;
    if (waitpid(pid, &st, 0) < 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
#define RUN(...) run_cmd(NULL, 0, (const char *const[]){__VA_ARGS__, NULL})
#define RUN_OUT(buf, ...) run_cmd(buf, sizeof(buf), (const char *const[]){__VA_ARGS__, NULL})

static bool self_path(char *buf, size_t n) {
#ifdef OS_LINUX
    ssize_t r = readlink("/proc/self/exe", buf, n - 1);
    if (r <= 0) return false;
    buf[r] = 0;
    return true;
#else
    (void)n; /* realpath() needs a PATH_MAX buffer; callers pass >= 1024 */
    char tmp[4096];
    uint32_t sz = (uint32_t)sizeof tmp;
    if (_NSGetExecutablePath(tmp, &sz) != 0) return false;
    return realpath(tmp, buf) != NULL;
#endif
}

static bool copy_file(const char *src, const char *dst, mode_t mode) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.new", dst);
    int in = open(src, O_RDONLY);
    if (in < 0) return false;
    int o = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (o < 0) { close(in); return false; }
    char b[65536];
    ssize_t r;
    bool ok = true;
    while ((r = read(in, b, sizeof b)) > 0)
        if (write(o, b, (size_t)r) != r) { ok = false; break; }
    close(in);
    if (close(o) != 0) ok = false;
    if (ok) { chmod(tmp, mode); ok = rename(tmp, dst) == 0; }
    if (!ok) unlink(tmp);
    return ok;
}

static bool utf8_out(void) {
    const char *cs = nl_langinfo(CODESET);
    return cs && (strcasecmp(cs, "UTF-8") == 0 || strcasecmp(cs, "utf8") == 0);
}

/* ------------------------------------------------------------------ config */
typedef struct {
    int limit; bool auto_discharge; int hysteresis; int interval;
    int screen_blank; /* seconds, -1 = leave alone */
    char sata_alpm[32], wifi_ps[8], usb_as[8], cpu_gov[32], cpu_turbo[8], aspm[32], spindown[8];
    char fan[16];          /* "auto", "<rpm>" or "<n>%" of the fan's min..max range */
    char fan_paused[32];   /* fan daemon (mbpfan) that macbat stopped for a manual speed */
    bool fan_paused_enabled;
    char theme[16];        /* look of the status / settings screens; empty = default (modern) */
    bool topup;            /* charge to 100% once, then back to the limit (cleared by the service) */
    long topup_since;
} conf_t;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static conf_t load_conf(void) {
    conf_t c;
    memset(&c, 0, sizeof c);
    c.limit = 80; c.auto_discharge = true; c.hysteresis = 3; c.interval = 30; c.screen_blank = -1;
    snprintf(c.fan, sizeof c.fan, "auto");
    FILE *f = fopen(CONF_PATH, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            char *h = strchr(line, '#');
            if (h) *h = 0;
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            char *k = line, *v = eq + 1;
            while (isspace((unsigned char)*k)) k++;
            for (char *e = k + strlen(k); e > k && isspace((unsigned char)e[-1]);) *--e = 0;
            while (isspace((unsigned char)*v)) v++;
            for (char *e = v + strlen(v); e > v && isspace((unsigned char)e[-1]);) *--e = 0;
            if (!strcmp(k, "limit")) c.limit = atoi(v);
            else if (!strcmp(k, "hysteresis")) c.hysteresis = atoi(v);
            else if (!strcmp(k, "interval")) c.interval = atoi(v);
#define CS(name) else if (!strcmp(k, #name)) snprintf(c.name, sizeof c.name, "%s", v)
            CS(sata_alpm); CS(wifi_ps); CS(usb_as); CS(cpu_gov); CS(cpu_turbo); CS(aspm); CS(spindown);
            CS(fan); CS(fan_paused); CS(theme);
#undef CS
            else if (!strcmp(k, "fan_paused_enabled")) c.fan_paused_enabled = !strcasecmp(v, "on");
            else if (!strcmp(k, "topup")) c.topup = !strcasecmp(v, "on");
            else if (!strcmp(k, "topup_since")) c.topup_since = atol(v);
            else if (!strcmp(k, "screen_blank")) c.screen_blank = *v ? atoi(v) : -1;
            else if (!strcmp(k, "auto_discharge"))
                c.auto_discharge = !strcasecmp(v, "on") || !strcasecmp(v, "1") || !strcasecmp(v, "yes") || !strcasecmp(v, "true");
        }
        fclose(f);
    }
    c.limit = clampi(c.limit, 20, 100);
    c.hysteresis = clampi(c.hysteresis, 1, 20);
    c.interval = clampi(c.interval, 5, 600);
    if (!*c.fan) snprintf(c.fan, sizeof c.fan, "auto");
    return c;
}

static void save_conf(conf_t c) {
    char b[2048], t[96];
    snprintf(b, sizeof b,
             "# macbat configuration (re-read automatically by the macbat service)\n"
             "limit=%d\nauto_discharge=%s\nhysteresis=%d\ninterval=%d\n",
             c.limit, c.auto_discharge ? "on" : "off", c.hysteresis, c.interval);
    if (c.screen_blank >= 0) { snprintf(t, sizeof t, "screen_blank=%d\n", c.screen_blank); strcat(b, t); }
#define CS(name) if (*c.name) { snprintf(t, sizeof t, #name "=%s\n", c.name); strcat(b, t); }
    CS(sata_alpm) CS(wifi_ps) CS(usb_as) CS(cpu_gov) CS(cpu_turbo) CS(aspm) CS(spindown)
#undef CS
    snprintf(t, sizeof t, "fan=%s\n", c.fan);
    strcat(b, t);
    if (*c.theme) { snprintf(t, sizeof t, "theme=%s\n", c.theme); strcat(b, t); }
    if (c.topup) { snprintf(t, sizeof t, "topup=on\ntopup_since=%ld\n", c.topup_since); strcat(b, t); }
    if (*c.fan_paused) {
        snprintf(t, sizeof t, "fan_paused=%s\nfan_paused_enabled=%s\n", c.fan_paused, c.fan_paused_enabled ? "on" : "off");
        strcat(b, t);
    }
    if (!write_text(CONF_PATH, b, 0644)) die("cannot write %s: %s", CONF_PATH, strerror(errno));
}

static time_t conf_mtime(void) { struct stat st; return stat(CONF_PATH, &st) == 0 ? st.st_mtime : 0; }

/* ------------------------------------------------------------------ SMC values */
typedef struct { char type[5]; uint8_t data[32]; int size; } smcval_t;

/* Apple Silicon SMCs are little-endian, Intel SMCs big-endian. Decided at run time so the x86_64
 * slice of the universal binary still decodes correctly when run under Rosetta. */
static bool apple_silicon(void) {
#ifdef OS_MAC
    int v = 0;
    size_t sz = sizeof v;
    return sysctlbyname("hw.optional.arm64", &v, &sz, NULL, 0) == 0 && v == 1;
#else
    return false;
#endif
}
#define SMC_LE apple_silicon()

static double smc_number(const smcval_t *v, bool *ok) {
    const uint8_t *d = v->data;
    char t[5];
    snprintf(t, sizeof t, "%s", v->type);
    for (int i = 3; i >= 0 && (t[i] == ' ' || !t[i]); i--) t[i] = 0;
    *ok = true;
    uint32_t u16 = SMC_LE ? (uint32_t)(d[0] | d[1] << 8) : (uint32_t)(d[0] << 8 | d[1]);
    uint32_t u32 = SMC_LE ? (uint32_t)d[0] | (uint32_t)d[1] << 8 | (uint32_t)d[2] << 16 | (uint32_t)d[3] << 24
                          : (uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | (uint32_t)d[3];
    if ((!strcmp(t, "ui8") || !strcmp(t, "flag") || !strcmp(t, "hex_")) && v->size == 1) return d[0];
    if (!strcmp(t, "si8") && v->size == 1) return (int8_t)d[0];
    if (!strcmp(t, "ui16") && v->size == 2) return u16;
    if (!strcmp(t, "si16") && v->size == 2) return (int16_t)u16;
    if (!strcmp(t, "ui32") && v->size == 4) return u32;
    if (!strcmp(t, "si32") && v->size == 4) return (int32_t)u32;
    if (!strcmp(t, "sp78") && v->size == 2) return (int16_t)u16 / 256.0;
    if (!strcmp(t, "fpe2") && v->size == 2) return (d[0] << 8 | d[1]) / 4.0; /* always big-endian */
    if (!strcmp(t, "flt") && v->size == 4) { float f; memcpy(&f, &u32, 4); return f; }
    *ok = false;
    return 0;
}

/* ------------------------------------------------------------------ SMC: Linux */
#ifdef OS_LINUX
#define SMC_SYS "/sys/devices/platform/applesmc.768"
#define SMC_DATA 0x300
#define SMC_CMD 0x304
#define SMC_READ 0x10
#define SMC_WRITE 0x11
#define ST_AWAIT 0x01
#define ST_IBCLOSED 0x02
#define ST_BUSY 0x04

static struct { char key[5]; int idx; } g_idx[32];
static int g_nidx = 0;

static bool smc_available(void) { return exists(SMC_SYS "/key_count"); }

static bool sysfs_name_at(int i, char name[5]) {
    char b[16];
    int fd = open(SMC_SYS "/key_at_index", O_WRONLY);
    if (fd < 0) return false;
    int n = snprintf(b, sizeof b, "%d", i);
    bool ok = write(fd, b, (size_t)n) == n;
    close(fd);
    if (!ok) return false;
    fd = open(SMC_SYS "/key_at_index_name", O_RDONLY);
    if (fd < 0) return false;
    ssize_t r = read(fd, b, sizeof b);
    close(fd);
    if (r < 4) return false;
    memcpy(name, b, 4);
    name[4] = 0;
    return true;
}

static int smc_index(const char *key) {
    for (int i = 0; i < g_nidx; i++)
        if (!memcmp(g_idx[i].key, key, 4)) return g_idx[i].idx;
    int n = (int)read_long(SMC_SYS "/key_count", 0), lo = 0, hi = n - 1, found = -1;
    char name[5];
    while (lo <= hi) { /* keys are sorted by their 32-bit value */
        int mid = (lo + hi) / 2;
        if (!sysfs_name_at(mid, name)) return -2;
        int c = memcmp(name, key, 4);
        if (!c) { found = mid; break; }
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    if (found < 0)
        for (int i = 0; i < n; i++) {
            if (!sysfs_name_at(i, name)) return -2;
            if (!memcmp(name, key, 4)) { found = i; break; }
        }
    if (g_nidx < 32) { memcpy(g_idx[g_nidx].key, key, 5); g_idx[g_nidx++].idx = found; }
    return found;
}

/* returns 1 found, 0 absent, -1 no permission/error */
static int smc_read(const char *key, smcval_t *v) {
    int i = smc_index(key);
    if (i == -2) return -1;
    if (i < 0) return 0;
    char name[5];
    if (!sysfs_name_at(i, name) || memcmp(name, key, 4)) { g_nidx = 0; return -1; }
    char t[16] = "";
    read_text(SMC_SYS "/key_at_index_type", t, sizeof t);
    snprintf(v->type, sizeof v->type, "%-4.4s", t);
    int fd = open(SMC_SYS "/key_at_index_data", O_RDONLY);
    if (fd < 0) return -1;
    ssize_t r = read(fd, v->data, sizeof v->data);
    close(fd);
    if (r < 0) return -1;
    v->size = (int)r;
    return 1;
}

static int wait_status(uint8_t val, uint8_t mask) {
    for (unsigned us = 16; us < 0x20000; us <<= 1) {
        if ((inb(SMC_CMD) & mask) == val) return 0;
        usleep(us);
    }
    return -1;
}
static int send_byte(uint8_t b, uint16_t port) {
    if (wait_status(0, ST_IBCLOSED) || wait_status(ST_BUSY, ST_BUSY)) return -1;
    outb(b, port);
    return 0;
}
static int send_cmd(uint8_t c) {
    if (wait_status(0, ST_IBCLOSED)) return -1;
    outb(c, SMC_CMD);
    return 0;
}
static int smc_sane(void) {
    if (!wait_status(0, ST_BUSY)) return 0;
    if (send_cmd(SMC_READ)) return -1;
    return wait_status(0, ST_BUSY);
}
static int port_read(const char *key, uint8_t *buf, int n) {
    if (smc_sane() || send_cmd(SMC_READ)) return -1;
    for (int i = 0; i < 4; i++) if (send_byte((uint8_t)key[i], SMC_DATA)) return -1;
    if (send_byte((uint8_t)n, SMC_DATA)) return -1;
    for (int i = 0; i < n; i++) {
        if (wait_status(ST_AWAIT | ST_BUSY, ST_AWAIT | ST_BUSY)) return -1;
        buf[i] = inb(SMC_DATA);
    }
    for (int i = 0; i < 16; i++) { usleep(16); if (!(inb(SMC_CMD) & ST_AWAIT)) break; (void)inb(SMC_DATA); }
    return wait_status(0, ST_BUSY);
}
static int port_write(const char *key, const uint8_t *buf, int n) {
    if (smc_sane() || send_cmd(SMC_WRITE)) return -1;
    for (int i = 0; i < 4; i++) if (send_byte((uint8_t)key[i], SMC_DATA)) return -1;
    if (send_byte((uint8_t)n, SMC_DATA)) return -1;
    for (int i = 0; i < n; i++) if (send_byte(buf[i], SMC_DATA)) return -1;
    return wait_status(0, ST_BUSY);
}

/* mbpfan polls the SMC and keeps applesmc files open: stop it around raw writes, never block on systemd */
static bool pause_mbpfan(void) {
    if (RUN("systemctl", "is-active", "--quiet", "mbpfan") != 0) return false;
    if (RUN("timeout", "20", "systemctl", "stop", "mbpfan") != 0) RUN("pkill", "-STOP", "-x", "mbpfan");
    return true;
}

typedef struct { const char *key; uint8_t data[4]; int len; } wr_t;

static bool smc_write(const wr_t *w, int n, char *err, size_t errn) {
    bool mbpfan = pause_mbpfan();
    bool unloaded = RUN("modprobe", "-r", "applesmc") == 0;
    bool ok = true;
    err[0] = 0;
    if (ioperm(SMC_DATA, 5, 1) != 0) {
        snprintf(err, errn, "ioperm: %s", strerror(errno));
        ok = false;
    } else {
        for (int i = 0; i < n && ok; i++) {
            uint8_t back[4] = {0};
            if (port_write(w[i].key, w[i].data, w[i].len) || port_read(w[i].key, back, w[i].len)) {
                snprintf(err, errn, "%s: SMC timeout", w[i].key);
                ok = false;
            } else if (memcmp(back, w[i].data, (size_t)w[i].len)) {
                snprintf(err, errn, "%s: write verify failed", w[i].key);
                ok = false;
            }
        }
        ioperm(SMC_DATA, 5, 0);
    }
    if (unloaded) {
        RUN("modprobe", "applesmc");
        for (int i = 0; i < 50 && !smc_available(); i++) usleep(100000);
    }
    /* --no-block: we may be running inside our own systemd stop job */
    if (mbpfan) RUN("systemctl", "restart", "--no-block", "mbpfan");
    g_nidx = 0;
    return ok;
}
#endif /* OS_LINUX */

/* ------------------------------------------------------------------ SMC: macOS */
#ifdef OS_MAC
typedef struct { uint8_t major, minor, build, reserved; uint16_t release; } SMCVers;
typedef struct { uint16_t version, length; uint32_t cpuPLimit, gpuPLimit, memPLimit; } SMCPLimit;
typedef struct { uint32_t dataSize; uint32_t dataType; uint8_t dataAttributes; } SMCKeyInfo;
typedef struct {
    uint32_t key;
    SMCVers vers;
    SMCPLimit pLimitData;
    SMCKeyInfo keyInfo;
    uint8_t result, status, data8;
    uint32_t data32;
    uint8_t bytes[32];
} SMCKeyData;
_Static_assert(sizeof(SMCKeyData) == 80, "SMCKeyData must be 80 bytes");

static io_connect_t g_conn = 0;
static struct { char key[5]; int present; uint32_t size, type; } g_info[64];
static int g_ninfo = 0;

static bool smc_available(void) { return true; }

static bool smc_open(void) {
    if (g_conn) return true;
    io_service_t svc = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("AppleSMC"));
    if (!svc) return false;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &g_conn);
    IOObjectRelease(svc);
    return kr == KERN_SUCCESS;
}

static uint32_t key4(const char *k) { return (uint32_t)k[0] << 24 | (uint32_t)k[1] << 16 | (uint32_t)k[2] << 8 | (uint32_t)k[3]; }

static kern_return_t smc_call(SMCKeyData *in, SMCKeyData *out) {
    size_t n = sizeof *out;
    return IOConnectCallStructMethod(g_conn, 2, in, sizeof *in, out, &n);
}

static int smc_keyinfo(const char *key, uint32_t *size, uint32_t *type) {
    for (int i = 0; i < g_ninfo; i++)
        if (!memcmp(g_info[i].key, key, 4)) { *size = g_info[i].size; *type = g_info[i].type; return g_info[i].present; }
    if (!smc_open()) return -1;
    SMCKeyData in = {0}, out = {0};
    in.key = key4(key);
    in.data8 = 9; /* READ_KEYINFO */
    if (smc_call(&in, &out) != KERN_SUCCESS) return -1;
    int present = out.result == 0;
    *size = out.keyInfo.dataSize;
    *type = out.keyInfo.dataType;
    if (g_ninfo < 64) {
        memcpy(g_info[g_ninfo].key, key, 5);
        g_info[g_ninfo].present = present;
        g_info[g_ninfo].size = *size;
        g_info[g_ninfo++].type = *type;
    }
    return present;
}

static int smc_read(const char *key, smcval_t *v) {
    uint32_t size, type;
    int p = smc_keyinfo(key, &size, &type);
    if (p <= 0) return p;
    SMCKeyData in = {0}, out = {0};
    in.key = key4(key);
    in.data8 = 5; /* READ_BYTES */
    in.keyInfo.dataSize = size;
    if (smc_call(&in, &out) != KERN_SUCCESS || out.result) return -1;
    v->size = (int)(size > 32 ? 32 : size);
    memcpy(v->data, out.bytes, (size_t)v->size);
    v->type[0] = (char)(type >> 24); v->type[1] = (char)(type >> 16); v->type[2] = (char)(type >> 8); v->type[3] = (char)type;
    v->type[4] = 0;
    return 1;
}

typedef struct { const char *key; uint8_t data[4]; int len; } wr_t;

static bool smc_write(const wr_t *w, int n, char *err, size_t errn) {
    err[0] = 0;
    for (int i = 0; i < n; i++) {
        uint32_t size, type;
        if (smc_keyinfo(w[i].key, &size, &type) != 1) { snprintf(err, errn, "%s: key not present", w[i].key); return false; }
        if ((int)size != w[i].len) { snprintf(err, errn, "%s: size %u != %d", w[i].key, size, w[i].len); return false; }
        SMCKeyData in = {0}, out = {0};
        in.key = key4(w[i].key);
        in.data8 = 6; /* WRITE_BYTES */
        in.keyInfo.dataSize = size;
        memcpy(in.bytes, w[i].data, (size_t)w[i].len);
        kern_return_t kr = smc_call(&in, &out);
        if (kr != KERN_SUCCESS || out.result) {
            snprintf(err, errn, "%s: write failed (kr=%#x result=%u)%s", w[i].key, kr, out.result,
                     kr == kIOReturnNotPrivileged ? " - needs sudo" : "");
            return false;
        }
        smcval_t back;
        if (smc_read(w[i].key, &back) != 1 || memcmp(back.data, w[i].data, (size_t)w[i].len)) {
            snprintf(err, errn, "%s: write verify failed", w[i].key);
            return false;
        }
    }
    return true;
}
#endif /* OS_MAC */

static bool smc_has(const char *key) { smcval_t v; return smc_read(key, &v) == 1; }

/* ------------------------------------------------------------------ capabilities + control state */
typedef struct { const char *hw_limit, *inhibit, *discharge; } caps_t;
typedef struct { int hw_limit, charging_blocked, adapter_off; } control_t; /* -1 = unknown */

static bool get_caps(caps_t *c) {
    smcval_t v;
    if (smc_read("BCLM", &v) < 0) return false; /* no permission */
    c->hw_limit = smc_has("BCLM") ? "BCLM" : NULL;
    c->inhibit = smc_has("CHTE") ? "CHTE" : (smc_has("CH0B") && smc_has("CH0C")) ? "CH0B" : NULL;
#ifdef OS_LINUX
    c->discharge = smc_has("ACEN") ? "ACEN" : smc_has("CH0I") ? "CH0I" : NULL; /* ACEN=0 verified on MacBookPro12,1 */
#else
    c->discharge = smc_has("CHIE") ? "CHIE" : smc_has("CH0I") ? "CH0I" : NULL;
#endif
    return true;
}

static control_t get_control(const caps_t *c) {
    control_t s = {-1, -1, -1};
    smcval_t v;
    if (c->hw_limit && smc_read("BCLM", &v) == 1) s.hw_limit = v.data[0];
    if (c->inhibit && smc_read(c->inhibit, &v) == 1) {
        int any = 0;
        for (int i = 0; i < v.size; i++) any |= v.data[i];
        s.charging_blocked = any != 0;
    }
    if (c->discharge && smc_read(c->discharge, &v) == 1) {
        uint8_t b = v.data[0];
        s.adapter_off = !strcmp(c->discharge, "ACEN") ? b == 0 : !strcmp(c->discharge, "CHIE") ? (b & 0x08) != 0 : b != 0;
    }
    return s;
}

static int w_hw_limit(wr_t *w, int pct) { w[0] = (wr_t){"BCLM", {(uint8_t)pct}, 1}; return 1; }

static int w_charging(wr_t *w, const caps_t *c, bool enable) {
    if (!c->inhibit) return 0;
    if (!strcmp(c->inhibit, "CHTE")) { w[0] = (wr_t){"CHTE", {enable ? 0 : 1, 0, 0, 0}, 4}; return 1; }
    uint8_t v = enable ? 0 : 2;
    w[0] = (wr_t){"CH0B", {v}, 1};
    w[1] = (wr_t){"CH0C", {v}, 1};
    return 2;
}

static int w_adapter(wr_t *w, const caps_t *c, bool enable) {
    if (!c->discharge) return 0;
    if (!strcmp(c->discharge, "ACEN")) w[0] = (wr_t){"ACEN", {enable ? 1 : 0}, 1};
    else if (!strcmp(c->discharge, "CHIE")) w[0] = (wr_t){"CHIE", {enable ? 0 : 8}, 1};
    else w[0] = (wr_t){"CH0I", {enable ? 0 : 1}, 1};
    return 1;
}

/* ------------------------------------------------------------------ conflicts (other charge managers) */
static const char *conflict(void) {
#ifdef OS_MAC
    static const char *procs[][2] = {
        {"AlDente", "AlDente"},
        {"com.apphousekitchen.aldente-pro.helper", "AlDente"},
        {"batterytoolkitd", "Battery Toolkit"},
        {"me.mhaeuser.batterytoolkitd", "Battery Toolkit"},
    };
    char out[256];
    for (size_t i = 0; i < sizeof procs / sizeof procs[0]; i++)
        if (RUN_OUT(out, "pgrep", "-x", procs[i][0]) == 0) return procs[i][1];
    if (exists("/Library/LaunchDaemons/com.apphousekitchen.aldente-pro.helper.plist")) return "AlDente";
#endif
    return NULL;
}

static void refuse_if_conflict(const char *what) {
    const char *c = conflict();
    if (c && !g_force)
        die("%s is managing battery charging on this Mac - refusing to '%s' so the two do not fight.\n"
            "        Quit/uninstall %s first, or pass --force if you really want both.", c, what, c);
}

/* ------------------------------------------------------------------ fans */
/* A manual speed is always clamped to the fan's own [min, max]: fans are never stopped or slowed below the
 * SMC minimum.  "auto" hands the fans back to the SMC (or to mbpfan, if macbat had paused it). */
#define MAXFAN 4
#define FAN_HOT 90  /* thermal guard (daemon): back to automatic at this CPU temperature ... */
#define FAN_COOL 75 /* ... and the manual speed again once it has cooled down to this */
typedef struct { char label[24]; int cur, min, max, target, manual; } fan_t; /* rpm; manual: 1/0/-1 */
typedef struct { int n; fan_t f[MAXFAN]; } fans_t;

static bool fan_spec_ok(const char *s) {
    if (!strcmp(s, "auto")) return true;
    char *e;
    long v = strtol(s, &e, 10);
    if (e == s) return false;
    if (*e == '%' && !e[1]) return v >= 0 && v <= 100;
    return !*e && v > 0 && v <= 20000;
}

static bool fan_limits_ok(const fan_t *f) { return f->min > 0 && f->max > f->min; }

/* rpm for "3000" / "50%" (0% = min, 100% = max), clamped to [min, max]; 0 for "auto" */
static int fan_want(const char *spec, const fan_t *f) {
    if (!strcmp(spec, "auto") || !fan_limits_ok(f)) return 0;
    int v = atoi(spec);
    if (spec[strlen(spec) - 1] == '%') v = f->min + (int)iround((f->max - f->min) * clampi(v, 0, 100) / 100.0);
    return clampi(v, f->min, f->max);
}

static void fan_spec_text(const char *spec, char *o, size_t n) {
    if (!strcmp(spec, "auto")) snprintf(o, n, "auto");
    else if (spec[strlen(spec) - 1] == '%') snprintf(o, n, "%s", spec);
    else snprintf(o, n, "%s RPM", spec);
}

static int lock_smc(void) {
    int lk = open(LOCK_PATH, O_RDWR | O_CREAT, 0644);
    if (lk >= 0) flock(lk, LOCK_EX);
    return lk;
}
static void unlock_smc(int lk) { if (lk >= 0) { flock(lk, LOCK_UN); close(lk); } }

#ifdef OS_LINUX
static bool read_fans(fans_t *F) {
    memset(F, 0, sizeof *F);
    char p[128];
    for (int i = 1; i <= MAXFAN; i++) {
        snprintf(p, sizeof p, SMC_SYS "/fan%d_input", i);
        if (!exists(p)) break;
        fan_t *f = &F->f[F->n++];
        f->cur = (int)read_long(p, -1);
#define FR(field, name) (snprintf(p, sizeof p, SMC_SYS "/fan%d_" name, i), f->field = (int)read_long(p, -1))
        FR(min, "min"); FR(max, "max"); FR(target, "output"); FR(manual, "manual");
#undef FR
        snprintf(p, sizeof p, SMC_SYS "/fan%d_label", i);
        if (!read_text(p, f->label, sizeof f->label) || !*f->label) snprintf(f->label, sizeof f->label, "Fan %d", i);
    }
    return F->n > 0;
}

static bool fan_sysw(int i, const char *what, int v) {
    char p[128], b[16];
    snprintf(p, sizeof p, SMC_SYS "/fan%d_%s", i + 1, what);
    snprintf(b, sizeof b, "%d", v);
    return write_file(p, b);
}

/* rpm[i] > 0: manual at that speed, 0: automatic */
static bool fans_set(const fans_t *F, const int *rpm, char *err, size_t en) {
    int lk = lock_smc();
    bool ok = true;
    err[0] = 0;
    for (int i = 0; i < F->n && ok; i++) {
        ok = rpm[i] > 0 ? fan_sysw(i, "manual", 1) && fan_sysw(i, "output", rpm[i]) : fan_sysw(i, "manual", 0);
        if (!ok) snprintf(err, en, "fan %d: %s", i + 1, strerror(errno));
    }
    unlock_smc(lk);
    return ok;
}

/* other fan daemons would override a manual speed within a second */
static const char *FAN_DAEMONS[] = {"mbpfan", "macfanctld"};
static const char *fan_daemon(void) {
    for (size_t i = 0; i < sizeof FAN_DAEMONS / sizeof FAN_DAEMONS[0]; i++)
        if (RUN("systemctl", "is-active", "--quiet", FAN_DAEMONS[i]) == 0) return FAN_DAEMONS[i];
    return NULL;
}
static bool fan_daemon_pause(conf_t *c, char *note, size_t n) {
    const char *d = fan_daemon();
    if (!d) return true;
    bool en = RUN("systemctl", "is-enabled", "--quiet", d) == 0;
    if (RUN("timeout", "20", "systemctl", "stop", d) != 0) { snprintf(note, n, "cannot stop %s", d); return false; }
    if (en) RUN("systemctl", "disable", "--quiet", d);
    snprintf(c->fan_paused, sizeof c->fan_paused, "%s", d);
    c->fan_paused_enabled = en;
    snprintf(note, n, " (%s paused)", d);
    return true;
}
static void fan_daemon_resume(conf_t *c, char *note, size_t n) {
    if (!*c->fan_paused) return;
    if (c->fan_paused_enabled) RUN("systemctl", "enable", "--quiet", c->fan_paused);
    bool ok = RUN("systemctl", "start", c->fan_paused) == 0;
    snprintf(note, n, " (%s %s)", c->fan_paused, ok ? "back on" : "could NOT be started");
    c->fan_paused[0] = 0;
    c->fan_paused_enabled = false;
}

static double cpu_temp(void) { /* hottest coretemp sensor, -1 if none */
    DIR *d = opendir("/sys/class/hwmon");
    if (!d) return -1;
    struct dirent *e;
    char p[300], name[32];
    double best = -1;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "/sys/class/hwmon/%s/name", e->d_name);
        if (!read_text(p, name, sizeof name) || strcmp(name, "coretemp")) continue;
        for (int i = 1; i < 64; i++) {
            snprintf(p, sizeof p, "/sys/class/hwmon/%s/temp%d_input", e->d_name, i);
            long t = read_long(p, -1);
            if (t > 0 && t / 1000.0 > best) best = t / 1000.0;
        }
    }
    closedir(d);
    return best;
}
#else /* macOS */
static int smc_int(const char *key) {
    smcval_t v;
    bool ok;
    if (smc_read(key, &v) != 1) return -1;
    double x = smc_number(&v, &ok);
    return ok ? (int)iround(x) : -1;
}

static bool read_fans(fans_t *F) {
    memset(F, 0, sizeof *F);
    int n = smc_int("FNum");
    if (n <= 0) return false;
    smcval_t v;
    int forced = smc_read("FS! ", &v) == 1 && v.size == 2 ? (v.data[0] << 8 | v.data[1]) : -1;
    char k[8];
    for (int i = 0; i < n && i < MAXFAN; i++) {
        fan_t *f = &F->f[F->n++];
        snprintf(f->label, sizeof f->label, "Fan %d", i + 1);
#define FK(field, suffix) (snprintf(k, sizeof k, "F%d" suffix, i), f->field = smc_int(k))
        FK(cur, "Ac"); FK(min, "Mn"); FK(max, "Mx"); FK(target, "Tg");
#undef FK
        snprintf(k, sizeof k, "F%dMd", i);
        int md = smc_int(k);
        f->manual = md >= 0 ? md == 1 : forced >= 0 ? (forced >> i) & 1 : -1;
    }
    return F->n > 0;
}

/* encode an rpm for a fan key in the key's own format (flt on Apple Silicon, fpe2 on Intel) */
static int enc_rpm(const char *key, int rpm, uint8_t *d) {
    smcval_t v;
    if (smc_read(key, &v) != 1) return 0;
    if (!strncmp(v.type, "flt", 3) && v.size == 4) {
        float x = (float)rpm;
        uint32_t u;
        memcpy(&u, &x, 4);
        for (int i = 0; i < 4; i++) d[i] = (uint8_t)(SMC_LE ? u >> (8 * i) : u >> (24 - 8 * i));
        return 4;
    }
    if (!strncmp(v.type, "fpe2", 4) && v.size == 2) { d[0] = (uint8_t)(rpm >> 6); d[1] = (uint8_t)(rpm << 2); return 2; }
    return 0;
}

/* EXPERIMENTAL (untested on real hardware): Apple Silicon uses F<n>Md (+ Ftst unlock), Intel the FS! bitmask */
static bool fans_set(const fans_t *F, const int *rpm, char *err, size_t en) {
    int lk = lock_smc();
    bool ok = true, any = false;
    char k[8];
    err[0] = 0;
    for (int i = 0; i < F->n; i++) any |= rpm[i] > 0;
    if (smc_has("F0Md")) {
        if (any && smc_has("Ftst")) { wr_t w = {"Ftst", {1}, 1}; ok = smc_write(&w, 1, err, en); }
        for (int i = 0; i < F->n && ok; i++) {
            snprintf(k, sizeof k, "F%dMd", i);
            wr_t w = {k, {rpm[i] > 0 ? 1 : 0}, 1};
            for (int t = 0; t < 30 && !(ok = smc_write(&w, 1, err, en)); t++) usleep(100000); /* thermalmonitord may resist */
            if (ok && rpm[i] > 0) {
                char tg[8];
                snprintf(tg, sizeof tg, "F%dTg", i);
                wr_t x = {tg, {0}, 0};
                x.len = enc_rpm(tg, rpm[i], x.data);
                ok = x.len ? smc_write(&x, 1, err, en) : (snprintf(err, en, "%s: unknown format", tg), false);
            }
        }
        if (ok && !any && smc_has("Ftst")) { wr_t w = {"Ftst", {0}, 1}; ok = smc_write(&w, 1, err, en); }
    } else {
        smcval_t v;
        if (smc_read("FS! ", &v) != 1 || v.size != 2) { snprintf(err, en, "no fan control keys"); unlock_smc(lk); return false; }
        int mask = v.data[0] << 8 | v.data[1];
        for (int i = 0; i < F->n && ok; i++) {
            if (rpm[i] > 0) {
                char tg[8];
                snprintf(tg, sizeof tg, "F%dTg", i);
                wr_t x = {tg, {0}, 0};
                x.len = enc_rpm(tg, rpm[i], x.data);
                ok = x.len ? smc_write(&x, 1, err, en) : (snprintf(err, en, "%s: unknown format", tg), false);
                mask |= 1 << i;
            } else mask &= ~(1 << i);
        }
        if (ok) { wr_t w = {"FS! ", {(uint8_t)(mask >> 8), (uint8_t)mask}, 2}; ok = smc_write(&w, 1, err, en); }
    }
    unlock_smc(lk);
    return ok;
}

static const char *fan_app(void) { /* fan tools that would fight a manual speed */
    static const char *procs[] = {"Macs Fan Control", "TG Pro", "smcFanControl"};
    char out[256];
    for (size_t i = 0; i < sizeof procs / sizeof procs[0]; i++)
        if (RUN_OUT(out, "pgrep", "-x", procs[i]) == 0) return procs[i];
    return NULL;
}

static double cpu_temp(void) { /* Intel Macs: CPU proximity / die sensor; Apple Silicon: not shown */
    smcval_t v;
    bool ok;
    const char *keys[] = {"TC0P", "TC0D", "TC0E"};
    for (size_t i = 0; i < 3; i++)
        if (smc_read(keys[i], &v) == 1) { double t = smc_number(&v, &ok); if (ok && t > 0 && t < 130) return t; }
    return -1;
}
#endif

/* who is driving the fans right now: AUTO (SMC) / AUTO (MBPFAN) / MANUAL 3000 RPM / MANUAL (OTHER TOOL) */
static void fan_mode(const conf_t *c, const fans_t *F, char *o, size_t n) {
    bool manual = false;
    for (int i = 0; i < F->n; i++) manual |= F->f[i].manual == 1;
    const char *other = NULL;
#ifdef OS_LINUX
    other = fan_daemon();
#else
    other = fan_app();
#endif
    char sp[32];
    fan_spec_text(c->fan, sp, sizeof sp);
    if (other) snprintf(o, n, "auto (%s)", other);
    else if (!manual && strcmp(c->fan, "auto")) snprintf(o, n, "auto (SMC) - %s pending", sp);
    else if (!manual) snprintf(o, n, "auto (SMC)");
    else if (strcmp(c->fan, "auto")) snprintf(o, n, "manual %s", sp);
    else snprintf(o, n, "manual (other tool)");
}

/* set fans to "auto" or a manual speed, remember it in the config. Shared by 'macbat fan' and the settings screen. */
static bool fan_apply(const char *spec, char *m, size_t n) {
    fans_t F;
    if (!read_fans(&F)) { snprintf(m, n, "no fans found"); return false; }
    bool manual = strcmp(spec, "auto") != 0;
    int rpm[MAXFAN] = {0};
    for (int i = 0; manual && i < F.n; i++) {
        if (!fan_limits_ok(&F.f[i])) { snprintf(m, n, "fan %d: min/max speed unknown - not changed", i + 1); return false; }
        rpm[i] = fan_want(spec, &F.f[i]);
    }
    conf_t c = load_conf();
    char note[160] = "", err[160];
#ifdef OS_LINUX
    const char *owner = fan_daemon();
    if (!manual && owner && !*c.fan_paused) { /* mbpfan runs the fans: nothing of ours to undo */
        snprintf(c.fan, sizeof c.fan, "auto");
        save_conf(c);
        snprintf(m, n, "fans: automatic (%s)", owner);
        return true;
    }
#endif
#ifdef OS_MAC
    const char *app = fan_app();
    if (app && !g_force) { snprintf(m, n, "%s controls the fans - not changed (--force overrides)", app); return false; }
#else
    if (manual && !fan_daemon_pause(&c, note, sizeof note)) { snprintf(m, n, "%s - fans not changed", note); return false; }
#endif
    snprintf(c.fan, sizeof c.fan, "%s", spec);
    save_conf(c); /* records a paused mbpfan before anything else can fail */
    bool ok = fans_set(&F, rpm, err, sizeof err);
    if (!ok && manual) { /* never leave the fans half-configured: back to automatic */
        int zero[MAXFAN] = {0};
        char e2[160];
        fans_set(&F, zero, e2, sizeof e2);
        snprintf(c.fan, sizeof c.fan, "auto");
    }
#ifdef OS_LINUX
    if (!manual || !ok) fan_daemon_resume(&c, note, sizeof note);
#endif
    save_conf(c);
    if (!ok) { snprintf(m, n, "fan write failed: %s%s", err, note); return false; }
    if (manual) {
        char r[64] = "", t[16];
        for (int i = 0; i < F.n; i++) { snprintf(t, sizeof t, "%s%d", i ? "/" : "", rpm[i]); strcat(r, t); }
        char sp[32];
        fan_spec_text(spec, sp, sizeof sp);
        if (strchr(spec, '%')) snprintf(m, n, "fans: manual %s = %s RPM%s", sp, r, note);
        else snprintf(m, n, "fans: manual %s RPM%s", r, note);
    } else snprintf(m, n, "fans: automatic%s", note);
    return true;
}

/* fans back to the SMC's automatic control when macbat owns them (restore, sleep, service stop, thermal guard) */
static bool fans_release(void) {
    conf_t c = load_conf();
    if (!strcmp(c.fan, "auto")) return false;
#ifdef OS_LINUX
    if (fan_daemon()) return false; /* mbpfan (re)started by someone else: its fans, not ours */
#endif
    fans_t F;
    if (!read_fans(&F)) return false;
    bool any = false;
    for (int i = 0; i < F.n; i++) any |= F.f[i].manual == 1;
    if (!any) return false;
    int zero[MAXFAN] = {0};
    char err[160];
    return fans_set(&F, zero, err, sizeof err);
}

/* daemon: keep the configured manual speed (after boot, resume, SMC resets) */
static bool g_fan_guard = false;
static void fan_enforce(const conf_t *c) {
    static bool warned = false;
    if (!strcmp(c->fan, "auto") || g_fan_guard) return;
#ifdef OS_LINUX
    const char *other = fan_daemon();
#else
    const char *other = fan_app();
#endif
    if (other) {
        if (!warned) logmsg("fans: %s is running - leaving the fans to it (fan=%s not applied)", other, c->fan);
        warned = true;
        return;
    }
    warned = false;
    fans_t F;
    if (!read_fans(&F)) return;
    int rpm[MAXFAN] = {0};
    bool need = false;
    for (int i = 0; i < F.n; i++) {
        if (!fan_limits_ok(&F.f[i])) return;
        rpm[i] = fan_want(c->fan, &F.f[i]);
        if (F.f[i].manual != 1 || abs(F.f[i].target - rpm[i]) > 25) need = true;
    }
    if (!need) return;
    char err[160];
    if (fans_set(&F, rpm, err, sizeof err)) logmsg("fans: manual %d RPM (fan=%s)", rpm[0], c->fan);
    else logmsg("fans: error: %s", err);
}

/* daemon, every few seconds: CPU at FAN_HOT -> automatic until it is back at FAN_COOL */
static void fan_guard(const char *fan) {
    if (!strcmp(fan, "auto")) { g_fan_guard = false; return; }
    double t = cpu_temp();
    if (t < 0) return;
    if (!g_fan_guard && t >= FAN_HOT) {
        g_fan_guard = true;
        fans_release();
        logmsg("fans: CPU %.0f°C - automatic control until it cools to %d°C", t, FAN_COOL);
    } else if (g_fan_guard && t <= FAN_COOL) {
        g_fan_guard = false;
        logmsg("fans: CPU %.0f°C - manual speed again", t);
    }
}

static void fans_json(const fans_t *F) {
    printf("[");
    for (int i = 0; i < F->n; i++) {
        const fan_t *f = &F->f[i];
        printf("%s{\"label\": \"%s\", \"rpm\": %d, \"min\": %d, \"max\": %d, \"target\": %d, \"manual\": %s}", i ? ", " : "",
               f->label, f->cur, f->min, f->max, f->target, f->manual < 0 ? "null" : f->manual ? "true" : "false");
    }
    printf("]");
}

/* ------------------------------------------------------------------ battery */
typedef struct {
    int percent;          /* -1 unknown */
    char state[16];
    int ac;               /* 1/0/-1 */
    double voltage, current, power, temp_c;
    bool has_v, has_i, has_p, has_t;
    int cycles, full_mah, design_mah, tte, ttf, adapter_w;
    double health;        /* -1 unknown */
    bool health_unreliable;
    char maker[32], model[32];
} battery_t;

static void battery_finish(battery_t *b, long now, long full, long design, long rate) {
    b->health = -1;
    b->health_unreliable = false;
    if (full > 0 && design > 0) {
        double h = 100.0 * (double)full / (double)design;
        if (h > 102.0) b->health_unreliable = true; /* gauge says full > design: not a real measurement */
        else b->health = h > 100 ? 100 : h;
    }
    b->tte = b->ttf = -1;
    if (rate > 0 && now >= 0) {
        if (!strcmp(b->state, "discharging")) b->tte = (int)(now * 60 / rate);
        else if (!strcmp(b->state, "charging") && full > 0) b->ttf = (int)((full > now ? full - now : 0) * 60 / rate);
    }
}

#ifdef OS_LINUX
static bool read_battery(battery_t *b) {
    memset(b, 0, sizeof *b);
    b->percent = b->ac = b->cycles = b->full_mah = b->design_mah = b->adapter_w = -1;
    const char *base = "/sys/class/power_supply";
    char bat[300] = "", ac[300] = "", p[400], t[64];
    DIR *d = opendir(base);
    if (!d) return false;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "%s/%s/type", base, e->d_name);
        if (!read_text(p, t, sizeof t)) continue;
        if (!strcmp(t, "Battery") && !*bat) {
            snprintf(p, sizeof p, "%s/%s/present", base, e->d_name);
            if (read_long(p, 1) == 1) snprintf(bat, sizeof bat, "%s/%s", base, e->d_name);
        } else if (!strcmp(t, "Mains") && !*ac) snprintf(ac, sizeof ac, "%s/%s", base, e->d_name);
    }
    closedir(d);
    if (!*bat) return false;
#define F(name) (snprintf(p, sizeof p, "%s/" name, bat), p)
    b->percent = (int)read_long(F("capacity"), -1);
    if (read_text(F("status"), t, sizeof t)) {
        for (char *c = t; *c; c++) *c = (char)tolower((unsigned char)*c);
        snprintf(b->state, sizeof b->state, "%.15s", t);
    }
    if (*ac) { snprintf(p, sizeof p, "%s/online", ac); b->ac = (int)read_long(p, -1); }
    b->cycles = (int)read_long(F("cycle_count"), -1);
    read_text(F("manufacturer"), b->maker, sizeof b->maker);
    read_text(F("model_name"), b->model, sizeof b->model);
    long uv = read_long(F("voltage_now"), -1), ua = read_long(F("current_now"), LONG_MIN);
    if (uv > 0) { b->voltage = uv / 1e6; b->has_v = true; }
    if (ua != LONG_MIN) {
        b->current = ua / 1e6;
        if (!strcmp(b->state, "discharging")) b->current = -dabs(b->current);
        b->has_i = true;
    }
    if (b->has_v && b->has_i) { b->power = b->voltage * b->current; b->has_p = true; }
    long tt = read_long(F("temp"), LONG_MIN);
    if (tt != LONG_MIN) { b->temp_c = tt / 10.0; b->has_t = true; }
    long now, full, design, rate;
    if (exists(F("charge_full"))) {
        now = read_long(F("charge_now"), -1); full = read_long(F("charge_full"), -1); design = read_long(F("charge_full_design"), -1);
        if (full > 0) b->full_mah = (int)(full / 1000);
        if (design > 0) b->design_mah = (int)(design / 1000);
        rate = ua != LONG_MIN ? labs(ua) : 0;
    } else {
        now = read_long(F("energy_now"), -1); full = read_long(F("energy_full"), -1); design = read_long(F("energy_full_design"), -1);
        rate = labs(read_long(F("power_now"), 0));
    }
#undef F
    battery_finish(b, now, full, design, rate);
    return true;
}

static void model_name(char *buf, size_t n) {
    if (!read_text("/sys/class/dmi/id/product_name", buf, n)) snprintf(buf, n, "?");
}
static const char *platform_name(void) { return "Linux · Intel Mac (applesmc)"; }
#endif

#ifdef OS_MAC
static long cf_long(CFDictionaryRef d, const char *k, long dflt) {
    CFStringRef s = CFStringCreateWithCString(NULL, k, kCFStringEncodingUTF8);
    CFTypeRef v = CFDictionaryGetValue(d, s);
    CFRelease(s);
    if (!v) return dflt;
    if (CFGetTypeID(v) == CFBooleanGetTypeID()) return CFBooleanGetValue((CFBooleanRef)v);
    if (CFGetTypeID(v) != CFNumberGetTypeID()) return dflt;
    int64_t x;
    if (!CFNumberGetValue((CFNumberRef)v, kCFNumberSInt64Type, &x)) return dflt;
    return (long)x;
}

static bool read_battery(battery_t *b) {
    memset(b, 0, sizeof *b);
    b->percent = b->ac = b->cycles = b->full_mah = b->design_mah = b->adapter_w = -1;
    io_service_t svc = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("AppleSmartBattery"));
    if (!svc) return false;
    CFMutableDictionaryRef d = NULL;
    if (IORegistryEntryCreateCFProperties(svc, &d, NULL, 0) != KERN_SUCCESS || !d) { IOObjectRelease(svc); return false; }
    long cur = cf_long(d, "CurrentCapacity", -1), max = cf_long(d, "MaxCapacity", -1);
    b->percent = max == 100 ? (int)cur : (cur >= 0 && max > 0 ? (int)iround(100.0 * cur / max) : -1);
    long raw = cf_long(d, "AppleRawMaxCapacity", max > 100 ? max : -1), design = cf_long(d, "DesignCapacity", -1);
    b->full_mah = (int)raw;
    b->design_mah = (int)design;
    b->cycles = (int)cf_long(d, "CycleCount", -1);
    bool ext = cf_long(d, "ExternalConnected", 0), charging = cf_long(d, "IsCharging", 0), full = cf_long(d, "FullyCharged", 0);
    b->ac = ext;
    long mv = cf_long(d, "Voltage", -1);
    long ma = cf_long(d, "InstantAmperage", LONG_MIN);
    if (ma == LONG_MIN) ma = cf_long(d, "Amperage", LONG_MIN);
    snprintf(b->state, sizeof b->state, "%s",
             full ? "full" : charging ? "charging" : ext ? "not charging" : "discharging");
    if (ma != LONG_MIN && ma < -5) snprintf(b->state, sizeof b->state, "discharging");
    if (mv > 0) { b->voltage = mv / 1000.0; b->has_v = true; }
    if (ma != LONG_MIN) { b->current = ma / 1000.0; b->has_i = true; }
    if (b->has_v && b->has_i) { b->power = b->voltage * b->current; b->has_p = true; }
    long t = cf_long(d, "Temperature", LONG_MIN);
    if (t != LONG_MIN && t != 0) { b->temp_c = t / 100.0; b->has_t = true; }
    CFStringRef ak = CFStringCreateWithCString(NULL, "AdapterDetails", kCFStringEncodingUTF8);
    CFTypeRef ad = CFDictionaryGetValue(d, ak);
    CFRelease(ak);
    if (ad && CFGetTypeID(ad) == CFDictionaryGetTypeID()) b->adapter_w = (int)cf_long((CFDictionaryRef)ad, "Watts", -1);
    long tte = cf_long(d, "AvgTimeToEmpty", -1), ttf = cf_long(d, "AvgTimeToFull", -1);
    CFRelease(d);
    IOObjectRelease(svc);
    battery_finish(b, -1, raw, design, 0);
    b->tte = (tte > 0 && tte < 65535 && !strcmp(b->state, "discharging")) ? (int)tte : -1;
    b->ttf = (ttf > 0 && ttf < 65535 && !strcmp(b->state, "charging")) ? (int)ttf : -1;
    return true;
}

static void model_name(char *buf, size_t n) {
    size_t sz = n;
    if (sysctlbyname("hw.model", buf, &sz, NULL, 0) != 0) snprintf(buf, n, "?");
}
static const char *platform_name(void) { return apple_silicon() ? "macOS · Apple Silicon" : "macOS · Intel"; }
#endif

/* ------------------------------------------------------------------ control loop */
static int plan(const conf_t *c, const battery_t *b, const control_t *s, const caps_t *cp, wr_t *w) {
    int n = 0, pct = b->percent, limit = c->topup ? 100 : c->limit; /* top-up: charge to 100% once */
    if (pct < 0) return 0;
    bool floor = pct <= SAFETY_FLOOR;
    if (cp->hw_limit && s->hw_limit != limit) n += w_hw_limit(w + n, limit);
    if (cp->inhibit && !cp->hw_limit) {
        bool blocked = s->charging_blocked == 1, want;
        if (floor || limit >= 100) want = false;
        else if (pct >= limit) want = true;
        else if (pct <= limit - c->hysteresis) want = false;
        else want = blocked; /* inside the hysteresis band */
        if (want != blocked) n += w_charging(w + n, cp, !want);
    }
    if (cp->discharge) {
        bool off = s->adapter_off == 1, want_off;
        if (floor || limit >= 100 || !c->auto_discharge) want_off = false;
        else if (off) want_off = pct > limit;        /* keep discharging down to the limit */
        else want_off = b->ac == 1 && pct >= limit + c->hysteresis; /* band: no toggling at limit / limit+1 */
        if (want_off != off) n += w_adapter(w + n, cp, !want_off);
    }
    return n;
}

static void describe(const wr_t *w, int n, const caps_t *cp, char *out, size_t on) {
    out[0] = 0;
    for (int i = 0; i < n; i++) {
        char part[64] = "";
        if (!strcmp(w[i].key, "BCLM")) snprintf(part, sizeof part, "hardware limit -> %d%%", w[i].data[0]);
        else if (!strcmp(w[i].key, "CHTE") || !strcmp(w[i].key, "CH0B"))
            snprintf(part, sizeof part, "charging %s", w[i].data[0] ? "paused" : "resumed");
        else if (cp->discharge && !strcmp(w[i].key, cp->discharge)) {
            bool off = !strcmp(w[i].key, "ACEN") ? w[i].data[0] == 0 : w[i].data[0] != 0;
            snprintf(part, sizeof part, "%s", off ? "adapter bypassed (discharging)" : "adapter restored");
        }
        if (*part) {
            if (*out) strncat(out, ", ", on - strlen(out) - 1);
            strncat(out, part, on - strlen(out) - 1);
        }
    }
}

static int restore_plan(const caps_t *cp, wr_t *w) {
    control_t s = get_control(cp);
    int n = 0;
    if (cp->inhibit && s.charging_blocked == 1) n += w_charging(w + n, cp, true);
    if (cp->discharge && s.adapter_off == 1) n += w_adapter(w + n, cp, true);
    return n;
}

static bool do_write(const wr_t *w, int n, char *err, size_t errn) {
    if (n == 0) return true;
    int lk = open(LOCK_PATH, O_RDWR | O_CREAT, 0644);
    if (lk >= 0) flock(lk, LOCK_EX);
    bool ok = smc_write(w, n, err, errn);
    if (lk >= 0) { flock(lk, LOCK_UN); close(lk); }
    return ok;
}

static void write_state(const caps_t *cp, const control_t *s) {
    char b[512];
    snprintf(b, sizeof b, "time=%ld\nhw_limit_key=%s\ninhibit_key=%s\ndischarge_key=%s\nhw_limit=%d\ncharging_blocked=%d\nadapter_off=%d\n",
             (long)time(NULL), cp->hw_limit ? cp->hw_limit : "", cp->inhibit ? cp->inhibit : "",
             cp->discharge ? cp->discharge : "", s->hw_limit, s->charging_blocked, s->adapter_off);
    write_text(STATE_PATH, b, 0644);
}

static char g_keybuf[3][5];
static bool read_state(caps_t *cp, control_t *s, long *age) {
    FILE *f = fopen(STATE_PATH, "r");
    if (!f) return false;
    char line[128];
    long t = 0;
    *cp = (caps_t){0};
    *s = (control_t){-1, -1, -1};
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        char *v = strchr(line, '=');
        if (!v) continue;
        *v++ = 0;
        if (!strcmp(line, "time")) t = atol(v);
        else if (!strcmp(line, "hw_limit_key") && *v) { snprintf(g_keybuf[0], 5, "%s", v); cp->hw_limit = g_keybuf[0]; }
        else if (!strcmp(line, "inhibit_key") && *v) { snprintf(g_keybuf[1], 5, "%s", v); cp->inhibit = g_keybuf[1]; }
        else if (!strcmp(line, "discharge_key") && *v) { snprintf(g_keybuf[2], 5, "%s", v); cp->discharge = g_keybuf[2]; }
        else if (!strcmp(line, "hw_limit")) s->hw_limit = atoi(v);
        else if (!strcmp(line, "charging_blocked")) s->charging_blocked = atoi(v);
        else if (!strcmp(line, "adapter_off")) s->adapter_off = atoi(v);
    }
    fclose(f);
    *age = (long)time(NULL) - t;
    return t > 0;
}

static void apply_sys_settings(const conf_t *c);
static volatile sig_atomic_t g_stop = 0, g_kick = 1;
static void on_term(int s) { (void)s; g_stop = 1; }
static void on_hup(int s) { (void)s; g_kick = 1; }

static caps_t g_dcaps;
static void daemon_restore(const char *why) {
    if (fans_release()) logmsg("%s: fans automatic", why);
    wr_t w[4];
    int n = restore_plan(&g_dcaps, w);
    if (!n) return;
    char err[160], d[160];
    describe(w, n, &g_dcaps, d, sizeof d);
    if (do_write(w, n, err, sizeof err)) logmsg("%s: %s", why, d);
    else logmsg("%s: restore FAILED: %s", why, err);
}

#ifdef OS_MAC
static io_connect_t g_pm_root;
static void pm_callback(void *refcon, io_service_t svc, natural_t msg, void *arg) {
    (void)refcon; (void)svc;
    if (msg == kIOMessageSystemWillSleep) daemon_restore("going to sleep");
    else if (msg == kIOMessageSystemHasPoweredOn) g_kick = 1;
    if (msg == kIOMessageCanSystemSleep || msg == kIOMessageSystemWillSleep) IOAllowPowerChange(g_pm_root, (long)arg);
}
#endif

#ifdef OS_LINUX
#  define CTL_PATH "/run/macbat.sock"
#else
#  define CTL_PATH "/var/run/macbat.sock"
static void ctl_cf_cb(CFFileDescriptorRef f, CFOptionFlags fl, void *info);
#endif
static int ctl_open(void);
static void ctl_serve(int lfd);

static int cmd_daemon(void) {
    need_root("daemon");
    refuse_if_conflict("daemon");
    if (!get_caps(&g_dcaps)) die("cannot read the SMC");
    char model[64];
    model_name(model, sizeof model);
    logmsg("macbat %s daemon on %s [%s] limit-key=%s inhibit-key=%s discharge-key=%s", MACBAT_VERSION, model, platform_name(),
           g_dcaps.hw_limit ? g_dcaps.hw_limit : "-", g_dcaps.inhibit ? g_dcaps.inhibit : "-", g_dcaps.discharge ? g_dcaps.discharge : "-");
    if (!g_dcaps.hw_limit && !g_dcaps.inhibit && !g_dcaps.discharge) die("no supported SMC charge-control keys on this machine");
    struct sigaction sa = {0};
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sa.sa_handler = on_hup;
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN); /* a control client may go away before the reply */
    int lfd = ctl_open();
    bool can_sleep_hook = true;
#ifdef OS_MAC
    IONotificationPortRef port = NULL;
    io_object_t notifier;
    g_pm_root = IORegisterForSystemPower(NULL, &port, pm_callback, &notifier);
    if (g_pm_root) CFRunLoopAddSource(CFRunLoopGetCurrent(), IONotificationPortGetRunLoopSource(port), kCFRunLoopDefaultMode);
    else { can_sleep_hook = false; logmsg("warning: no sleep notifications; auto-discharge disabled"); }
    if (lfd >= 0) {
        CFFileDescriptorRef cff = CFFileDescriptorCreate(NULL, lfd, false, ctl_cf_cb, NULL);
        CFFileDescriptorEnableCallBacks(cff, kCFFileDescriptorReadCallBack);
        CFRunLoopSourceRef src = CFFileDescriptorCreateRunLoopSource(NULL, cff, 0);
        CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopDefaultMode);
    }
#endif
    time_t last_mtime = -1;
    time_t next = 0;
    char dfan[16] = "auto";
    while (!g_stop) {
        fan_guard(dfan);
        time_t mt = conf_mtime(), now = time(NULL);
        if (g_kick || mt != last_mtime || now >= next) {
            g_kick = 0;
            last_mtime = mt;
            conf_t c = load_conf();
            apply_sys_settings(&c);
            snprintf(dfan, sizeof dfan, "%s", c.fan);
            fan_enforce(&c);
            if (!can_sleep_hook) c.auto_discharge = false;
            battery_t b;
            if (read_battery(&b)) {
                /* "full" alone is not enough: the gauge also says full while the SMC holds the battery at the limit */
                long tu = time(NULL) - c.topup_since;
                bool done = b.percent >= 100 || (b.percent >= 97 && !strcmp(b.state, "full") && tu > 600);
                if (c.topup && (done || tu > 12 * 3600)) {
                    logmsg("top-up %s - back to the %d%% limit", done ? "done" : "expired", c.limit);
                    c.topup = false;
                    save_conf(c);
                }
                control_t s = get_control(&g_dcaps);
                wr_t w[8];
                int n = plan(&c, &b, &s, &g_dcaps, w);
                if (n) {
                    char err[160], d[200];
                    describe(w, n, &g_dcaps, d, sizeof d);
                    if (do_write(w, n, err, sizeof err))
                        logmsg("battery %d%% ac=%s limit=%d%%: %s", b.percent, b.ac == 1 ? "yes" : "no", c.limit, d);
                    else logmsg("error: %s", err);
                    s = get_control(&g_dcaps);
                }
                write_state(&g_dcaps, &s);
            } else logmsg("error: cannot read battery");
            next = time(NULL) + c.interval;
        }
#ifdef OS_MAC
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 2.0, false); /* the control socket stops it early */
#else
        fd_set rf;
        FD_ZERO(&rf);
        if (lfd >= 0) FD_SET(lfd, &rf);
        struct timeval tv = {2, 0};
        if (select(lfd >= 0 ? lfd + 1 : 0, &rf, NULL, NULL, &tv) > 0 && lfd >= 0 && FD_ISSET(lfd, &rf)) ctl_serve(lfd);
#endif
    }
    if (lfd >= 0) { close(lfd); unlink(CTL_PATH); }
    daemon_restore("stopping");
    logmsg("stopped");
    return 0;
}

/* ------------------------------------------------------------------ status */
static const char *service_state(void) {
#ifdef OS_LINUX
    static char out[64];
    RUN_OUT(out, "systemctl", "is-active", "macbat");
    out[strcspn(out, "\n")] = 0;
    return *out ? out : "unknown";
#else
    static char out[16384];
    if (RUN_OUT(out, "launchctl", "print", "system/" LAUNCHD_LABEL) != 0)
        return exists(LAUNCHD_PLIST) ? "stopped" : "not installed";
    return strstr(out, "state = running") ? "active" : "loaded";
#endif
}

static bool svc_active(void) { return !strcmp(service_state(), "active"); }

static void print_bar(int pct, int limit, bool utf) {
    const int W = 24;
    int n = (int)iround((pct < 0 ? 0 : pct) / 100.0 * W), m = limit < 100 ? (int)iround(limit / 100.0 * W) : -1;
    if (m >= W) m = W - 1;
    for (int i = 0; i < W; i++) fputs(i == m ? (utf ? "│" : "|") : i < n ? (utf ? "█" : "#") : (utf ? "░" : "."), stdout);
}

static void fmt_min(int m, char *b, size_t n) { if (m < 0) snprintf(b, n, "-"); else snprintf(b, n, "%dh%02dm", m / 60, m % 60); }

static const char *mode_text(const conf_t *c, const battery_t *b, const caps_t *cp, const control_t *s, char *buf, size_t n) {
    if (s && s->adapter_off == 1) { snprintf(buf, n, "discharging to %d%% (adapter bypassed)", c->limit); return buf; }
    if (c->topup && b->percent < 100) return "topping up to 100% (once)";
    if (s && s->charging_blocked == 1) return "holding at limit (charging paused)";
    if (b->ac == 1 && cp && cp->hw_limit && s && s->hw_limit > 0 && b->percent >= s->hw_limit && strcmp(b->state, "charging"))
        return "holding at limit (hardware)";
    if (!strcmp(b->state, "discharging") && b->ac != 1) return "on battery";
    return b->state;
}

static void json_str(const char *k, const char *v, bool comma) {
    printf("  \"%s\": ", k);
    if (!v) printf("null"); else printf("\"%s\"", v);
    printf("%s\n", comma ? "," : "");
}


/* ------------------------------------------------------------------ themes */
/* Glyphs: only what the Linux console Terminus font has (box drawing incl. ╭ ━ ┃ ┈, █ ░, ▲ ▼ ● ○ ◆ ■ · › ‹ « » ↑).
 * Colours degrade: truecolor (COLORTERM=truecolor) -> 256 (TERM=*256*) -> 16 (TERM=linux, anything else) -> none. */
#define RW 62 /* inner width of the frame */

typedef struct { const char *attr; int rgb; const char *c16; } ink_t; /* rgb -1: use c16 (NULL = default colour) */
enum { ST_RETRO, ST_ROUND, ST_SQUARE };
typedef struct {
    const char *name, *desc;
    int style;
    const char *fill, *empty, *mark; /* slim bar glyphs (not retro) */
    int grad;                        /* truecolor bar gradient: hi colour -> grad, -1 = none */
    ink_t frame, title, label, value, dim, hi, warn, ok, bad;
} theme_def_t;

#define NONE {"", -1, NULL}
static const theme_def_t THEMES[] = {
    {"modern", "clean, rounded, one blue accent - reads on dark and light terminals", ST_ROUND, "━", "─", "┃", -1,
     {"", 0x6e7681, "90"}, {"", 0x3b8eea, "36"}, {"", 0x7d8590, "90"}, NONE, {"", 0x6e7681, "90"}, {"", 0x3b8eea, "36"},
     {"", 0xd29922, "33"}, {"", 0x2da44e, "32"}, {"", 0xe5534b, "91"}},
    {"mono", "no colour at all - any terminal, same as NO_COLOR", ST_SQUARE, "━", "┈", "┃", -1,
     NONE, {"1", -1, NULL}, {"2", -1, NULL}, NONE, {"2", -1, NULL}, {"1", -1, NULL}, {"1", -1, NULL}, NONE, {"1", -1, NULL}},
    {"ocean", "deep blue frame, aqua accents, gradient gauge", ST_ROUND, "━", "─", "┃", 0x4fd6f7,
     {"", 0x2f6fd8, "94"}, {"", 0x4fd6f7, "96"}, {"", 0x5aa9d6, "36"}, NONE, {"", 0x5f7f9f, "90"}, {"", 0x2f8fe8, "96"},
     {"", 0xffb454, "93"}, {"", 0x2ec4b6, "36"}, {"", 0xff5f7e, "91"}},
    {"candy", "pastel pink and lavender, dotted gauge", ST_ROUND, "●", "·", "◆", 0xb89cff,
     {"", 0xc39bf5, "95"}, {"", 0xff7ac6, "95"}, {"", 0xb48ef0, "35"}, NONE, {"", 0x9a86b5, "90"}, {"", 0xff7ac6, "95"},
     {"", 0xffc66d, "93"}, {"", 0x7ee0b5, "92"}, {"", 0xff6e8a, "91"}},
    {"nord", "arctic blue-grey (Nord palette), square frame", ST_SQUARE, "■", "·", "┃", -1,
     {"", 0x4c566a, "90"}, {"", 0x88c0d0, "96"}, {"", 0x81a1c1, "94"}, NONE, {"", 0x616e88, "90"}, {"", 0x88c0d0, "96"},
     {"", 0xebcb8b, "33"}, {"", 0xa3be8c, "32"}, {"", 0xbf616a, "91"}},
    {"retro", "green CRT: double frame, block digits (the 0.4 look)", ST_RETRO, "█", "░", "│", -1,
     {"", -1, "32"}, {"1", -1, "92"}, {"", -1, "32"}, {"", -1, "92"}, {"2", -1, "32"}, {"1", -1, "93"},
     {"1", -1, "91"}, {"1", -1, "92"}, {"1", -1, "91"}},
    {"amber", "amber CRT, same layout as retro", ST_RETRO, "█", "░", "│", -1,
     {"", -1, "33"}, {"1", -1, "93"}, {"", -1, "33"}, {"", -1, "93"}, {"2", -1, "33"}, {"1", -1, "97"},
     {"1", -1, "91"}, {"1", -1, "93"}, {"1", -1, "91"}},
};
#undef NONE
#define NTHEMES ((int)(sizeof THEMES / sizeof THEMES[0]))

typedef struct {
    char frame[40], title[40], label[40], value[40], dim[40], hi[40], warn[40], ok[40], bad[40];
    const char *reset;
    const theme_def_t *d;
    bool retro;
    int depth; /* 0 / 16 / 256 / 24 */
    const char *tl, *tr, *bl, *br, *h, *v, *jl, *jr, *jl2, *jr2, *h2;
} theme_t;

static const theme_def_t *g_th = NULL;
static char g_th_src[64] = "default";
static int g_depth = -1;

static const theme_def_t *find_theme(const char *n) {
    if (!n) return NULL;
    if (!strcasecmp(n, "green")) n = "retro"; /* MACBAT_THEME=green from 0.3/0.4 */
    for (int i = 0; i < NTHEMES; i++) if (!strcasecmp(THEMES[i].name, n)) return &THEMES[i];
    return NULL;
}
static void theme_names(char *o, size_t n) {
    o[0] = 0;
    for (int i = 0; i < NTHEMES; i++) { if (i) strncat(o, ", ", n - strlen(o) - 1); strncat(o, THEMES[i].name, n - strlen(o) - 1); }
}
static void resolve_theme(const char *flag) {
    char names[128];
    theme_names(names, sizeof names);
    const theme_def_t *t = NULL;
    const char *e = getenv("MACBAT_THEME");
    if (flag) {
        if (!(t = find_theme(flag))) die("unknown theme '%s' - themes: %s (preview: macbat theme --list)", flag, names);
        snprintf(g_th_src, sizeof g_th_src, "--theme");
    } else if (e && *e) {
        if ((t = find_theme(e))) snprintf(g_th_src, sizeof g_th_src, "MACBAT_THEME");
        else fprintf(stderr, "macbat: unknown theme '%s' in MACBAT_THEME - themes: %s\n", e, names);
    }
    if (!t) {
        conf_t c = load_conf();
        if (*c.theme) {
            if ((t = find_theme(c.theme))) snprintf(g_th_src, sizeof g_th_src, "%s", CONF_PATH);
            else fprintf(stderr, "macbat: unknown theme '%s' in %s - themes: %s\n", c.theme, CONF_PATH, names);
        }
    }
    if (!t) { t = &THEMES[0]; snprintf(g_th_src, sizeof g_th_src, "default"); }
    g_th = t;
}

static int color_depth(void) {
    const char *nc = getenv("NO_COLOR"), *mc = getenv("MACBAT_COLORS"), *t = getenv("TERM"), *ct = getenv("COLORTERM");
    if (nc && *nc) return 0;
    if (mc && *mc) {
        if (!strcmp(mc, "none") || !strcmp(mc, "0")) return 0;
        if (!strcmp(mc, "16")) return 16;
        if (!strcmp(mc, "256")) return 256;
        if (!strcasecmp(mc, "truecolor") || !strcasecmp(mc, "24bit")) return 24;
    }
    if (t && (!strcmp(t, "linux") || !strncmp(t, "vt", 2))) return 16; /* Linux console / fbterm: 16 colours */
    if (ct && (strstr(ct, "truecolor") || strstr(ct, "24bit"))) return 24;
    if (t && strstr(t, "256")) return 256;
    return 16;
}

static int rgb256(int rgb) { /* nearest xterm-256 colour (6x6x6 cube or grey ramp) */
    int r = rgb >> 16 & 255, g = rgb >> 8 & 255, b = rgb & 255;
    static const int lv[6] = {0, 95, 135, 175, 215, 255};
    int ci[3], cv[3], src[3] = {r, g, b};
    for (int k = 0; k < 3; k++) {
        int best = 0;
        for (int i = 1; i < 6; i++) if (abs(lv[i] - src[k]) < abs(lv[best] - src[k])) best = i;
        ci[k] = best;
        cv[k] = lv[best];
    }
    int avg = (r + g + b) / 3, gi = clampi((avg - 8 + 5) / 10, 0, 23), gv = 8 + 10 * gi;
    long dc = (long)(cv[0] - r) * (cv[0] - r) + (long)(cv[1] - g) * (cv[1] - g) + (long)(cv[2] - b) * (cv[2] - b);
    long dg = (long)(gv - r) * (gv - r) + (long)(gv - g) * (gv - g) + (long)(gv - b) * (gv - b);
    return dg < dc ? 232 + gi : 16 + 36 * ci[0] + 6 * ci[1] + ci[2];
}

static void rgb_esc(char *o, size_t n, int rgb, int depth, const char *attr) {
    char col[32] = "";
    if (depth == 24) snprintf(col, sizeof col, ";38;2;%d;%d;%d", rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255);
    else if (depth == 256) snprintf(col, sizeof col, ";38;5;%d", rgb256(rgb));
    snprintf(o, n, "\033[0%s%s%sm", *attr ? ";" : "", attr, col);
}

static void ink(char *o, size_t n, const ink_t *k, int depth) {
    if (k->rgb >= 0 && (depth == 24 || depth == 256)) { rgb_esc(o, n, k->rgb, depth, k->attr); return; }
    char col[16] = "";
    if (depth > 0 && k->c16) snprintf(col, sizeof col, ";%s", k->c16);
    snprintf(o, n, "\033[0%s%s%sm", *k->attr ? ";" : "", k->attr, col);
}

static theme_t theme(void) {
    if (!g_th) resolve_theme(NULL);
    if (g_depth < 0) g_depth = color_depth();
    theme_t T;
    memset(&T, 0, sizeof T);
    T.d = g_th;
    T.retro = g_th->style == ST_RETRO;
    T.depth = g_depth;
#define INK(role) ink(T.role, sizeof T.role, &g_th->role, T.depth)
    INK(frame); INK(title); INK(label); INK(value); INK(dim); INK(hi); INK(warn); INK(ok); INK(bad);
#undef INK
    T.reset = "\033[0m";
    if (T.retro) {
        T.tl = "╔"; T.tr = "╗"; T.bl = "╚"; T.br = "╝"; T.h = "═"; T.v = "║"; T.jl = "╠"; T.jr = "╣";
        T.jl2 = "╟"; T.jr2 = "╢"; T.h2 = "─";
    } else {
        bool round = g_th->style == ST_ROUND;
        T.tl = round ? "╭" : "┌"; T.tr = round ? "╮" : "┐"; T.bl = round ? "╰" : "└"; T.br = round ? "╯" : "┘";
        T.h = T.h2 = "─"; T.v = "│"; T.jl = T.jl2 = "├"; T.jr = T.jr2 = "┤";
    }
    return T;
}

static bool use_boxed(void) {
    if (g_plain || !isatty(1) || !utf8_out()) return false;
    const char *t = getenv("TERM");
    if (t && !strcmp(t, "dumb")) return false;
    struct winsize w;
    int cols = (ioctl(1, TIOCGWINSZ, &w) == 0 && w.ws_col) ? w.ws_col : 80;
    return cols >= RW + 2;
}

static int vis_width(const char *s) {
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        if (*p == 0x1b) { while (*p && *p != 'm') p++; if (*p) p++; continue; }
        if ((*p & 0xC0) != 0x80) w++;
        p++;
    }
    return w;
}

/* cut s so that its visible width is <= maxw (keeps escape sequences intact) */
static void vis_trunc(char *s, int maxw) {
    int w = 0;
    for (unsigned char *p = (unsigned char *)s; *p;) {
        if (*p == 0x1b) { while (*p && *p != 'm') p++; if (*p) p++; continue; }
        if ((*p & 0xC0) != 0x80) {
            if (w == maxw - 1 && vis_width((char *)p) > 1) { strcpy((char *)p, "…\033[0m"); return; }
            w++;
        }
        p++;
    }
}

/* ASCII only: with a UTF-8 locale macOS' toupper()/tolower() treat UTF-8 bytes as Latin-1 letters */
static bool a_alnum(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }
static char a_up(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }
static char a_low(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static void upcase(char *s) { for (; *s; s++) *s = a_up(*s); }

static void r_rule(const theme_t *T, const char *l, const char *fill, const char *r) {
    printf("%s%s", T->frame, l);
    for (int i = 0; i < RW; i++) fputs(fill, stdout);
    printf("%s%s\n", r, T->reset);
}
static void r_top(const theme_t *T) { r_rule(T, T->tl, T->h, T->tr); }
static void r_sep(const theme_t *T) { r_rule(T, T->jl, T->h, T->jr); }
static void r_sep2(const theme_t *T) { r_rule(T, T->jl2, T->h2, T->jr2); }
static void r_bot(const theme_t *T) { r_rule(T, T->bl, T->h, T->br); }

static void r_line(const theme_t *T, const char *content) {
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", content);
    vis_trunc(buf, RW - 2);
    int w = vis_width(buf);
    printf("%s%s%s %s", T->frame, T->v, T->reset, buf);
    for (int i = w; i < RW - 1; i++) putchar(' ');
    printf("%s%s%s\n", T->frame, T->v, T->reset);
}

static void r_split(const theme_t *T, const char *left, const char *right) {
    char buf[4096];
    int pad = RW - 2 - vis_width(left) - vis_width(right);
    snprintf(buf, sizeof buf, "%s%*s%s", left, pad > 1 ? pad : 1, "", right);
    r_line(T, buf);
}

/* top rule with a caption: ╭─ caption ───────╮ */
static void r_top_title(const theme_t *T, const char *caption) {
    printf("%s%s%s %s%s%s ", T->frame, T->tl, T->h, T->title, caption, T->frame);
    for (int i = vis_width(caption) + 3; i < RW; i++) fputs(T->h, stdout);
    printf("%s%s\n", T->tr, T->reset);
}

/* "LABEL ···· value" (retro) or "Label      value" (other themes) */
static void r_field(const theme_t *T, const char *label, const char *value) {
    char buf[4096], lead[64];
    if (T->retro) {
        int n = 9 - (int)strlen(label);
        lead[0] = 0;
        for (int i = 0; i < n && i < 20; i++) strcat(lead, "·");
        snprintf(buf, sizeof buf, " %s%s %s%s%s %s%s", T->label, label, T->dim, lead, T->reset, value, T->reset);
    } else snprintf(buf, sizeof buf, " %s%-10s%s%s%s", T->label, label, T->value, value, T->reset);
    r_line(T, buf);
}

/* slim gauge for the non-retro themes: w cells, frac filled, marker cell (or -1);
 * colour: lvl, or the theme's gradient on truecolor terminals when gradient is allowed */
static void m_bar(const theme_t *T, double frac, int mark, int w, const char *lvl, bool gradient, char *o, size_t on) {
    int fill = clampi((int)iround(frac * w), 0, w);
    bool grad = gradient && T->depth == 24 && T->d->grad >= 0 && T->d->hi.rgb >= 0;
    o[0] = 0;
    for (int i = 0; i < w; i++) {
        char col[48];
        const char *g;
        if (i == mark) { snprintf(col, sizeof col, "%s", T->value); g = T->d->mark; }
        else if (i < fill) {
            if (grad) {
                int a = T->d->hi.rgb, b = T->d->grad;
                double f = w > 1 ? (double)i / (w - 1) : 0;
                int r = (int)iround((a >> 16 & 255) + ((b >> 16 & 255) - (a >> 16 & 255)) * f);
                int gg = (int)iround((a >> 8 & 255) + ((b >> 8 & 255) - (a >> 8 & 255)) * f);
                int bb = (int)iround((a & 255) + ((b & 255) - (a & 255)) * f);
                rgb_esc(col, sizeof col, r << 16 | gg << 8 | bb, 24, "");
            } else snprintf(col, sizeof col, "%s", lvl);
            g = T->d->fill;
        } else { snprintf(col, sizeof col, "%s", T->dim); g = T->d->empty; }
        strncat(o, col, on - strlen(o) - 1);
        strncat(o, g, on - strlen(o) - 1);
    }
    strncat(o, T->reset, on - strlen(o) - 1);
}

/* "↑ limit 80%" under the marker of a gauge that starts after `indent` columns */
static void m_caption(const theme_t *T, int indent, int mark, const char *text, const char *right) {
    char l[512], t[160];
    snprintf(t, sizeof t, "↑ %s", text);
    int tw = vis_width(t), pos = indent + mark;
    if (pos + tw > RW - 2) { snprintf(t, sizeof t, "%s ↑", text); pos = indent + mark - tw + 1; }
    if (mark < 0) { t[0] = 0; pos = 0; }
    snprintf(l, sizeof l, "%*s%s%s%s", pos > 0 ? pos : 0, "", T->dim, t, T->reset);
    if (right) {
        int pad = RW - 2 - vis_width(l) - vis_width(right);
        if (pad >= 2) { size_t k = strlen(l); snprintf(l + k, sizeof l - k, "%*s%s%s%s", pad, "", T->dim, right, T->reset); }
    }
    r_line(T, l);
}

/* "MEDIUM + DIPM" -> "medium + DIPM": calmer value text for the non-retro themes */
static void soft_case(char *s) {
    static const char *keep[] = {"DIPM", "ALPM", "SMC", "AC", "CPU", "USB", "PCIE", "BCLM", "ACEN", "CHTE", "CHIE", "SATA"};
    for (char *p = s; *p;) {
        if (!a_alnum(*p)) { p++; continue; }
        char *e = p;
        while (a_alnum(*e)) e++;
        bool k = false;
        for (size_t i = 0; i < sizeof keep / sizeof keep[0]; i++)
            if ((size_t)(e - p) == strlen(keep[i]) && !strncasecmp(p, keep[i], (size_t)(e - p))) k = true;
        for (char *q = p; q < e; q++) *q = k ? a_up(*q) : a_low(*q);
        p = e;
    }
}
static void title_case(char *s) { soft_case(s); if (*s) *s = a_up(*s); }

/* 3x5 block digits */
static const char *BIG[12][5] = {
    {"###", "#.#", "#.#", "#.#", "###"}, {".#.", "##.", ".#.", ".#.", "###"}, {"###", "..#", "###", "#..", "###"},
    {"###", "..#", "###", "..#", "###"}, {"#.#", "#.#", "###", "..#", "..#"}, {"###", "#..", "###", "..#", "###"},
    {"###", "#..", "###", "#.#", "###"}, {"###", "..#", "..#", "..#", "..#"}, {"###", "#.#", "###", "#.#", "###"},
    {"###", "#.#", "###", "..#", "###"}, {"#.#", "..#", ".#.", "#..", "#.#"}, {"###", "..#", ".##", "...", ".#."},
};

static void big_row(const char *txt, int row, char *out, size_t on) {
    out[0] = 0;
    for (const char *c = txt; *c; c++) {
        int i = isdigit((unsigned char)*c) ? *c - '0' : *c == '%' ? 10 : 11;
        for (int x = 0; x < 3; x++) strncat(out, BIG[i][row][x] == '#' ? "█" : " ", on - strlen(out) - 1);
        if (c[1]) strncat(out, " ", on - strlen(out) - 1);
    }
}

static void status_retro(const conf_t *c, const battery_t *b, const caps_t *cp, const control_t *s, bool have,
                         const char *source, const char *model, const char *conf_app, const char *mode) {
    theme_t T = theme();
    char l[4096], r[256], v[1024];
    const int GW = 26;
    int pct = b->percent < 0 ? 0 : b->percent > 100 ? 100 : b->percent;
    int fill = (int)iround(pct / 100.0 * GW), mark = c->limit < 100 ? (int)iround(c->limit / 100.0 * GW) : -1;
    if (mark >= GW) mark = GW - 1;
    const char *lvl = pct >= 50 ? T.ok : pct >= 20 ? T.hi : T.bad;

    r_top(&T);
    snprintf(l, sizeof l, "%s█▒░ M A C B A T ░▒█%s", T.title, T.reset);
    snprintf(r, sizeof r, "%sv%s · %s%s", T.dim, MACBAT_VERSION, model, T.reset);
    r_split(&T, l, r);
    char pn[64], ts[16];
    snprintf(pn, sizeof pn, "%s", platform_name());
    upcase(pn);
    time_t now = time(NULL);
    strftime(ts, sizeof ts, "%H:%M:%S", localtime(&now));
    snprintf(l, sizeof l, "%s%s%s", T.label, pn, T.reset);
    snprintf(r, sizeof r, "%s%s%s", T.dim, ts, T.reset);
    r_split(&T, l, r);
    r_sep(&T);

    /* battery gauge (3 rows) next to 5-row block digits */
    char pcts[16];
    if (b->percent >= 0) snprintf(pcts, sizeof pcts, "%d", b->percent); else snprintf(pcts, sizeof pcts, "?");
    for (int row = 0; row < 5; row++) {
        char g[2048] = "", dg[256];
        if (row >= 1 && row <= 3) {
            strcat(g, T.frame);
            strcat(g, row == 1 ? "┌" : row == 2 ? "│" : "└");
            for (int i = 0; i < GW; i++) {
                if (row == 2) {
                    if (i < fill) { strcat(g, (mark >= 0 && i >= mark) ? T.hi : lvl); strcat(g, "█"); }
                    else { strcat(g, T.dim); strcat(g, "░"); }
                    strcat(g, T.frame);
                } else strcat(g, i == mark ? (row == 1 ? "┬" : "┴") : "─");
            }
            strcat(g, row == 1 ? "┐ " : row == 2 ? "│█" : "┘ ");
            strcat(g, T.reset);
        } else {
            for (int i = 0; i < GW + 3; i++) strcat(g, " ");
        }
        big_row(pcts, row, dg, sizeof dg);
        snprintf(l, sizeof l, "  %s     %s%s%s%s", g, lvl, dg, row == 4 ? " %" : "", T.reset);
        r_line(&T, l);
    }
    char md[128];
    snprintf(md, sizeof md, "%s", mode);
    upcase(md);
    const char *arrow = !strcmp(b->state, "charging") ? " ▲" : (!strcmp(b->state, "discharging") ? " ▼" : "");
    if (have && s->adapter_off == 1) arrow = " ▼";
    snprintf(l, sizeof l, "  %s▸ %s%s%s", T.hi, md, arrow, T.reset);
    r_line(&T, l);
    if (mark >= 0) {
        char tick[128] = "";
        for (int i = 0; i < mark + 3; i++) strcat(tick, " ");
        snprintf(l, sizeof l, "%s%s▲ LIMIT %d%%%s", tick, T.dim, c->limit, T.reset);
        r_line(&T, l);
    }
    r_sep2(&T);

    /* fields */
    if (have && s->adapter_off == 1) snprintf(v, sizeof v, "%sAC BYPASSED · RUNNING ON BATTERY", T.hi);
    else {
        snprintf(v, sizeof v, "%s%s", T.value, b->ac == 1 ? "AC CONNECTED" : "ON BATTERY");
        if (b->adapter_w > 0) { char t[32]; snprintf(t, sizeof t, " · %d W ADAPTER", b->adapter_w); strcat(v, t); }
    }
    r_field(&T, "POWER", v);
    if (have) {
        char how[48];
        if (cp->hw_limit) snprintf(how, sizeof how, "HARDWARE (BCLM)");
        else if (cp->inhibit) snprintf(how, sizeof how, "SOFTWARE (%s)", cp->inhibit);
        else snprintf(how, sizeof how, "NOT SUPPORTED");
        snprintf(v, sizeof v, "%s%d%% · %s", T.value, c->limit, how);
        if (cp->discharge) {
            char t[64];
            snprintf(t, sizeof t, " · AUTO-DISCHARGE %s", c->auto_discharge ? "ON" : "OFF");
            strcat(v, t);
        }
    } else snprintf(v, sizeof v, "%s%d%% · %sCONTROL STATE NEEDS SUDO / SERVICE", T.value, c->limit, T.dim);
    r_field(&T, "LIMIT", v);
    if (b->health_unreliable)
        snprintf(v, sizeof v, "%sN/A %s(UNCALIBRATED %d>%d mAh)", T.hi, T.dim, b->full_mah, b->design_mah);
    else if (b->health >= 0) {
        snprintf(v, sizeof v, "%s%.1f%%", T.value, b->health);
        if (b->full_mah > 0 && b->design_mah > 0) {
            char t[64];
            snprintf(t, sizeof t, " %s· %d/%d mAh", T.dim, b->full_mah, b->design_mah);
            strcat(v, t);
        }
    } else snprintf(v, sizeof v, "%s?", T.dim);
    if (b->cycles >= 0) { char t[48]; snprintf(t, sizeof t, "%s · %d CYCLES", T.value, b->cycles); strcat(v, t); }
    r_field(&T, "HEALTH", v);
    if (*b->maker || *b->model) {
        snprintf(v, sizeof v, "%s%s %s", T.value, b->maker, b->model);
        r_field(&T, "PACK", v);
    }
    v[0] = 0;
    strcat(v, T.value);
    char t[64];
    bool first = true;
#define ADD(...) do { snprintf(t, sizeof t, __VA_ARGS__); if (!first) strcat(v, " · "); strcat(v, t); first = false; } while (0)
    if (b->has_v) ADD("%.2f V", b->voltage);
    if (b->has_i) ADD("%+.2f A", b->current);
    if (b->has_p) ADD("%+.1f W", b->power);
    if (b->has_t) ADD("%.1f°C", b->temp_c);
#undef ADD
    r_field(&T, "CELL", first ? "?" : v);
    if (b->tte >= 0 || b->ttf >= 0) {
        char m[16];
        fmt_min(b->tte >= 0 ? b->tte : b->ttf, m, sizeof m);
        snprintf(v, sizeof v, "%s%s IN %s", T.value, b->tte >= 0 ? "EMPTY" : "FULL", m);
        r_field(&T, "TIME", v);
    }
    fans_t F;
    if (read_fans(&F)) {
        char fm[64], ft[48];
        fan_mode(c, &F, fm, sizeof fm);
        upcase(fm);
        snprintf(v, sizeof v, "%s", T.value);
        for (int i = 0; i < F.n; i++) { snprintf(ft, sizeof ft, "%s%d", i ? " · " : "", F.f[i].cur); strcat(v, ft); }
        snprintf(ft, sizeof ft, " RPM %s· %s", T.dim, fm);
        strcat(v, ft);
        r_field(&T, F.n > 1 ? "FANS" : "FAN", v);
    }
    const char *svc = service_state();
    char su[32];
    snprintf(su, sizeof su, "%s", svc);
    upcase(su);
    snprintf(v, sizeof v, "%s● %s%s", !strcmp(svc, "active") ? T.ok : T.bad, su, T.reset);
    if (strcmp(source, "live")) { char x[128]; snprintf(x, sizeof x, " %s· %s", T.dim, source); strcat(v, x); }
    r_field(&T, "SERVICE", v);
    if (conf_app) {
        char cu[32];
        snprintf(cu, sizeof cu, "%s", conf_app);
        upcase(cu);
        snprintf(l, sizeof l, " %s!! %s MANAGES CHARGING - MACBAT STAYS HANDS-OFF%s", T.warn, cu, T.reset);
        r_line(&T, l);
    }
    r_bot(&T);
}

/* ------------------------------------------------------------------ status page, non-retro themes */
static void hdr_modern(const theme_t *T, const char *what, const char *model) {
    char l[512], r[256], ts[16];
    r_top(T);
    snprintf(l, sizeof l, "%smacbat%s %s%s%s", T->title, T->reset, T->dim, what, T->reset);
    snprintf(r, sizeof r, "%sv%s · %s%s", T->dim, MACBAT_VERSION, model, T->reset);
    r_split(T, l, r);
    time_t now = time(NULL);
    strftime(ts, sizeof ts, "%H:%M:%S", localtime(&now));
    snprintf(l, sizeof l, "%s%s%s", T->dim, platform_name(), T->reset);
    snprintf(r, sizeof r, "%s%s%s", T->dim, ts, T->reset);
    r_split(T, l, r);
    r_sep(T);
}

static const char *lvl_ink(const theme_t *T, int pct) {
    if (T->retro) return pct >= 50 ? T->ok : pct >= 20 ? T->hi : T->bad;
    return pct >= 50 ? T->ok : pct >= 20 ? T->warn : T->bad;
}

static void status_modern(const conf_t *c, const battery_t *b, const caps_t *cp, const control_t *s, bool have,
                          const char *source, const char *model, const char *conf_app, const char *mode) {
    theme_t T = theme();
    char l[4096], v[1024], t[160];
    const int BW = RW - 4;
    int pct = b->percent < 0 ? 0 : b->percent > 100 ? 100 : b->percent;
    int mark = c->limit < 100 ? clampi((int)iround(c->limit / 100.0 * BW), 0, BW - 1) : -1;
    const char *lvl = lvl_ink(&T, pct);
    hdr_modern(&T, "battery", model);
    r_line(&T, "");
    char md[128];
    snprintf(md, sizeof md, "%s", mode);
    if (*md) md[0] = a_up(md[0]);
    const char *arrow = !strcmp(b->state, "charging") ? " ▲" : (!strcmp(b->state, "discharging") ? " ▼" : "");
    if (have && s->adapter_off == 1) arrow = " ▼";
    if (b->percent >= 0) snprintf(t, sizeof t, "%d%%", b->percent); else snprintf(t, sizeof t, "?%%");
    snprintf(l, sizeof l, " %s%s%s   %s%s%s%s", lvl, t, T.reset, T.hi, md, arrow, T.reset);
    r_line(&T, l);
    char bar[3072];
    m_bar(&T, pct / 100.0, mark, BW, lvl, pct >= 20, bar, sizeof bar);
    snprintf(l, sizeof l, " %s", bar);
    r_line(&T, l);
    snprintf(t, sizeof t, "limit %d%%", c->limit);
    m_caption(&T, 1, mark, t, NULL);
    r_sep(&T);

    if (have && s->adapter_off == 1) snprintf(v, sizeof v, "%sAC bypassed · running on battery", T.hi);
    else {
        snprintf(v, sizeof v, "%s", b->ac == 1 ? "AC connected" : "on battery");
        if (b->adapter_w > 0) { snprintf(t, sizeof t, " · %d W adapter", b->adapter_w); strcat(v, t); }
    }
    r_field(&T, "Power", v);
    if (have) {
        char how[48];
        if (cp->hw_limit) snprintf(how, sizeof how, "hardware (BCLM)");
        else if (cp->inhibit) snprintf(how, sizeof how, "software (%s)", cp->inhibit);
        else snprintf(how, sizeof how, "not supported");
        snprintf(v, sizeof v, "%d%% · %s", c->limit, how);
        if (cp->discharge) { snprintf(t, sizeof t, " · auto-discharge %s", c->auto_discharge ? "on" : "off"); strcat(v, t); }
    } else snprintf(v, sizeof v, "%d%% %s· control state needs sudo or the service", c->limit, T.dim);
    r_field(&T, "Limit", v);
    if (b->health_unreliable)
        snprintf(v, sizeof v, "%sn/a %s(uncalibrated: %d > %d mAh)%s", T.hi, T.dim, b->full_mah, b->design_mah, T.value);
    else if (b->health >= 0) {
        snprintf(v, sizeof v, "%.1f%%", b->health);
        if (b->full_mah > 0 && b->design_mah > 0) { snprintf(t, sizeof t, " %s(%d/%d mAh)%s", T.dim, b->full_mah, b->design_mah, T.value); strcat(v, t); }
    } else snprintf(v, sizeof v, "?");
    if (b->cycles >= 0) { snprintf(t, sizeof t, " · %d cycles", b->cycles); strcat(v, t); }
    r_field(&T, "Health", v);
    if (*b->maker || *b->model) { snprintf(v, sizeof v, "%s %s", b->maker, b->model); r_field(&T, "Pack", v); }
    v[0] = 0;
    bool first = true;
#define ADD(...) do { snprintf(t, sizeof t, __VA_ARGS__); if (!first) strcat(v, " · "); strcat(v, t); first = false; } while (0)
    if (b->has_v) ADD("%.2f V", b->voltage);
    if (b->has_i) ADD("%+.2f A", b->current);
    if (b->has_p) ADD("%+.1f W", b->power);
    if (b->has_t) ADD("%.1f °C", b->temp_c);
#undef ADD
    r_field(&T, "Cell", first ? "?" : v);
    if (b->tte >= 0 || b->ttf >= 0) {
        char m[16];
        fmt_min(b->tte >= 0 ? b->tte : b->ttf, m, sizeof m);
        snprintf(v, sizeof v, "%s in %s", b->tte >= 0 ? "empty" : "full", m);
        r_field(&T, "Time", v);
    }
    fans_t F;
    if (read_fans(&F)) {
        char fm[64];
        fan_mode(c, &F, fm, sizeof fm);
        v[0] = 0;
        for (int i = 0; i < F.n; i++) { snprintf(t, sizeof t, "%s%d", i ? " · " : "", F.f[i].cur); strcat(v, t); }
        snprintf(t, sizeof t, " rpm %s· %s", T.dim, fm);
        strcat(v, t);
        r_field(&T, F.n > 1 ? "Fans" : "Fan", v);
    }
    const char *svc = service_state();
    snprintf(v, sizeof v, "%s● %s%s", !strcmp(svc, "active") ? T.ok : T.bad, svc, T.value);
    if (strcmp(source, "live")) { snprintf(t, sizeof t, " %s· %s", T.dim, source); strcat(v, t); }
    r_field(&T, "Service", v);
    if (conf_app) {
        snprintf(l, sizeof l, " %s! %s manages charging - macbat stays hands-off%s", T.warn, conf_app, T.reset);
        r_line(&T, l);
    }
    r_bot(&T);
}

static int cmd_status(bool json) {
    battery_t b;
    if (!read_battery(&b)) die("no battery found");
    conf_t c = load_conf();
    caps_t cp;
    control_t s = {-1, -1, -1};
    bool have = get_caps(&cp);
    long age = -1;
    char source[96] = "live";
    if (have) s = get_control(&cp);
    else if (read_state(&cp, &s, &age)) { have = true; snprintf(source, sizeof source, "daemon, %lds ago", age); }
    else snprintf(source, sizeof source, "unavailable (run with sudo, or install the service)");
    char model[64];
    model_name(model, sizeof model);
    const char *conf_app = conflict();
    char mbuf[96];
    const char *mode = mode_text(&c, &b, have ? &cp : NULL, have ? &s : NULL, mbuf, sizeof mbuf);
    if (json) {
        printf("{\n");
        json_str("version", MACBAT_VERSION, true);
        if (!g_th) resolve_theme(NULL);
        json_str("theme", g_th->name, true);
        json_str("platform", platform_name(), true);
        json_str("model", model, true);
        printf("  \"conf\": {\"limit\": %d, \"auto_discharge\": %s, \"hysteresis\": %d, \"interval\": %d, \"topup\": %s},\n",
               c.limit, c.auto_discharge ? "true" : "false", c.hysteresis, c.interval, c.topup ? "true" : "false");
        printf("  \"battery\": {\"percent\": %d, \"state\": \"%s\", \"ac\": %s, ", b.percent, b.state,
               b.ac < 0 ? "null" : b.ac ? "true" : "false");
        if (b.has_v) printf("\"voltage\": %.3f, ", b.voltage); else printf("\"voltage\": null, ");
        if (b.has_i) printf("\"current\": %.3f, ", b.current); else printf("\"current\": null, ");
        if (b.has_p) printf("\"power\": %.2f, ", b.power); else printf("\"power\": null, ");
        if (b.has_t) printf("\"temp_c\": %.1f, ", b.temp_c); else printf("\"temp_c\": null, ");
        printf("\"cycles\": %d, \"full_mah\": %d, \"design_mah\": %d, ", b.cycles, b.full_mah, b.design_mah);
        if (b.health >= 0) printf("\"health\": %.1f, ", b.health); else printf("\"health\": null, ");
        printf("\"health_reliable\": %s, \"time_to_empty_min\": %d, \"time_to_full_min\": %d, \"adapter_w\": %d},\n",
               b.health_unreliable ? "false" : "true", b.tte, b.ttf, b.adapter_w);
        if (have)
            printf("  \"caps\": {\"hw_limit\": %s%s%s, \"inhibit\": %s%s%s, \"discharge\": %s%s%s},\n"
                   "  \"control\": {\"hw_limit\": %d, \"charging_blocked\": %d, \"adapter_off\": %d},\n",
                   cp.hw_limit ? "\"" : "", cp.hw_limit ? cp.hw_limit : "null", cp.hw_limit ? "\"" : "",
                   cp.inhibit ? "\"" : "", cp.inhibit ? cp.inhibit : "null", cp.inhibit ? "\"" : "",
                   cp.discharge ? "\"" : "", cp.discharge ? cp.discharge : "null", cp.discharge ? "\"" : "",
                   s.hw_limit, s.charging_blocked, s.adapter_off);
        fans_t F;
        if (read_fans(&F)) {
            char fm[64];
            fan_mode(&c, &F, fm, sizeof fm);
            printf("  \"fans\": ");
            fans_json(&F);
            printf(",\n");
            json_str("fan_mode", fm, true);
            json_str("fan_setting", c.fan, true);
        }
        json_str("mode", mode, true);
        json_str("control_source", source, true);
        json_str("conflict", conf_app, true);
        json_str("service", service_state(), false);
        printf("}\n");
        return 0;
    }
    if (use_boxed()) {
        if (theme().retro) status_retro(&c, &b, &cp, &s, have, source, model, conf_app, mode);
        else status_modern(&c, &b, &cp, &s, have, source, model, conf_app, mode);
        return 0;
    }
    bool utf = utf8_out();
    const char *dot = utf ? "·" : "-";
    printf("macbat %s  %s  %s  %s  %s\n", MACBAT_VERSION, dot, model, dot, platform_name());
    printf("  Battery   ");
    if (b.percent >= 0) printf("%d%%", b.percent); else printf("?%%");
    printf("  [");
    print_bar(b.percent, c.limit, utf);
    printf("]  %s\n", mode);
    if (have && s.adapter_off == 1) printf("  Power     AC adapter bypassed by macbat (running on battery)\n");
    else {
        printf("  Power     %s", b.ac == 1 ? "AC connected" : "on battery");
        if (b.adapter_w > 0) printf("  %s  %d W adapter", dot, b.adapter_w);
        printf("\n");
    }
    if (conf_app) printf("  Warning   %s is managing charging here - macbat will not change anything\n", conf_app);
    if (have) {
        char how[48];
        if (cp.hw_limit) snprintf(how, sizeof how, "hardware (BCLM)");
        else if (cp.inhibit) snprintf(how, sizeof how, "software (%s)", cp.inhibit);
        else snprintf(how, sizeof how, "not supported");
        printf("  Limit     %d%%  %s  %s", c.limit, dot, how);
        if (s.hw_limit > 0 && s.hw_limit != c.limit) printf(", SMC now %d%%", s.hw_limit);
        if (cp.discharge) printf("  %s  auto-discharge: %s (%s)\n", dot, c.auto_discharge ? "on" : "off", cp.discharge);
        else printf("  %s  auto-discharge: not supported\n", dot);
    } else printf("  Limit     %d%%  %s  control state: %s\n", c.limit, dot, source);
    printf("  Health    ");
    if (b.health_unreliable)
        printf("n/a - gauge reports %d mAh full > %d mAh design (uncalibrated gauge)", b.full_mah, b.design_mah);
    else if (b.health >= 0) {
        printf("%.1f%%", b.health);
        if (b.full_mah > 0 && b.design_mah > 0) printf(" (%d/%d mAh)", b.full_mah, b.design_mah);
    } else printf("?");
    if (b.cycles >= 0) printf("  %s  %d cycles", dot, b.cycles);
    if (*b.maker || *b.model) printf("  %s  %s %s", dot, b.maker, b.model);
    printf("\n  Electric  ");
    bool first = true;
#define SEP() do { if (!first) printf("  %s  ", dot); first = false; } while (0)
    if (b.has_v) { SEP(); printf("%.2f V", b.voltage); }
    if (b.has_i) { SEP(); printf("%+.2f A", b.current); }
    if (b.has_p) { SEP(); printf("%+.1f W", b.power); }
    if (b.has_t) { SEP(); printf("%.1f %s", b.temp_c, utf ? "°C" : "C"); }
#undef SEP
    if (first) printf("?");
    printf("\n");
    if (b.tte >= 0 || b.ttf >= 0) {
        char e[16], f[16];
        fmt_min(b.tte, e, sizeof e);
        fmt_min(b.ttf, f, sizeof f);
        printf("  Time      to empty %s  %s  to full %s\n", e, dot, f);
    }
    fans_t F;
    if (read_fans(&F)) {
        char fm[64];
        fan_mode(&c, &F, fm, sizeof fm);
        printf("  Fans      ");
        for (int i = 0; i < F.n; i++) printf("%s%d", i ? " / " : "", F.f[i].cur);
        printf(" rpm  %s  %s\n", dot, fm);
    }
    printf("  Service   %s", service_state());
    if (strcmp(source, "live")) printf("  %s  control: %s", dot, source);
    printf("\n");
    return 0;
}

/* ------------------------------------------------------------------ fan page (macbat fan) */
static void fan_retro(const conf_t *c, const fans_t *F, const char *fm, double temp, const char *model) {
    theme_t T = theme();
    char l[4096], r[256], v[1024], t[96];
    const int GW = 26;
    r_top(&T);
    snprintf(l, sizeof l, "%s█▒░ M A C B A T ░▒█%s  %sFANS%s", T.title, T.reset, T.label, T.reset);
    snprintf(r, sizeof r, "%sv%s · %s%s", T.dim, MACBAT_VERSION, model, T.reset);
    r_split(&T, l, r);
    char pn[64], ts[16];
    snprintf(pn, sizeof pn, "%s", platform_name());
    upcase(pn);
    time_t now = time(NULL);
    strftime(ts, sizeof ts, "%H:%M:%S", localtime(&now));
    snprintf(l, sizeof l, "%s%s%s", T.label, pn, T.reset);
    snprintf(r, sizeof r, "%s%s%s", T.dim, ts, T.reset);
    r_split(&T, l, r);
    r_sep(&T);
    for (int i = 0; i < F->n; i++) {
        const fan_t *f = &F->f[i];
        if (i) r_line(&T, "");
        int mx = f->max > 0 ? f->max : 1;
        int fill = clampi((int)iround((double)f->cur / mx * GW), 0, GW);
        int mark = f->min > 0 ? clampi((int)iround((double)f->min / mx * GW), 0, GW - 1) : -1;
        double frac = fan_limits_ok(f) ? (double)(f->cur - f->min) / (f->max - f->min) : 0;
        const char *lvl = frac < 0.5 ? T.ok : frac < 0.8 ? T.hi : T.bad;
        char lab[48], rpm[16];
        if (!strncmp(f->label, "Fan ", 4)) snprintf(lab, sizeof lab, "FAN %d", i + 1);
        else snprintf(lab, sizeof lab, "FAN %d · %s", i + 1, f->label);
        upcase(lab);
        snprintf(rpm, sizeof rpm, "%d", f->cur < 0 ? 0 : f->cur);
        for (int row = 0; row < 5; row++) {
            char g[2048] = "", dg[256];
            if (row >= 1 && row <= 3) {
                strcat(g, T.frame);
                strcat(g, row == 1 ? "┌" : row == 2 ? "│" : "└");
                for (int k = 0; k < GW; k++) {
                    if (row == 2) {
                        strcat(g, k < fill ? lvl : T.dim);
                        strcat(g, k < fill ? "█" : "░");
                        strcat(g, T.frame);
                    } else strcat(g, k == mark ? (row == 1 ? "┬" : "┴") : "─");
                }
                strcat(g, row == 1 ? "┐ " : row == 2 ? "│ " : "┘ ");
                strcat(g, T.reset);
            } else {
                char txt[96] = "";
                if (row == 0) snprintf(txt, sizeof txt, "%s%s%s", T.label, lab, T.reset);
                else if (mark >= 0) snprintf(txt, sizeof txt, "%*s%s▲ MIN %d%s", mark + 1, "", T.dim, f->min, T.reset);
                vis_trunc(txt, GW + 3);
                snprintf(g, sizeof g, "%s%*s", txt, GW + 3 - vis_width(txt), "");
            }
            big_row(rpm, row, dg, sizeof dg);
            snprintf(l, sizeof l, "  %s     %s%s%s%s", g, lvl, dg, row == 4 ? " RPM" : "", T.reset);
            r_line(&T, l);
        }
    }
    r_sep2(&T);
    char fmu[64];
    snprintf(fmu, sizeof fmu, "%s", fm);
    upcase(fmu);
    snprintf(v, sizeof v, "%s%s", strncmp(fm, "manual", 6) ? T.value : T.hi, fmu);
    r_field(&T, "MODE", v);
    snprintf(v, sizeof v, "%s", T.value);
    for (int i = 0; i < F->n; i++) { snprintf(t, sizeof t, "%s%d-%d", i ? " · " : "", F->f[i].min, F->f[i].max); strcat(v, t); }
    strcat(v, " RPM");
    r_field(&T, "RANGE", v);
    snprintf(v, sizeof v, "%s", T.value);
    for (int i = 0; i < F->n; i++) { snprintf(t, sizeof t, "%s%d", i ? " · " : "", F->f[i].target); strcat(v, t); }
    strcat(v, " RPM");
    r_field(&T, "TARGET", v);
    if (temp > 0) {
        snprintf(v, sizeof v, "%s%.1f°C %s· THERMAL GUARD AT %d°C", temp >= FAN_HOT ? T.bad : T.value, temp, T.dim, FAN_HOT);
        r_field(&T, "CPU", v);
    }
    char sp[32];
    fan_spec_text(c->fan, sp, sizeof sp);
    upcase(sp);
    if (!strcmp(c->fan, "auto")) snprintf(v, sizeof v, "%sAUTO", T.value);
    else {
        snprintf(v, sizeof v, "%s%s %s· %s", T.value, sp, T.dim, svc_active() ? "KEPT BY THE SERVICE" : "SERVICE NOT RUNNING");
        if (*c->fan_paused) { snprintf(t, sizeof t, " · %s OFF", c->fan_paused); upcase(t); strcat(v, t); }
    }
    r_field(&T, "SETTING", v);
    snprintf(l, sizeof l, "%ssudo macbat fan set <rpm|NN%%>  ·  sudo macbat fan auto%s", T.dim, T.reset);
    r_line(&T, l);
    r_bot(&T);
}

static void fan_modern(const conf_t *c, const fans_t *F, const char *fm, double temp, const char *model) {
    theme_t T = theme();
    char l[4096], v[1024], t[160], bar[3072];
    const int BW = RW - 4;
    hdr_modern(&T, "fans", model);
    for (int i = 0; i < F->n; i++) {
        const fan_t *f = &F->f[i];
        r_line(&T, "");
        int mx = f->max > 0 ? f->max : 1;
        int mark = f->min > 0 ? clampi((int)iround((double)f->min / mx * BW), 0, BW - 1) : -1;
        double frac = fan_limits_ok(f) ? (double)(f->cur - f->min) / (f->max - f->min) : 0;
        const char *lvl = frac < 0.5 ? T.ok : frac < 0.8 ? T.warn : T.bad;
        char lab[48], r[64];
        if (!strncmp(f->label, "Fan ", 4)) snprintf(lab, sizeof lab, "Fan %d", i + 1);
        else snprintf(lab, sizeof lab, "Fan %d · %s", i + 1, f->label);
        snprintf(l, sizeof l, " %s%s%s", T.label, lab, T.reset);
        snprintf(r, sizeof r, "%s%d rpm%s ", lvl, f->cur < 0 ? 0 : f->cur, T.reset);
        r_split(&T, l, r);
        m_bar(&T, (double)(f->cur < 0 ? 0 : f->cur) / mx, mark, BW, lvl, frac < 0.5, bar, sizeof bar);
        snprintf(l, sizeof l, " %s", bar);
        r_line(&T, l);
        snprintf(t, sizeof t, "min %d", f->min);
        snprintf(r, sizeof r, "max %d ", f->max);
        m_caption(&T, 1, mark, t, r);
    }
    r_sep(&T);
    snprintf(v, sizeof v, "%s%s", strncmp(fm, "manual", 6) ? "" : T.hi, fm);
    r_field(&T, "Mode", v);
    v[0] = 0;
    for (int i = 0; i < F->n; i++) { snprintf(t, sizeof t, "%s%d-%d", i ? " · " : "", F->f[i].min, F->f[i].max); strcat(v, t); }
    strcat(v, " rpm");
    r_field(&T, "Range", v);
    v[0] = 0;
    for (int i = 0; i < F->n; i++) { snprintf(t, sizeof t, "%s%d", i ? " · " : "", F->f[i].target); strcat(v, t); }
    strcat(v, " rpm");
    r_field(&T, "Target", v);
    if (temp > 0) {
        snprintf(v, sizeof v, "%s%.1f °C %s· thermal guard at %d °C", temp >= FAN_HOT ? T.bad : "", temp, T.dim, FAN_HOT);
        r_field(&T, "CPU", v);
    }
    char sp[32];
    fan_spec_text(c->fan, sp, sizeof sp);
    if (!strcmp(c->fan, "auto")) snprintf(v, sizeof v, "auto");
    else {
        snprintf(v, sizeof v, "%s %s· %s", sp, T.dim, svc_active() ? "kept by the service" : "service not running");
        if (*c->fan_paused) { snprintf(t, sizeof t, " · %s off", c->fan_paused); strcat(v, t); }
    }
    r_field(&T, "Setting", v);
    snprintf(l, sizeof l, "%ssudo macbat fan set <rpm|NN%%>  ·  sudo macbat fan auto%s", T.dim, T.reset);
    r_line(&T, l);
    r_bot(&T);
}

static int cmd_fan(const char *a, const char *b, bool json) {
    if (!a || !strcmp(a, "show") || !strcmp(a, "status")) {
        fans_t F;
        if (!read_fans(&F)) die("no fans found");
        conf_t c = load_conf();
        char fm[64], model[64];
        fan_mode(&c, &F, fm, sizeof fm);
        model_name(model, sizeof model);
        double temp = cpu_temp();
        if (json) {
            printf("{\n  \"fans\": ");
            fans_json(&F);
            printf(",\n");
            json_str("mode", fm, true);
            json_str("setting", c.fan, true);
            json_str("paused_daemon", *c.fan_paused ? c.fan_paused : NULL, true);
            if (temp > 0) printf("  \"cpu_temp_c\": %.1f\n}\n", temp); else printf("  \"cpu_temp_c\": null\n}\n");
        } else if (use_boxed()) {
            if (theme().retro) fan_retro(&c, &F, fm, temp, model);
            else fan_modern(&c, &F, fm, temp, model);
        }
        else {
            printf("macbat %s - %s - fans\n", MACBAT_VERSION, model);
            for (int i = 0; i < F.n; i++)
                printf("  Fan %d     %d rpm  (%s, min %d, max %d, target %d)\n", i + 1, F.f[i].cur, F.f[i].label, F.f[i].min,
                       F.f[i].max, F.f[i].target);
            printf("  Mode      %s\n  Setting   %s%s%s\n", fm, c.fan, *c.fan_paused ? ", paused: " : "", c.fan_paused);
            if (temp > 0) printf("  CPU       %.1f C\n", temp);
        }
        return 0;
    }
    const char *spec = !strcmp(a, "auto") ? "auto" : !strcmp(a, "set") ? b : NULL;
    if (!strcmp(a, "set") && !b) die("usage: macbat fan set <rpm|NN%%>   e.g. 3000 or 50%%");
    if (!spec) die("usage: macbat fan [auto | set <rpm|NN%%>]");
    if (!fan_spec_ok(spec))
        die("fan speed must be an RPM (e.g. 3000 - clamped to the fan's min..max) or a percentage of that range\n"
            "        (0%% = min, 100%% = max). Fans cannot be switched off.");
    need_root("fan");
    char m[256];
    bool had = *load_conf().fan_paused;
    bool ok = fan_apply(spec, m, sizeof m);
    printf("%s\n", m);
    conf_t c = load_conf();
    if (ok && !had && *c.fan_paused)
        printf("%s was stopped%s so it cannot override this speed - 'sudo macbat fan auto' %s it again.\n", c.fan_paused,
               c.fan_paused_enabled ? " and disabled" : "", c.fan_paused_enabled ? "enables and starts" : "starts");
    if (ok && strcmp(spec, "auto") && !svc_active())
        printf("note: the macbat service is not running - the speed is not re-applied after reboot / sleep\n");
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------ settings */
static void apply_now(conf_t c) {
    caps_t cp;
    if (!get_caps(&cp)) die("cannot read the SMC");
    control_t s = get_control(&cp);
    if (cp.hw_limit && s.hw_limit != c.limit) {
        wr_t w[1];
        char err[160];
        w_hw_limit(w, c.limit);
        if (!do_write(w, 1, err, sizeof err)) die("%s", err);
        printf("hardware charge limit set to %d%%\n", c.limit);
    }
    if (strcmp(service_state(), "active")) {
        if (cp.inhibit || cp.discharge)
            printf("note: the macbat service is not running - software limit / auto-discharge need it (sudo macbat install)\n");
    } else printf("the macbat service will apply the change within a few seconds\n");
}

static int cmd_limit(const char *v) {
    conf_t c = load_conf();
    if (!v) { printf("%d%%\n", c.limit); return 0; }
    need_root("limit");
    refuse_if_conflict("limit");
    int pct = (!strcasecmp(v, "off")) ? 100 : atoi(v);
    if (pct < 20 || pct > 100) die("limit must be between 20 and 100 (or 'off')");
    c.limit = pct;
    save_conf(c);
    printf("limit: %d%%%s\n", pct, pct == 100 ? " (off)" : "");
    apply_now(c);
    return 0;
}

static int cmd_discharge(const char *v) {
    conf_t c = load_conf();
    if (!v) { printf("%s\n", c.auto_discharge ? "on" : "off"); return 0; }
    if (strcmp(v, "on") && strcmp(v, "off")) die("usage: macbat discharge on|off");
    need_root("discharge");
    refuse_if_conflict("discharge");
    c.auto_discharge = !strcmp(v, "on");
    save_conf(c);
    printf("auto-discharge above the limit: %s\n", v);
    apply_now(c);
    return 0;
}

static int cmd_restore(void) {
    need_root("restore");
    bool fans = fans_release();
    if (fans && !g_quiet) printf("fans back to automatic control\n");
    refuse_if_conflict("restore");
    caps_t cp;
    if (!get_caps(&cp)) die("cannot read the SMC");
    wr_t w[4];
    int n = restore_plan(&cp, w);
    char err[160], d[160];
    describe(w, n, &cp, d, sizeof d);
    if (!do_write(w, n, err, sizeof err)) die("%s", err);
    if (!g_quiet) printf("%s\n", n ? d : "nothing to restore (charging and adapter already normal)");
    return 0;
}

static int cmd_keys(void) {
    static const char *keys[] = {"BCLM", "CHTE", "CH0B", "CH0C", "CHIE", "CH0I", "ACEN", "ACLC", "B0AV", "B0FC", "B0CT", "TB0T", "BFCL",
                                 "FNum", "F0Ac", "F0Mn", "F0Mx", "F0Tg", "F0Md", "FS! ", "Ftst"};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        smcval_t v;
        int r = smc_read(keys[i], &v);
        if (r < 0) die("cannot read the SMC - try sudo");
        if (!r) { printf("  %s  -\n", keys[i]); continue; }
        printf("  %s  %-4s  ", keys[i], v.type);
        for (int j = 0; j < v.size; j++) printf("%02x", v.data[j]);
        bool ok;
        double x = smc_number(&v, &ok);
        printf("%*s", 10 - 2 * v.size > 0 ? 10 - 2 * v.size : 1, "");
        if (ok) printf(" = %g\n", x); else printf("\n");
    }
    return 0;
}

/* ------------------------------------------------------------------ install / uninstall */
static int cmd_install(void) {
    need_root("install");
    refuse_if_conflict("install");
    caps_t cp;
    if (!get_caps(&cp)) die("cannot read the SMC");
    if (!cp.hw_limit && !cp.inhibit && !cp.discharge) die("this machine has no supported SMC charge-control keys");
    char me[1024], target[1024];
    if (!self_path(me, sizeof me)) die("cannot determine my own path");
    /* packaged installs (/usr/bin, .deb) stay where they are; anything else is copied to /usr/local/bin */
    if (!strcmp(me, "/usr/bin/macbat") || !strcmp(me, LOCAL_BIN)) snprintf(target, sizeof target, "%s", me);
    else {
        mkdir("/usr/local", 0755);
        mkdir("/usr/local/bin", 0755);
        if (!copy_file(me, LOCAL_BIN, 0755)) die("cannot install %s: %s", LOCAL_BIN, strerror(errno));
        snprintf(target, sizeof target, "%s", LOCAL_BIN);
    }
    if (!exists(CONF_PATH)) save_conf(load_conf());
    char body[4096];
#ifdef OS_LINUX
    snprintf(body, sizeof body,
             "[Unit]\nDescription=macbat - battery charge limiter for Apple laptops\n"
             "# no ordering against mbpfan: macbat restarts it around SMC writes\n\n"
             "[Service]\nType=simple\nExecStart=%s daemon\nExecStopPost=%s restore --quiet\n"
             "Restart=on-failure\nRestartSec=10\nTimeoutStopSec=45\n\n[Install]\nWantedBy=multi-user.target\n",
             target, target);
    if (!write_text(SYSTEMD_UNIT, body, 0644)) die("cannot write %s", SYSTEMD_UNIT);
    snprintf(body, sizeof body,
             "#!/bin/sh\n# macbat: never stay in 'adapter bypassed / charging paused' while suspended\n"
             "[ \"$1\" = pre ] && %s restore --quiet\n[ \"$1\" = post ] && systemctl kill -s HUP macbat.service 2>/dev/null\nexit 0\n",
             target);
    mkdir("/usr/lib/systemd/system-sleep", 0755);
    if (!write_text(SLEEP_HOOK, body, 0755)) die("cannot write %s", SLEEP_HOOK);
    RUN("systemctl", "daemon-reload");
    RUN("systemctl", "enable", "macbat.service");
    if (RUN("systemctl", "restart", "macbat.service") != 0) die("systemctl restart macbat failed (see: journalctl -u macbat)");
#else
    snprintf(body, sizeof body,
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
             "<plist version=\"1.0\"><dict>\n"
             "  <key>Label</key><string>" LAUNCHD_LABEL "</string>\n"
             "  <key>ProgramArguments</key><array><string>%s</string><string>daemon</string></array>\n"
             "  <key>RunAtLoad</key><true/>\n  <key>KeepAlive</key><true/>\n"
             "  <key>StandardOutPath</key><string>/var/log/macbat.log</string>\n"
             "  <key>StandardErrorPath</key><string>/var/log/macbat.log</string>\n"
             "</dict></plist>\n", target);
    if (!write_text(LAUNCHD_PLIST, body, 0644)) die("cannot write %s", LAUNCHD_PLIST);
    RUN("launchctl", "bootout", "system/" LAUNCHD_LABEL);
    if (RUN("launchctl", "bootstrap", "system", LAUNCHD_PLIST) != 0) die("launchctl bootstrap failed");
#endif
    conf_t c = load_conf();
    printf("installed %s and started the service (limit %d%%, auto-discharge %s; keys: limit=%s inhibit=%s discharge=%s)\n",
           target, c.limit, c.auto_discharge ? "on" : "off", cp.hw_limit ? cp.hw_limit : "-", cp.inhibit ? cp.inhibit : "-",
           cp.discharge ? cp.discharge : "-");
    return 0;
}

static int cmd_uninstall(void) {
    need_root("uninstall");
#ifdef OS_LINUX
    RUN("systemctl", "disable", "--now", "macbat.service");
    unlink(SYSTEMD_UNIT);
    unlink(SLEEP_HOOK);
    RUN("systemctl", "daemon-reload");
#else
    RUN("launchctl", "bootout", "system/" LAUNCHD_LABEL);
    unlink(LAUNCHD_PLIST);
#endif
    unlink(STATE_PATH);
    unlink(CTL_PATH);
    conf_t fc = load_conf();
    if (strcmp(fc.fan, "auto") || *fc.fan_paused) {
        char m[256];
        fan_apply("auto", m, sizeof m);
        printf("%s\n", m);
    }
    if (conflict() && !g_force) {
        printf("service removed. %s is managing charging, so SMC values were left untouched.\n", conflict());
        return 0;
    }
    caps_t cp;
    if (get_caps(&cp)) {
        wr_t w[6];
        int n = restore_plan(&cp, w);
        if (cp.hw_limit) n += w_hw_limit(w + n, 100);
        char err[160];
        if (!do_write(w, n, err, sizeof err)) die("%s", err);
    }
    printf("service removed; charging restored to normal (limit 100%%). The binary and %s were left in place.\n", CONF_PATH);
    return 0;
}

/* ------------------------------------------------------------------ system power / display settings */
__attribute__((unused)) static void set_conf_str(char *dst, size_t n, const char *v) { snprintf(dst, n, "%s", v ? v : ""); }

#ifdef OS_LINUX
static bool tlp_active(void) {
    static int v = -1;
    if (v < 0) v = RUN("systemctl", "is-enabled", "--quiet", "tlp") == 0;
    return v == 1;
}

/* Settings that TLP also manages go into /etc/tlp.d/90-macbat.conf so TLP and macbat never fight. */
static bool tlp_set(const char *k1, const char *k2, const char *value) {
    const char *path = "/etc/tlp.d/90-macbat.conf";
    char body[8192] = "", line[256];
    FILE *f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            size_t l1 = strlen(k1), l2 = k2 ? strlen(k2) : 0;
            if ((!strncmp(line, k1, l1) && line[l1] == '=') || (k2 && !strncmp(line, k2, l2) && line[l2] == '=')) continue;
            strncat(body, line, sizeof body - strlen(body) - 1);
        }
        fclose(f);
    } else strcat(body, "# written by macbat (settings screen) - overrides other TLP files\n");
    snprintf(line, sizeof line, "%s=%s\n", k1, value);
    strncat(body, line, sizeof body - strlen(body) - 1);
    if (k2) { snprintf(line, sizeof line, "%s=%s\n", k2, value); strncat(body, line, sizeof body - strlen(body) - 1); }
    if (!write_text(path, body, 0644)) return false;
    return RUN("tlp", "start") == 0;
}

static bool find_entry(const char *base, const char *contains, const char *file, char *out, size_t n) {
    DIR *d = opendir(base);
    if (!d) return false;
    struct dirent *e;
    bool ok = false;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || (contains && !strstr(e->d_name, contains))) continue;
        snprintf(out, n, "%s/%s/%s", base, e->d_name, file);
        if (exists(out)) { ok = true; break; }
    }
    closedir(d);
    return ok;
}

static bool wifi_iface(char *out, size_t n) {
    DIR *d = opendir("/sys/class/net");
    if (!d) return false;
    struct dirent *e;
    char p[300];
    bool ok = false;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "/sys/class/net/%s/wireless", e->d_name);
        if (exists(p)) { snprintf(out, n, "%s", e->d_name); ok = true; break; }
    }
    closedir(d);
    return ok;
}

static bool have_cmd(const char *c) { char o[8]; return RUN_OUT(o, "sh", "-c", "command -v \"$0\"", c) == 0; }

static bool rotational_disks(char (*dev)[32], int max, int *n) {
    *n = 0;
    DIR *d = opendir("/sys/block");
    if (!d) return false;
    struct dirent *e;
    char p[300];
    while ((e = readdir(d)) && *n < max) {
        if (strncmp(e->d_name, "sd", 2)) continue;
        snprintf(p, sizeof p, "/sys/block/%s/queue/rotational", e->d_name);
        snprintf(dev[*n], 32, "/dev/%.24s", e->d_name);
        if (read_long(p, 0) == 1) {
            snprintf(p, sizeof p, "/sys/block/%s/size", e->d_name);
            if (read_long(p, 0) > 0) (*n)++;
        }
    }
    closedir(d);
    return *n > 0;
}

/* --- individual knobs: get current value token / set (apply now) --- */
static bool lx_blank_get(char *o, size_t n) {
    long v = read_long("/sys/module/kernel/parameters/consoleblank", -1);
    if (v < 0) return false;
    snprintf(o, n, "%ld", v);
    return true;
}
static bool lx_blank_apply(int sec) {
    char esc[48];
    int min = (sec + 59) / 60;
    snprintf(esc, sizeof esc, "\033[9;%d]\033[14;1]", min);
    return write_file("/dev/tty63", esc); /* blank interval is global; tty63 is never anyone's console */
}
static bool lx_alpm_get(char *o, size_t n) { char p[300]; return find_entry("/sys/class/scsi_host", NULL, "link_power_management_policy", p, sizeof p) && read_text(p, o, n); }
static bool lx_alpm_apply(const char *v) {
    DIR *d = opendir("/sys/class/scsi_host");
    if (!d) return false;
    struct dirent *e;
    char p[300];
    int ok = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "/sys/class/scsi_host/%s/link_power_management_policy", e->d_name);
        if (exists(p) && write_file(p, v)) ok++;
    }
    closedir(d);
    return ok > 0;
}
static bool lx_wifi_get(char *o, size_t n) {
    char ifc[64], out[256];
    if (!wifi_iface(ifc, sizeof ifc) || RUN_OUT(out, "iw", "dev", ifc, "get", "power_save") != 0) return false;
    snprintf(o, n, "%s", strstr(out, ": on") ? "on" : "off");
    return true;
}
static bool lx_wifi_apply(const char *v) { char ifc[64]; return wifi_iface(ifc, sizeof ifc) && RUN("iw", "dev", ifc, "set", "power_save", v) == 0; }
static bool lx_usb_get(char *o, size_t n) {
    DIR *d = opendir("/sys/bus/usb/devices");
    if (!d) return false;
    struct dirent *e;
    char p[300], v[16];
    int a = 0, on = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "/sys/bus/usb/devices/%s/power/control", e->d_name);
        if (read_text(p, v, sizeof v)) { if (!strcmp(v, "auto")) a++; else on++; }
    }
    closedir(d);
    snprintf(o, n, "%s", on == 0 && a ? "on" : a == 0 ? "off" : "mixed");
    return a + on > 0;
}
static bool lx_usb_apply(const char *v) {
    DIR *d = opendir("/sys/bus/usb/devices");
    if (!d) return false;
    struct dirent *e;
    char p[300];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "/sys/bus/usb/devices/%s/power/control", e->d_name);
        if (exists(p)) write_file(p, !strcmp(v, "on") ? "auto" : "on");
    }
    closedir(d);
    return true;
}
static bool lx_gov_get(char *o, size_t n) { return read_text("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", o, n); }
static bool lx_gov_apply(const char *v) {
    char p[128];
    int ok = 0;
    for (int i = 0; i < 1024; i++) {
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", i);
        if (!exists(p)) { if (i > 0) break; continue; }
        if (write_file(p, v)) ok++;
    }
    return ok > 0;
}
static bool lx_turbo_get(char *o, size_t n) {
    long nt = read_long("/sys/devices/system/cpu/intel_pstate/no_turbo", -1);
    if (nt >= 0) { snprintf(o, n, "%s", nt ? "off" : "on"); return true; }
    long b = read_long("/sys/devices/system/cpu/cpufreq/boost", -1);
    if (b >= 0) { snprintf(o, n, "%s", b ? "on" : "off"); return true; }
    return false;
}
static bool lx_turbo_apply(const char *v) {
    bool on = !strcmp(v, "on");
    if (exists("/sys/devices/system/cpu/intel_pstate/no_turbo")) return write_file("/sys/devices/system/cpu/intel_pstate/no_turbo", on ? "0" : "1");
    return write_file("/sys/devices/system/cpu/cpufreq/boost", on ? "1" : "0");
}
static bool lx_aspm_get(char *o, size_t n) {
    char b[128];
    if (!read_text("/sys/module/pcie_aspm/parameters/policy", b, sizeof b)) return false;
    char *s = strchr(b, '['), *e = s ? strchr(s, ']') : NULL;
    if (!s || !e) return false;
    *e = 0;
    snprintf(o, n, "%s", s + 1);
    return true;
}
static bool lx_aspm_apply(const char *v) { return write_file("/sys/module/pcie_aspm/parameters/policy", v); }
static bool lx_spindown_apply(const char *v) {
    char dev[8][32];
    int n;
    if (!rotational_disks(dev, 8, &n)) return false;
    for (int i = 0; i < n; i++) RUN("hdparm", "-q", "-S", v, dev[i]);
    return true;
}

/* re-applied by the daemon at boot / on config change (only what TLP does not manage) */
static void apply_sys_settings(const conf_t *c) {
    if (c->screen_blank >= 0) lx_blank_apply(c->screen_blank);
    if (tlp_active()) return;
    if (*c->sata_alpm) lx_alpm_apply(c->sata_alpm);
    if (*c->wifi_ps) lx_wifi_apply(c->wifi_ps);
    if (*c->usb_as) lx_usb_apply(c->usb_as);
    if (*c->cpu_gov) lx_gov_apply(c->cpu_gov);
    if (*c->cpu_turbo) lx_turbo_apply(c->cpu_turbo);
    if (*c->aspm) lx_aspm_apply(c->aspm);
    if (*c->spindown) lx_spindown_apply(c->spindown);
}
#else
static void apply_sys_settings(const conf_t *c) { (void)c; }
#endif

/* ------------------------------------------------------------------ settings model */
enum { K_OPTS, K_NUM, K_ACTION };
typedef struct {
    const char *section, *label;
    int kind;
    const char *opts[10], *names[10];
    int lo, hi, step;
    bool (*avail)(void);
    int (*dyn)(char (*o)[32], int max);
    bool (*get)(char *out, size_t n);
    bool (*set)(const char *tok, char *msg, size_t n);
    bool (*show)(char *out, size_t n); /* optional: live text instead of the value name */
} setting_t;

static bool av_always(void) { return true; }
__attribute__((unused)) static const char *persist_note(void) { return svc_active() ? "" : " (install the service to keep it after reboot)"; }

/* battery */
static bool g_limit(char *o, size_t n) { snprintf(o, n, "%d", load_conf().limit); return true; }
static bool s_limit(const char *t, char *m, size_t n) {
    if (conflict() && !g_force) { snprintf(m, n, "%s manages charging here - limit not changed", conflict()); return false; }
    conf_t c = load_conf();
    c.limit = clampi(atoi(t), 20, 100);
    save_conf(c);
    caps_t cp;
    if (get_caps(&cp) && cp.hw_limit) {
        control_t s = get_control(&cp);
        if (s.hw_limit != c.limit) {
            wr_t w[1];
            char err[160];
            w_hw_limit(w, c.limit);
            if (!do_write(w, 1, err, sizeof err)) { snprintf(m, n, "saved, but SMC write failed: %s", err); return false; }
        }
    }
    snprintf(m, n, "charge limit %d%%%s", c.limit, svc_active() ? "" : " - start the service for discharge/software limit");
    return true;
}
static bool g_dis(char *o, size_t n) { snprintf(o, n, "%s", load_conf().auto_discharge ? "on" : "off"); return true; }
static bool s_dis(const char *t, char *m, size_t n) {
    if (conflict() && !g_force) { snprintf(m, n, "%s manages charging here - not changed", conflict()); return false; }
    conf_t c = load_conf();
    c.auto_discharge = !strcmp(t, "on");
    save_conf(c);
    snprintf(m, n, "auto-discharge above the limit: %s", t);
    return true;
}
static bool g_act(char *o, size_t n) { snprintf(o, n, "run"); return true; }
static bool s_topup(const char *t, char *m, size_t n) {
    if (conflict() && !g_force) { snprintf(m, n, "%s manages charging here - not changed", conflict()); return false; }
    conf_t c = load_conf();
    c.topup = !strcmp(t, "on");
    c.topup_since = (long)time(NULL);
    save_conf(c);
    if (c.topup) snprintf(m, n, "top-up: charging to 100%% once, then back to the %d%% limit%s", c.limit,
                          svc_active() ? "" : " (needs the service)");
    else snprintf(m, n, "top-up cancelled - limit %d%% again", c.limit);
    return true;
}
static bool s_restore(const char *t, char *m, size_t n) {
    (void)t;
    if (conflict() && !g_force) { snprintf(m, n, "%s manages charging here - not touched", conflict()); return false; }
    caps_t cp;
    if (!get_caps(&cp)) { snprintf(m, n, "cannot read the SMC"); return false; }
    wr_t w[4];
    int k = restore_plan(&cp, w);
    char err[160], d[160];
    describe(w, k, &cp, d, sizeof d);
    if (!do_write(w, k, err, sizeof err)) { snprintf(m, n, "%s", err); return false; }
    snprintf(m, n, "%s", k ? d : "charging and adapter already normal");
    return true;
}

/* look (both platforms) */
static int dyn_theme(char (*o)[32], int max) {
    int k = 0;
    for (int i = 0; i < NTHEMES && k < max; i++) snprintf(o[k++], 32, "%s", THEMES[i].name);
    return k;
}
static bool g_theme(char *o, size_t n) { if (!g_th) resolve_theme(NULL); snprintf(o, n, "%s", g_th->name); return true; }
static bool s_theme(const char *t, char *m, size_t n) {
    const theme_def_t *d = find_theme(t);
    if (!d) { snprintf(m, n, "unknown theme %s", t); return false; }
    conf_t c = load_conf();
    snprintf(c.theme, sizeof c.theme, "%s", d->name);
    save_conf(c);
    g_th = d;
    snprintf(g_th_src, sizeof g_th_src, "%s", CONF_PATH);
    snprintf(m, n, "theme %s (saved)", d->name);
    return true;
}

/* fans (both platforms) */
static bool av_fans(void) { fans_t F; return read_fans(&F); }
static int dyn_fan(char (*o)[32], int max) {
    int k = 0;
    snprintf(o[k++], 32, "auto");
    for (int p = 0; p <= 100 && k < max; p += 10) snprintf(o[k++], 32, "%d%%", p);
    return k;
}
static bool g_fan(char *o, size_t n) { /* nearest 10 % step, so the arrows continue from a CLI-set rpm */
    conf_t c = load_conf();
    fans_t F;
    if (!strcmp(c.fan, "auto")) { snprintf(o, n, "auto"); return true; }
    int p = atoi(c.fan);
    if (!strchr(c.fan, '%')) {
        if (!read_fans(&F) || !fan_limits_ok(&F.f[0])) { snprintf(o, n, "auto"); return true; }
        p = (int)iround(100.0 * (p - F.f[0].min) / (F.f[0].max - F.f[0].min));
    }
    snprintf(o, n, "%d%%", clampi((int)iround(p / 10.0) * 10, 0, 100));
    return true;
}
static bool s_fan(const char *t, char *m, size_t n) { return fan_apply(t, m, n); }
static bool show_fan(char *o, size_t n) {
    fans_t F;
    if (!read_fans(&F)) return false;
    conf_t c = load_conf();
    char r[48] = "", t[16], sp[32];
    for (int i = 0; i < F.n; i++) { snprintf(t, sizeof t, "%s%d", i ? "/" : "", F.f[i].cur); strcat(r, t); }
#ifdef OS_LINUX
    const char *other = fan_daemon();
#else
    const char *other = fan_app();
#endif
    fan_spec_text(c.fan, sp, sizeof sp);
    if (other) snprintf(o, n, "%s · %s RPM", other, r);
    else if (!strcmp(c.fan, "auto")) snprintf(o, n, "AUTO · %s RPM", r);
    else snprintf(o, n, "%s · NOW %s", sp, r);
    upcase(o);
    return true;
}

#ifdef OS_LINUX
static bool g_blank(char *o, size_t n) { return lx_blank_get(o, n); }
static bool s_blank(const char *t, char *m, size_t n) {
    int sec = atoi(t);
    if (!lx_blank_apply(sec)) { snprintf(m, n, "cannot set screen blanking: %s", strerror(errno)); return false; }
    conf_t c = load_conf();
    c.screen_blank = sec;
    save_conf(c);
    if (sec) snprintf(m, n, "screen turns off after %d min without input%s", (sec + 59) / 60, persist_note());
    else snprintf(m, n, "screen never turns off%s", persist_note());
    return true;
}
static bool bl_path(const char *base, const char *contains, char *p, size_t n) { return find_entry(base, contains, "brightness", p, n); }
static bool g_level(const char *base, const char *contains, char *o, size_t n) {
    char p[300], mx[300];
    if (!bl_path(base, contains, p, sizeof p)) return false;
    snprintf(mx, sizeof mx, "%.*smax_brightness", (int)(strlen(p) - strlen("brightness")), p);
    long cur = read_long(p, -1), max = read_long(mx, 0);
    if (cur < 0 || max <= 0) return false;
    snprintf(o, n, "%ld", iround(100.0 * cur / max));
    return true;
}
static bool s_level(const char *base, const char *contains, int pct, int minraw, char *m, size_t n, const char *what) {
    char p[300], mx[300], v[32];
    if (!bl_path(base, contains, p, sizeof p)) { snprintf(m, n, "no %s found", what); return false; }
    snprintf(mx, sizeof mx, "%.*smax_brightness", (int)(strlen(p) - strlen("brightness")), p);
    long max = read_long(mx, 0), raw = iround(max * pct / 100.0);
    if (raw < minraw) raw = minraw;
    snprintf(v, sizeof v, "%ld", raw);
    if (!write_file(p, v)) { snprintf(m, n, "cannot set %s: %s", what, strerror(errno)); return false; }
    snprintf(m, n, "%s %d%% (systemd restores it at boot)", what, pct);
    return true;
}
static bool av_bl(void) { char p[300]; return bl_path("/sys/class/backlight", NULL, p, sizeof p); }
static bool g_bl(char *o, size_t n) { return g_level("/sys/class/backlight", NULL, o, n); }
static bool s_bl(const char *t, char *m, size_t n) { return s_level("/sys/class/backlight", NULL, atoi(t), 1, m, n, "display brightness"); }
static bool av_kbd(void) { char p[300]; return bl_path("/sys/class/leds", "kbd_backlight", p, sizeof p); }
static bool g_kbd(char *o, size_t n) { return g_level("/sys/class/leds", "kbd_backlight", o, n); }
static bool s_kbd(const char *t, char *m, size_t n) { return s_level("/sys/class/leds", "kbd_backlight", atoi(t), 0, m, n, "keyboard backlight"); }

static bool logind_get(const char *prop, char *o, size_t n) {
    char out[256];
    if (RUN_OUT(out, "busctl", "get-property", "org.freedesktop.login1", "/org/freedesktop/login1",
                "org.freedesktop.login1.Manager", prop) != 0) return false;
    char *q = strchr(out, '"');
    if (q) {
        char *e = strchr(q + 1, '"');
        if (!e) return false;
        *e = 0;
        snprintf(o, n, "%s", q + 1);
        return true;
    }
    char *sp = strchr(out, ' ');
    if (!sp) return false;
    snprintf(o, n, "%s", sp + 1);
    o[strcspn(o, "\n")] = 0;
    return true;
}
static bool logind_set(const char *k1, const char *v1, const char *k2, const char *v2) {
    const char *dir = "/etc/systemd/logind.conf.d", *path = "/etc/systemd/logind.conf.d/90-macbat.conf";
    char body[4096] = "[Login]\n", line[256];
    FILE *f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            if (line[0] == '[' || line[0] == '\n') continue;
            size_t l1 = strlen(k1), l2 = k2 ? strlen(k2) : 0;
            if ((!strncmp(line, k1, l1) && line[l1] == '=') || (k2 && !strncmp(line, k2, l2) && line[l2] == '=')) continue;
            strncat(body, line, sizeof body - strlen(body) - 1);
        }
        fclose(f);
    }
    snprintf(line, sizeof line, "%s=%s\n", k1, v1);
    strncat(body, line, sizeof body - strlen(body) - 1);
    if (k2) { snprintf(line, sizeof line, "%s=%s\n", k2, v2); strncat(body, line, sizeof body - strlen(body) - 1); }
    mkdir(dir, 0755);
    if (!write_text(path, body, 0644)) return false;
    return RUN("systemctl", "reload", "systemd-logind") == 0;
}
static bool lid_closed(void) {
    char p[300], s[64];
    return find_entry("/proc/acpi/button/lid", NULL, "state", p, sizeof p) && read_text(p, s, sizeof s) && strstr(s, "closed");
}
static bool g_lid(char *o, size_t n) { return logind_get("HandleLidSwitch", o, n); }
static bool s_lid(const char *t, char *m, size_t n) {
    if (strcmp(t, "ignore") && lid_closed()) { snprintf(m, n, "the lid is closed - open it first (it would act immediately)"); return false; }
    if (!strcmp(t, "suspend")) RUN("systemctl", "unmask", "sleep.target", "suspend.target");
    if (!logind_set("HandleLidSwitch", t, "HandleLidSwitchExternalPower", t)) { snprintf(m, n, "cannot update logind"); return false; }
    snprintf(m, n, "closing the lid: %s", t);
    return true;
}
static bool g_pwr(char *o, size_t n) { return logind_get("HandlePowerKey", o, n); }
static bool s_pwr(const char *t, char *m, size_t n) {
    if (!logind_set("HandlePowerKey", t, NULL, NULL)) { snprintf(m, n, "cannot update logind"); return false; }
    snprintf(m, n, "power button: %s%s", t, !strcmp(t, "ignore") ? " (another tool may handle it)" : "");
    return true;
}
static bool g_idle(char *o, size_t n) {
    char a[32], us[32];
    if (!logind_get("IdleAction", a, sizeof a) || !logind_get("IdleActionUSec", us, sizeof us)) return false;
    if (!strcmp(a, "ignore")) snprintf(o, n, "0"); else snprintf(o, n, "%lld", atoll(us) / 1000000);
    return true;
}
static bool s_idle(const char *t, char *m, size_t n) {
    if (!strcmp(t, "0")) { if (!logind_set("IdleAction", "ignore", NULL, NULL)) goto fail; snprintf(m, n, "no automatic sleep when idle"); return true; }
    RUN("systemctl", "unmask", "sleep.target", "suspend.target");
    if (!logind_set("IdleAction", "suspend", "IdleActionSec", t)) goto fail;
    snprintf(m, n, "sleep after %d min without any session activity", atoi(t) / 60);
    return true;
fail:
    snprintf(m, n, "cannot update logind");
    return false;
}
#define TLP_OR_CONF(field, k1, k2, tlpval, applyfn, label)                                                       \
    do {                                                                                                          \
        if (!applyfn(t)) { snprintf(m, n, "cannot set " label ": %s", strerror(errno)); return false; }         \
        if (tlp_active()) {                                                                                       \
            if (!tlp_set(k1, k2, tlpval)) { snprintf(m, n, label " set now, but TLP config update failed"); return false; } \
            snprintf(m, n, label ": %s (saved in TLP config)", t);                                                \
        } else {                                                                                                  \
            conf_t c = load_conf();                                                                               \
            set_conf_str(c.field, sizeof c.field, t);                                                             \
            save_conf(c);                                                                                         \
            snprintf(m, n, label ": %s%s", t, persist_note());                                                    \
        }                                                                                                         \
        return true;                                                                                              \
    } while (0)
static bool av_alpm(void) { char p[300]; return find_entry("/sys/class/scsi_host", NULL, "link_power_management_policy", p, sizeof p); }
static bool s_alpm(const char *t, char *m, size_t n) { TLP_OR_CONF(sata_alpm, "SATA_LINKPWR_ON_AC", "SATA_LINKPWR_ON_BAT", t, lx_alpm_apply, "disk link power"); }
static bool av_spin(void) { char d[8][32]; int k; return have_cmd("hdparm") && rotational_disks(d, 8, &k); }
static bool g_spin(char *o, size_t n) { conf_t c = load_conf(); snprintf(o, n, "%s", *c.spindown ? c.spindown : "?"); return true; }
static bool s_spin(const char *t, char *m, size_t n) { TLP_OR_CONF(spindown, "DISK_SPINDOWN_TIMEOUT_ON_AC", "DISK_SPINDOWN_TIMEOUT_ON_BAT", t, lx_spindown_apply, "disk spin-down"); }
static bool av_wifi(void) { char i[64]; return wifi_iface(i, sizeof i) && have_cmd("iw"); }
static bool s_wifi(const char *t, char *m, size_t n) { TLP_OR_CONF(wifi_ps, "WIFI_PWR_ON_AC", "WIFI_PWR_ON_BAT", t, lx_wifi_apply, "Wi-Fi power save"); }
static bool av_usb(void) { return exists("/sys/bus/usb/devices"); }
static bool s_usb(const char *t, char *m, size_t n) { TLP_OR_CONF(usb_as, "USB_AUTOSUSPEND", NULL, !strcmp(t, "on") ? "1" : "0", lx_usb_apply, "USB autosuspend"); }
static bool av_gov(void) { return exists("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"); }
static int dyn_gov(char (*o)[32], int max) {
    char b[256];
    if (!read_text("/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors", b, sizeof b)) return 0;
    int k = 0;
    for (char *tok = strtok(b, " "); tok && k < max; tok = strtok(NULL, " ")) snprintf(o[k++], 32, "%s", tok);
    return k;
}
static bool s_gov(const char *t, char *m, size_t n) { TLP_OR_CONF(cpu_gov, "CPU_SCALING_GOVERNOR_ON_AC", "CPU_SCALING_GOVERNOR_ON_BAT", t, lx_gov_apply, "CPU governor"); }
static bool av_turbo(void) { char o[8]; return lx_turbo_get(o, sizeof o); }
static bool s_turbo(const char *t, char *m, size_t n) { TLP_OR_CONF(cpu_turbo, "CPU_BOOST_ON_AC", "CPU_BOOST_ON_BAT", !strcmp(t, "on") ? "1" : "0", lx_turbo_apply, "CPU turbo"); }
static bool av_aspm(void) { return exists("/sys/module/pcie_aspm/parameters/policy"); }
static int dyn_aspm(char (*o)[32], int max) {
    char b[128];
    if (!read_text("/sys/module/pcie_aspm/parameters/policy", b, sizeof b)) return 0;
    int k = 0;
    for (char *tok = strtok(b, " "); tok && k < max; tok = strtok(NULL, " ")) {
        if (*tok == '[') tok++;
        tok[strcspn(tok, "]")] = 0;
        snprintf(o[k++], 32, "%s", tok);
    }
    return k;
}
static bool s_aspm(const char *t, char *m, size_t n) { TLP_OR_CONF(aspm, "PCIE_ASPM_ON_AC", "PCIE_ASPM_ON_BAT", t, lx_aspm_apply, "PCIe ASPM"); }

static const setting_t SETTINGS[] = {
    {"BATTERY", "Charge limit", K_NUM, {0}, {0}, 20, 100, 5, av_always, NULL, g_limit, s_limit, NULL},
    {"BATTERY", "Auto-discharge above limit", K_OPTS, {"on", "off"}, {0}, 0, 0, 0, av_always, NULL, g_dis, s_dis, NULL},
    {"BATTERY", "Resume charging now", K_ACTION, {0}, {0}, 0, 0, 0, av_always, NULL, g_act, s_restore, NULL},
    {"FANS", "Fan speed", K_OPTS, {0}, {0}, 0, 0, 0, av_fans, dyn_fan, g_fan, s_fan, show_fan},
    {"DISPLAY", "Screen off when idle", K_OPTS, {"0", "60", "120", "300", "600", "900", "1800"},
     {"NEVER", "1 MIN", "2 MIN", "5 MIN", "10 MIN", "15 MIN", "30 MIN"}, 0, 0, 0, av_always, NULL, g_blank, s_blank, NULL},
    {"DISPLAY", "Display brightness", K_NUM, {0}, {0}, 5, 100, 5, av_bl, NULL, g_bl, s_bl, NULL},
    {"DISPLAY", "Keyboard backlight", K_NUM, {0}, {0}, 0, 100, 10, av_kbd, NULL, g_kbd, s_kbd, NULL},
    {"SLEEP", "Lid close", K_OPTS, {"suspend", "lock", "ignore", "poweroff"}, {0}, 0, 0, 0, av_always, NULL, g_lid, s_lid, NULL},
    {"SLEEP", "Power button", K_OPTS, {"poweroff", "suspend", "ignore"}, {0}, 0, 0, 0, av_always, NULL, g_pwr, s_pwr, NULL},
    {"SLEEP", "Sleep when idle", K_OPTS, {"0", "900", "1800", "3600", "7200"}, {"NEVER", "15 MIN", "30 MIN", "1 HOUR", "2 HOURS"},
     0, 0, 0, av_always, NULL, g_idle, s_idle, NULL},
    {"DISK", "SATA link power (ALPM)", K_OPTS, {"max_performance", "medium_power", "med_power_with_dipm", "min_power"},
     {"MAX PERFORMANCE", "MEDIUM", "MEDIUM + DIPM", "MIN POWER"}, 0, 0, 0, av_alpm, NULL, lx_alpm_get, s_alpm, NULL},
    {"DISK", "Disk spin-down", K_OPTS, {"0", "60", "120", "241", "242"}, {"NEVER", "5 MIN", "10 MIN", "30 MIN", "1 HOUR"},
     0, 0, 0, av_spin, NULL, g_spin, s_spin, NULL},
    {"POWER SAVING", "Wi-Fi power save", K_OPTS, {"on", "off"}, {0}, 0, 0, 0, av_wifi, NULL, lx_wifi_get, s_wifi, NULL},
    {"POWER SAVING", "USB autosuspend", K_OPTS, {"on", "off"}, {0}, 0, 0, 0, av_usb, NULL, lx_usb_get, s_usb, NULL},
    {"POWER SAVING", "CPU governor", K_OPTS, {0}, {0}, 0, 0, 0, av_gov, dyn_gov, lx_gov_get, s_gov, NULL},
    {"POWER SAVING", "CPU turbo boost", K_OPTS, {"on", "off"}, {0}, 0, 0, 0, av_turbo, NULL, lx_turbo_get, s_turbo, NULL},
    {"POWER SAVING", "PCIe ASPM", K_OPTS, {0}, {0}, 0, 0, 0, av_aspm, dyn_aspm, lx_aspm_get, s_aspm, NULL},
    {"APPEARANCE", "Theme", K_OPTS, {0}, {0}, 0, 0, 0, av_always, dyn_theme, g_theme, s_theme, NULL},
};
#else /* macOS: pmset */
static bool pm_get(const char *key, char *o, size_t n) {
    static char out[16384];
    if (RUN_OUT(out, "pmset", "-g") != 0) return false;
    for (char *l = strtok(out, "\n"); l; l = strtok(NULL, "\n")) {
        while (*l == ' ') l++;
        size_t k = strlen(key);
        if (!strncmp(l, key, k) && (l[k] == ' ' || l[k] == '\t')) {
            snprintf(o, n, "%d", atoi(l + k));
            return true;
        }
    }
    return false;
}
static bool pm_set(const char *key, const char *v, char *m, size_t n, const char *what) {
    if (RUN("pmset", "-a", key, v) != 0) { snprintf(m, n, "pmset %s %s failed", key, v); return false; }
    snprintf(m, n, "%s: %s", what, v);
    return true;
}
#define PM(name, key, what)                                                                 \
    static bool g_##name(char *o, size_t n) { return pm_get(key, o, n); }                  \
    static bool s_##name(const char *t, char *m, size_t n) { return pm_set(key, t, m, n, what); } \
    static bool av_##name(void) { char o[16]; return pm_get(key, o, sizeof o); }
PM(dsl, "displaysleep", "display sleep (min)")
PM(ssl, "sleep", "system sleep (min)")
PM(dks, "disksleep", "disk sleep (min)")
PM(nap, "powernap", "power nap")
PM(lpm, "lowpowermode", "low power mode")
PM(pwm, "powermode", "energy mode (0 automatic, 1 low power, 2 high power)")
static const setting_t SETTINGS[] = {
    {"BATTERY", "Charge limit", K_NUM, {0}, {0}, 20, 100, 5, av_always, NULL, g_limit, s_limit, NULL},
    {"BATTERY", "Auto-discharge above limit", K_OPTS, {"on", "off"}, {0}, 0, 0, 0, av_always, NULL, g_dis, s_dis, NULL},
    {"BATTERY", "Resume charging now", K_ACTION, {0}, {0}, 0, 0, 0, av_always, NULL, g_act, s_restore, NULL},
    {"FANS", "Fan speed", K_OPTS, {0}, {0}, 0, 0, 0, av_fans, dyn_fan, g_fan, s_fan, show_fan},
    {"DISPLAY", "Display sleep", K_OPTS, {"0", "1", "2", "5", "10", "15", "30"}, {"NEVER", "1 MIN", "2 MIN", "5 MIN", "10 MIN", "15 MIN", "30 MIN"},
     0, 0, 0, av_dsl, NULL, g_dsl, s_dsl, NULL},
    {"SLEEP", "System sleep", K_OPTS, {"0", "1", "5", "10", "15", "30", "60"}, {"NEVER", "1 MIN", "5 MIN", "10 MIN", "15 MIN", "30 MIN", "1 HOUR"},
     0, 0, 0, av_ssl, NULL, g_ssl, s_ssl, NULL},
    {"SLEEP", "Power Nap", K_OPTS, {"1", "0"}, {"ON", "OFF"}, 0, 0, 0, av_nap, NULL, g_nap, s_nap, NULL},
    {"SLEEP", "Low power mode", K_OPTS, {"1", "0"}, {"ON", "OFF"}, 0, 0, 0, av_lpm, NULL, g_lpm, s_lpm, NULL},
    {"SLEEP", "Energy mode", K_OPTS, {"0", "1", "2"}, {"AUTOMATIC", "LOW POWER", "HIGH POWER"}, 0, 0, 0, av_pwm, NULL, g_pwm, s_pwm, NULL},
    {"DISK", "Disk sleep", K_OPTS, {"0", "10", "30", "60"}, {"NEVER", "10 MIN", "30 MIN", "1 HOUR"}, 0, 0, 0, av_dks, NULL, g_dks, s_dks, NULL},
    {"APPEARANCE", "Theme", K_OPTS, {0}, {0}, 0, 0, 0, av_always, dyn_theme, g_theme, s_theme, NULL},
};
#endif
#define NSET ((int)(sizeof SETTINGS / sizeof SETTINGS[0]))
/* ------------------------------------------------------------------ control socket (service) */
/* Lets the menu bar app - or any admin - change settings while the service runs, without a password prompt.
 * The socket is created root-owned, mode 0660, group admin (macOS) / sudo, admin or wheel (Linux); every
 * connection's peer credentials are checked again: root, or a member of that group who on macOS must also be
 * the user logged in at the screen. One line in ("limit 80"), one line out ("ok ..." / "err ..."); only the
 * requests below, every value validated; each request is logged. */
static const char *CTL_GROUPS[] = {"admin", "sudo", "wheel"};

#ifdef OS_LINUX
/* static binary: no NSS (getpwuid / getgrnam need glibc's shared libraries) - read the local files directly */
static bool lx_user(uid_t uid, char *name, size_t nn, gid_t *gid) {
    FILE *f = fopen("/etc/passwd", "r");
    char line[512];
    bool found = false;
    while (f && !found && fgets(line, sizeof line, f)) {
        char *fld[4] = {0}, *p = line;
        for (int i = 0; i < 4 && p; i++) { fld[i] = p; p = strchr(p, ':'); if (p) *p++ = 0; }
        if (fld[3] && (uid_t)atol(fld[2]) == uid) { snprintf(name, nn, "%s", fld[0]); *gid = (gid_t)atol(fld[3]); found = true; }
    }
    if (f) fclose(f);
    return found;
}
/* gid of group `grp` (-1 if missing); *member: user is listed in it or has it as primary group */
static long lx_group(const char *grp, const char *user, gid_t primary, bool *member) {
    FILE *f = fopen("/etc/group", "r");
    char line[2048];
    long gid = -1;
    *member = false;
    while (f && gid < 0 && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        char *fld[4] = {0}, *p = line;
        for (int i = 0; i < 4 && p; i++) { fld[i] = p; p = (i < 3) ? strchr(p, ':') : NULL; if (p) *p++ = 0; }
        if (!fld[2] || strcmp(fld[0], grp)) continue;
        gid = atol(fld[2]);
        if (user && (gid_t)gid == primary) *member = true;
        for (char *u = fld[3] ? strtok(fld[3], ",") : NULL; u && user; u = strtok(NULL, ",")) if (!strcmp(u, user)) *member = true;
    }
    if (f) fclose(f);
    return gid;
}
#endif

static gid_t ctl_gid(void) {
    for (size_t i = 0; i < sizeof CTL_GROUPS / sizeof CTL_GROUPS[0]; i++) {
#ifdef OS_LINUX
        bool m;
        long g = lx_group(CTL_GROUPS[i], NULL, 0, &m);
        if (g >= 0) return (gid_t)g;
#else
        struct group *g = getgrnam(CTL_GROUPS[i]);
        if (g) return g->gr_gid;
#endif
    }
    return 0;
}

static bool ctl_peer_ok(int fd, char *who, size_t wn) {
    uid_t uid;
#ifdef OS_LINUX
    struct ucred cr;
    socklen_t l = sizeof cr;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &l) != 0) return false;
    uid = cr.uid;
    char name[64];
    gid_t prim;
    if (!lx_user(uid, name, sizeof name, &prim)) { snprintf(who, wn, "uid %u", (unsigned)uid); return uid == 0; }
    snprintf(who, wn, "%s", name);
    if (uid == 0) return true;
    for (size_t k = 0; k < sizeof CTL_GROUPS / sizeof CTL_GROUPS[0]; k++) {
        bool m;
        if (lx_group(CTL_GROUPS[k], name, prim, &m) >= 0 && m) return true;
    }
    return false;
#else
    gid_t pg;
    if (getpeereid(fd, &uid, &pg) != 0) return false;
    struct passwd *pw = getpwuid(uid);
    snprintf(who, wn, "%s", pw ? pw->pw_name : "?");
    if (uid == 0) return true;
    if (!pw) return false;
    struct stat st;
    if (stat("/dev/console", &st) != 0 || st.st_uid != uid) return false; /* only the user at the screen */
    int groups[64];
    int ng = 64;
    if (getgrouplist(pw->pw_name, pw->pw_gid, groups, &ng) < 0) ng = 64;
    for (int i = 0; i < ng; i++)
        for (size_t k = 0; k < sizeof CTL_GROUPS / sizeof CTL_GROUPS[0]; k++) {
            struct group *gr = getgrnam(CTL_GROUPS[k]);
            if (gr && (gid_t)groups[i] == gr->gr_gid) return true;
        }
    return false;
#endif
}

static int ctl_open(void) {
    unlink(CTL_PATH);
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", CTL_PATH);
    mode_t old = umask(0177); /* created 0600 root: nobody else can connect before the chown/chmod */
    int r = bind(s, (struct sockaddr *)&a, sizeof a);
    umask(old);
    if (r != 0 || chown(CTL_PATH, 0, ctl_gid()) != 0 || chmod(CTL_PATH, 0660) != 0 || listen(s, 4) != 0) {
        logmsg("control socket unavailable: %s", strerror(errno));
        close(s);
        unlink(CTL_PATH);
        return -1;
    }
    fcntl(s, F_SETFD, FD_CLOEXEC);
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
    return s;
}

static bool on_off(const char *v) { return v && (!strcmp(v, "on") || !strcmp(v, "off")); }

static bool ctl_exec(char *line, char *m, size_t n) {
    char *w[5] = {0};
    int k = 0;
    for (char *t = strtok(line, " \t\r\n"); t && k < 5; t = strtok(NULL, " \t\r\n")) w[k++] = t;
    bool force = k && !strcmp(w[0], "force");
    if (force) { for (int i = 0; i < 4; i++) w[i] = w[i + 1]; k--; }
    if (k <= 0) { snprintf(m, n, "empty request"); return false; }
    bool saved = g_force, ok = false;
    g_force = g_force || force;
    if (!strcmp(w[0], "ping") && k == 1) { snprintf(m, n, "macbat %s", MACBAT_VERSION); ok = true; }
    else if (!strcmp(w[0], "limit") && k == 2) {
        char *e;
        long v = strtol(w[1], &e, 10);
        if (*e || e == w[1] || v < 20 || v > 100) snprintf(m, n, "limit must be 20-100");
        else ok = s_limit(w[1], m, n);
    } else if (!strcmp(w[0], "discharge") && k == 2 && on_off(w[1])) ok = s_dis(w[1], m, n);
    else if (!strcmp(w[0], "topup") && k == 2 && on_off(w[1])) ok = s_topup(w[1], m, n);
    else if (!strcmp(w[0], "restore") && k == 1) { fans_release(); ok = s_restore("run", m, n); }
    else if (!strcmp(w[0], "fan") && k == 2 && !strcmp(w[1], "auto")) ok = fan_apply("auto", m, n);
    else if (!strcmp(w[0], "fan") && k == 3 && !strcmp(w[1], "set") && strcmp(w[2], "auto") && fan_spec_ok(w[2])) ok = fan_apply(w[2], m, n);
#ifdef OS_MAC
    else if (!strcmp(w[0], "lowpower") && k == 2 && on_off(w[1])) ok = pm_set("lowpowermode", !strcmp(w[1], "on") ? "1" : "0", m, n, "low power mode");
    else if (!strcmp(w[0], "powermode") && k == 2 && (!strcmp(w[1], "0") || !strcmp(w[1], "1") || !strcmp(w[1], "2")))
        ok = pm_set("powermode", w[1], m, n, "energy mode");
#endif
    else snprintf(m, n, "unknown request");
    g_force = saved;
    if (ok) g_kick = 1;
    return ok;
}

static void ctl_serve(int lfd) {
    for (;;) {
        int c = accept(lfd, NULL, NULL);
        if (c < 0) return;
        fcntl(c, F_SETFD, FD_CLOEXEC);
        fcntl(c, F_SETFL, fcntl(c, F_GETFL) & ~O_NONBLOCK); /* BSD: accepted sockets inherit O_NONBLOCK */
        struct timeval tv = {2, 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        char buf[160] = "", msg[256] = "", who[64] = "?", req[160] = "";
        bool ok = false;
        if (!ctl_peer_ok(c, who, sizeof who)) {
#ifdef OS_MAC
            snprintf(msg, sizeof msg, "not allowed - only root or an admin logged in at the screen");
#else
            snprintf(msg, sizeof msg, "not allowed - only root or members of admin / sudo / wheel");
#endif
            logmsg("control: refused %s", who);
        } else {
            size_t got = 0;
            while (got < sizeof buf - 1 && !memchr(buf, '\n', got)) {
                ssize_t r = read(c, buf + got, sizeof buf - 1 - got);
                if (r <= 0) break;
                got += (size_t)r;
            }
            buf[got] = 0;
            snprintf(req, sizeof req, "%s", buf);
            req[strcspn(req, "\r\n")] = 0;
            if (!memchr(buf, '\n', got)) snprintf(msg, sizeof msg, "incomplete request");
            else ok = ctl_exec(buf, msg, sizeof msg);
            logmsg("control: %s: '%s' -> %s%s%s", who, req, ok ? "ok" : "refused", ok ? "" : ": ", ok ? "" : msg);
        }
        for (char *p = msg; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
        char out[300];
        snprintf(out, sizeof out, "%s %s\n", ok ? "ok" : "err", msg);
        if (write(c, out, strlen(out)) < 0) { /* client gone: nothing to do */ }
        close(c);
    }
}

#ifdef OS_MAC
static void ctl_cf_cb(CFFileDescriptorRef f, CFOptionFlags fl, void *info) {
    (void)fl; (void)info;
    ctl_serve(CFFileDescriptorGetNativeDescriptor(f));
    CFFileDescriptorEnableCallBacks(f, kCFFileDescriptorReadCallBack); /* callbacks are one-shot */
    CFRunLoopStop(CFRunLoopGetCurrent()); /* let the service loop apply the change right away */
}
#endif


/* ------------------------------------------------------------------ settings screen (TUI) */
#include <termios.h>
#include <sys/select.h>
static struct termios g_tio;
static bool g_raw = false;
static void tui_restore(void) {
    if (g_raw) { tcsetattr(0, TCSAFLUSH, &g_tio); g_raw = false; fputs("\033[0m\033[?25h\033[H\033[2J", stdout); fflush(stdout); }
}
static void tui_sig(int s) { tui_restore(); signal(s, SIG_DFL); raise(s); }
enum { KEY_UP_ = 1000, KEY_DOWN_, KEY_LEFT_, KEY_RIGHT_ };

static int tui_key(int ms) {
    fd_set fs;
    FD_ZERO(&fs);
    FD_SET(0, &fs);
    struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
    if (select(1, &fs, NULL, NULL, &tv) <= 0) return 0;
    unsigned char c;
    if (read(0, &c, 1) != 1) return 'q';
    if (c != 27) return c;
    unsigned char s[2];
    FD_ZERO(&fs);
    FD_SET(0, &fs);
    struct timeval t2 = {0, 60000};
    if (select(1, &fs, NULL, NULL, &t2) <= 0 || read(0, &s[0], 1) != 1) return 27;
    if (s[0] != '[' && s[0] != 'O') return 27;
    if (read(0, &s[1], 1) != 1) return 27;
    switch (s[1]) {
    case 'A': return KEY_UP_;
    case 'B': return KEY_DOWN_;
    case 'C': return KEY_RIGHT_;
    case 'D': return KEY_LEFT_;
    }
    return 0;
}

static int setting_opts(const setting_t *st, char (*o)[32], int max) {
    if (st->dyn) return st->dyn(o, max);
    int k = 0;
    while (k < max && k < 10 && st->opts[k]) { snprintf(o[k], 32, "%s", st->opts[k]); k++; }
    return k;
}

static void value_text(const setting_t *st, const char *tok, bool ok, char *out, size_t n) {
    if (!ok) { snprintf(out, n, "N/A"); return; }
    if (st->kind == K_ACTION) { snprintf(out, n, "[ ENTER ]"); return; }
    if (st->kind == K_NUM) { snprintf(out, n, "%s%%", tok); return; }
    for (int i = 0; i < 10 && st->opts[i]; i++)
        if (!strcmp(st->opts[i], tok) && st->names[i]) { snprintf(out, n, "%s", st->names[i]); return; }
    if (st->names[0] && atoi(tok) > 0 && isdigit((unsigned char)tok[0])) {
        int sec = atoi(tok);
        if (sec % 60 == 0 && st->opts[1] && atoi(st->opts[1]) >= 60) snprintf(out, n, "%d MIN", sec / 60);
        else snprintf(out, n, "%s", tok);
        return;
    }
    snprintf(out, n, "%s", tok);
    upcase(out);
    for (char *p = out; *p; p++) if (*p == '_') *p = ' ';
}

static void change(const setting_t *st, int dir, char *msg, size_t mn, bool *ok) {
    char cur[64] = "";
    bool have = st->get(cur, sizeof cur);
    if (st->kind == K_ACTION) { *ok = st->set("run", msg, mn); return; }
    if (st->kind == K_NUM) {
        int v = have ? atoi(cur) : st->lo;
        v = (v / st->step) * st->step + dir * st->step;
        v = clampi(v, st->lo, st->hi);
        char t[16];
        snprintf(t, sizeof t, "%d", v);
        *ok = st->set(t, msg, mn);
        return;
    }
    char o[16][32];
    int k = setting_opts(st, o, 16), idx = -1;
    for (int i = 0; i < k; i++) if (!strcmp(o[i], cur)) idx = i;
    if (k == 0) { snprintf(msg, mn, "no options available"); *ok = false; return; }
    idx = idx < 0 ? (dir > 0 ? 0 : k - 1) : (idx + dir + k) % k;
    *ok = st->set(o[idx], msg, mn);
}

/* the 3-row battery block at the top of the settings screen (and of 'macbat theme --list') */
static void tui_gauge(const theme_t *Tp, bool hb, const battery_t *bp, const conf_t *cfg, bool have, const caps_t *cpp,
                      const control_t *sp) {
    const theme_t T = *Tp;
    battery_t b = *bp;
    conf_t c = *cfg;
    caps_t cp = *cpp;
    control_t s = *sp;
    char l[4096];
    if (!T.retro) {
        const int BW = RW - 4;
        int pct = hb && b.percent >= 0 ? b.percent : 0;
        int mark = c.limit < 100 ? clampi((int)iround(c.limit / 100.0 * BW), 0, BW - 1) : -1;
        const char *lvl = lvl_ink(&T, pct);
        char mb[96], md[96], e[256] = "", t[48], r[512], bar[3072];
        snprintf(md, sizeof md, "%s", hb ? mode_text(&c, &b, have ? &cp : NULL, have ? &s : NULL, mb, sizeof mb) : "?");
        md[strcspn(md, "(")] = 0;
        if (*md) md[0] = a_up(md[0]);
        if (hb && b.has_v) { snprintf(t, sizeof t, "%.2f V  ", b.voltage); strcat(e, t); }
        if (hb && b.has_i) { snprintf(t, sizeof t, "%+.2f A  ", b.current); strcat(e, t); }
        if (hb && b.has_p) { snprintf(t, sizeof t, "%+.1f W  ", b.power); strcat(e, t); }
        if (hb && b.has_t) { snprintf(t, sizeof t, "%.0f °C", b.temp_c); strcat(e, t); }
        snprintf(l, sizeof l, " %s%d%%%s  %s%s%s", lvl, pct, T.reset, T.hi, md, T.reset);
        snprintf(r, sizeof r, "%s%s%s ", T.dim, e, T.reset);
        r_split(&T, l, r);
        m_bar(&T, pct / 100.0, mark, BW, lvl, pct >= 20, bar, sizeof bar);
        snprintf(l, sizeof l, " %s", bar);
        r_line(&T, l);
        const char *svc = service_state();
        char hs[64];
        if (hb && b.health_unreliable) snprintf(hs, sizeof hs, "health n/a");
        else if (hb && b.health >= 0) snprintf(hs, sizeof hs, "health %.0f%%", b.health);
        else snprintf(hs, sizeof hs, "health ?");
        snprintf(l, sizeof l, " %s● %s%s %s· %s%s", !strcmp(svc, "active") ? T.ok : T.bad,
                 !strcmp(svc, "active") ? "service on" : "service off", T.reset, T.dim, hs, T.reset);
        snprintf(r, sizeof r, "%slimit %d%%%s ", T.dim, c.limit, T.reset);
        r_split(&T, l, r);
        return;
    }
    /* compact gauge + live readings */
    const int GW = 20;
    int pct = hb && b.percent >= 0 ? b.percent : 0;
    int fill = (int)iround(pct / 100.0 * GW), mark = c.limit < 100 ? (int)iround(c.limit / 100.0 * GW) : -1;
    if (mark >= GW) mark = GW - 1;
    const char *lvl = pct >= 50 ? T.ok : pct >= 20 ? T.hi : T.bad;
    char mb[96];
    const char *mode = hb ? mode_text(&c, &b, have ? &cp : NULL, have ? &s : NULL, mb, sizeof mb) : "?";
    for (int row = 0; row < 3; row++) {
        char g[2048];
        snprintf(g, sizeof g, "%s%s", T.frame, row == 0 ? "┌" : row == 1 ? "│" : "└");
        for (int i = 0; i < GW; i++) {
            if (row == 1) {
                strcat(g, i < fill ? ((mark >= 0 && i >= mark) ? T.hi : lvl) : T.dim);
                strcat(g, i < fill ? "█" : "░");
                strcat(g, T.frame);
            } else strcat(g, i == mark ? (row == 0 ? "┬" : "┴") : "─");
        }
        strcat(g, row == 0 ? "┐ " : row == 1 ? "│█" : "┘ ");
        strcat(g, T.reset);
        char info[512] = "";
        if (row == 0) {
            char md[96];
            snprintf(md, sizeof md, "%s", mode);
            md[strcspn(md, "(")] = 0; /* compact header: drop the explanation in parentheses */
            upcase(md);
            snprintf(info, sizeof info, "%s%d%%%s  %s%s%s", lvl, pct, T.reset, T.hi, md, T.reset);
        } else if (row == 1) {
            char e[256] = "", t[48];
            if (hb && b.has_v) { snprintf(t, sizeof t, "%.2fV ", b.voltage); strcat(e, t); }
            if (hb && b.has_i) { snprintf(t, sizeof t, "%+.2fA ", b.current); strcat(e, t); }
            if (hb && b.has_p) { snprintf(t, sizeof t, "%+.1fW ", b.power); strcat(e, t); }
            if (hb && b.has_t) { snprintf(t, sizeof t, "%.0f°C", b.temp_c); strcat(e, t); }
            snprintf(info, sizeof info, "%s%s%s", T.value, e, T.reset);
        } else {
            const char *svc = service_state();
            char hs[64];
            if (hb && b.health_unreliable) snprintf(hs, sizeof hs, "HEALTH N/A");
            else if (hb && b.health >= 0) snprintf(hs, sizeof hs, "HEALTH %.0f%%", b.health);
            else snprintf(hs, sizeof hs, "HEALTH ?");
            snprintf(info, sizeof info, "%s● %s%s %s· %s", !strcmp(svc, "active") ? T.ok : T.bad,
                     !strcmp(svc, "active") ? "SERVICE ON" : "SERVICE OFF", T.reset, T.dim, hs);
        }
        snprintf(l, sizeof l, " %s  %s", g, info);
        r_line(&T, l);
    }
}

static int cmd_tui(char **argv) {
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) != 0 || ws.ws_col < RW + 2 || ws.ws_row < 20 || !utf8_out()) {
        cmd_status(false);
        fprintf(stderr, "(settings screen needs a UTF-8 terminal of at least %dx20)\n", RW + 2);
        return 0;
    }
    int vis[NSET], nv = 0;
    for (int i = 0; i < NSET; i++) if (SETTINGS[i].avail()) vis[nv++] = i;
    bool root = geteuid() == 0;
    tcgetattr(0, &g_tio);
    struct termios raw = g_tio;
    raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSAFLUSH, &raw);
    g_raw = true;
    atexit(tui_restore);
    signal(SIGINT, tui_sig);
    signal(SIGTERM, tui_sig);
    fputs("\033[?25l\033[H\033[2J", stdout);
    int sel = 0, top = 0;
    char msg[256];
    bool msg_ok = true;
    snprintf(msg, sizeof msg, "%s", root ? "ready - changes apply immediately" : "read-only: press S to relaunch with sudo");
    msg_ok = root;
    for (;;) {
        theme_t T = theme(); /* every frame: the Theme row previews live */
        ioctl(1, TIOCGWINSZ, &ws);
        int rows = ws.ws_row, viewport = rows - 12;
        if (viewport < 4) viewport = 4;
        battery_t b;
        bool hb = read_battery(&b);
        conf_t c = load_conf();
        caps_t cp;
        control_t s = {-1, -1, -1};
        long age;
        bool have = get_caps(&cp) ? (s = get_control(&cp), true) : read_state(&cp, &s, &age);
        char model[64];
        model_name(model, sizeof model);
        fputs("\033[H", stdout);
        r_top(&T);
        char l[4096], r[256];
        if (T.retro) snprintf(l, sizeof l, "%s█▒░ M A C B A T ░▒█%s  %sSETTINGS%s", T.title, T.reset, T.label, T.reset);
        else snprintf(l, sizeof l, "%smacbat%s %ssettings%s", T.title, T.reset, T.dim, T.reset);
        snprintf(r, sizeof r, "%s%s%s", root ? T.dim : T.warn, root ? model : T.retro ? "READ-ONLY" : "read-only", T.reset);
        r_split(&T, l, r);
        r_sep(&T);
        tui_gauge(&T, hb, &b, &c, have, &cp, &s);
        r_sep2(&T);
        /* list with section headers */
        int lines[64], kind[64], nl = 0, selline = 0;
        const char *last = NULL;
        for (int i = 0; i < nv && nl < 62; i++) {
            const setting_t *st = &SETTINGS[vis[i]];
            if (!last || strcmp(last, st->section)) { lines[nl] = vis[i]; kind[nl++] = 1; last = st->section; }
            if (i == sel) selline = nl;
            lines[nl] = vis[i];
            kind[nl++] = 0;
        }
        if (selline < top) top = selline > 0 && kind[selline - 1] == 1 ? selline - 1 : selline;
        if (selline >= top + viewport) top = selline - viewport + 1;
        for (int k = 0; k < viewport; k++) {
            int li = top + k;
            if (li >= nl) { r_line(&T, ""); continue; }
            const setting_t *st = &SETTINGS[lines[li]];
            if (kind[li] == 1) {
                char sec[32];
                snprintf(sec, sizeof sec, "%s", st->section);
                if (T.retro) snprintf(l, sizeof l, "%s── %s %s", T.dim, sec, T.reset);
                else { title_case(sec); snprintf(l, sizeof l, "%s%s%s", T.title, sec, T.reset); }
                r_line(&T, l);
                continue;
            }
            char tok[64] = "", val[64];
            if (!st->show || !st->show(val, sizeof val)) {
                bool ok = st->get(tok, sizeof tok);
                value_text(st, tok, ok, val, sizeof val);
            }
            char dots[80] = "";
            int dl = 30 - (int)strlen(st->label);
            for (int d = 0; d < dl; d++) strcat(dots, T.retro ? "·" : " ");
            bool on = li == selline;
            if (!T.retro) soft_case(val);
            if (on)
                snprintf(l, sizeof l, "%s%s %s %s%s %s%s%s %s %s%s%s", T.hi, T.retro ? "▸" : "›", st->label, T.dim, dots, T.hi,
                         T.retro ? "«" : "‹", "\033[7m", val, "\033[27m", T.retro ? "»" : "›", T.reset);
            else
                snprintf(l, sizeof l, "  %s%s %s%s %s  %s%s", T.label, st->label, T.dim, dots, T.value, val, T.reset);
            r_line(&T, l);
        }
        r_sep2(&T);
        snprintf(l, sizeof l, "%s%s%s", msg_ok ? T.ok : T.warn, msg, T.reset);
        r_line(&T, l);
        snprintf(l, sizeof l, "%s↑↓ select  ←→ change  ENTER apply/run  %sQ quit%s", T.dim,
                 root ? "R refresh  " : "S sudo  ", T.reset);
        r_line(&T, l);
        r_bot(&T);
        fputs("\033[J", stdout);
        fflush(stdout);

        int key = tui_key(2000);
        if (!key) continue;
        if (key == 'q' || key == 'Q' || key == 27) break;
        if (key == KEY_UP_ || key == 'k') sel = (sel + nv - 1) % nv;
        else if (key == KEY_DOWN_ || key == 'j' || key == '\t') sel = (sel + 1) % nv;
        else if ((key == 's' || key == 'S') && !root) {
            tui_restore();
            char me[1024];
            if (self_path(me, sizeof me)) execlp("sudo", "sudo", me, (char *)NULL);
            (void)argv;
            die("cannot relaunch with sudo");
        } else if (key == KEY_LEFT_ || key == KEY_RIGHT_ || key == 'h' || key == 'l' || key == '-' || key == '+' ||
                   key == '\r' || key == '\n' || key == ' ') {
            int dir = (key == KEY_LEFT_ || key == 'h' || key == '-') ? -1 : 1;
            const setting_t *st = &SETTINGS[vis[sel]];
            if (!root && st->set == s_theme) { /* themes are only looks: preview them without sudo */
                char o[16][32], cur[32];
                int k = setting_opts(st, o, 16), idx = 0;
                st->get(cur, sizeof cur);
                for (int i = 0; i < k; i++) if (!strcmp(o[i], cur)) idx = i;
                g_th = find_theme(o[(idx + dir + k) % k]);
                snprintf(g_th_src, sizeof g_th_src, "preview");
                snprintf(msg, sizeof msg, "theme %s - preview only, sudo saves it", g_th->name);
                msg_ok = true;
                continue;
            }
            if (!root) { snprintf(msg, sizeof msg, "read-only: press S to relaunch with sudo"); msg_ok = false; continue; }
            if (st->kind == K_ACTION && !(key == '\r' || key == '\n' || key == ' ')) continue;
            change(st, dir, msg, sizeof msg, &msg_ok);
        }
    }
    tui_restore();
    return 0;
}

static int cmd_topup(const char *v) {
    if (v && !on_off(v)) die("usage: sudo macbat topup [on|off]   (on: charge to 100%% once, then back to the limit)");
    need_root("topup");
    refuse_if_conflict("topup");
    char m[256];
    bool ok = s_topup(v ? v : "on", m, sizeof m);
    printf("%s\n", m);
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------ themes command */
static int cmd_theme(const char *a) {
    char names[128];
    theme_names(names, sizeof names);
    if (!g_th) resolve_theme(NULL);
    if (!a) {
        printf("theme: %s (%s)\n\n", g_th->name, g_th_src);
        for (int i = 0; i < NTHEMES; i++)
            printf("  %s %-7s %s%s\n", &THEMES[i] == g_th ? "*" : " ", THEMES[i].name, THEMES[i].desc, i ? "" : " (default)");
        printf("\npreview all:  macbat theme --list\nsave:         sudo macbat theme NAME   (or the settings screen)\n"
               "just once:    macbat --theme NAME status   ·   MACBAT_THEME=NAME   ·   NO_COLOR=1 drops colour\n");
        return 0;
    }
    if (!strcmp(a, "--list") || !strcmp(a, "list") || !strcmp(a, "-l")) {
        if (!use_boxed()) {
            for (int i = 0; i < NTHEMES; i++) printf("%-7s %s\n", THEMES[i].name, THEMES[i].desc);
            return 0;
        }
        battery_t b;
        bool hb = read_battery(&b);
        conf_t c = load_conf();
        caps_t cp;
        control_t s = {-1, -1, -1};
        long age;
        bool have = get_caps(&cp) ? (s = get_control(&cp), true) : read_state(&cp, &s, &age);
        const theme_def_t *cur = g_th;
        for (int i = 0; i < NTHEMES; i++) {
            g_th = &THEMES[i];
            theme_t T = theme();
            char cap[96];
            snprintf(cap, sizeof cap, "%s%s%s", THEMES[i].name, i ? "" : " · default", &THEMES[i] == cur ? " · current" : "");
            r_top_title(&T, cap);
            tui_gauge(&T, hb, &b, &c, have, &cp, &s);
            r_bot(&T);
        }
        g_th = cur;
        printf("save one: sudo macbat theme NAME   ·   try one: macbat --theme NAME status\n");
        return 0;
    }
    if (!strcmp(a, "default") || !strcmp(a, "reset")) {
        need_root("theme");
        conf_t c = load_conf();
        c.theme[0] = 0;
        save_conf(c);
        printf("theme: back to the default (%s)\n", THEMES[0].name);
        return 0;
    }
    const theme_def_t *d = find_theme(a);
    if (!d) die("unknown theme '%s' - themes: %s (preview: macbat theme --list)", a, names);
    need_root("theme");
    conf_t c = load_conf();
    snprintf(c.theme, sizeof c.theme, "%s", d->name);
    save_conf(c);
    printf("theme: %s (saved in %s)\n", d->name, CONF_PATH);
    return 0;
}

/* ------------------------------------------------------------------ main */
static void usage(void) {
    puts("macbat " MACBAT_VERSION " - battery charge limiter & status for Apple laptops\n"
         "\n"
         "usage:\n"
         "  macbat                        interactive settings screen (battery, fans, display, sleep, disk, power saving)\n"
         "  macbat status [--json|--plain] battery and charge-limit status\n"
         "  sudo macbat limit [20-100|off] show / set the charge limit\n"
         "  sudo macbat discharge [on|off] auto-discharge (bypass the adapter) while above the limit\n"
         "  sudo macbat restore           resume charging, re-enable the adapter, fans to automatic - now\n"
         "  sudo macbat topup [off]       charge to 100% once, then back to the limit (off: cancel)\n"
         "  macbat fan [--json|--plain]   fan speeds, limits and mode\n"
         "  sudo macbat fan set <rpm|NN%> fixed fan speed, clamped to the fan's min..max (0% = min, 100% = max)\n"
         "  sudo macbat fan auto          automatic fan control again (re-enables mbpfan if macbat paused it)\n"
         "  sudo macbat install           install + start the background service (systemd / launchd)\n"
         "  sudo macbat uninstall         remove the service, restore normal charging (100%)\n"
         "  macbat theme [NAME|--list]    show / preview (--list) / save (sudo) the look: modern, mono, ocean,\n"
         "                                candy, nord, retro, amber\n"
         "  macbat keys                   show the relevant SMC keys (diagnostics)\n"
         "  macbat --version\n"
         "\n"
         "options: --theme NAME, --force (ignore other charge managers such as AlDente), --quiet, --plain\n"
         "env:     MACBAT_THEME=NAME, NO_COLOR=1, MACBAT_COLORS=16|256|truecolor (default: from TERM / COLORTERM)\n"
         "config:  " CONF_PATH "  (limit, auto_discharge, hysteresis, interval, fan, theme)\n"
         "https://github.com/neuralfoundry-coder/macbat");
}

int main(int argc, char **argv) {
    setlocale(LC_CTYPE, "");
    const char *pos[3] = {NULL, NULL, NULL};
    int np = 0;
    bool json = false;
    const char *theme_flag = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) json = true;
        else if (!strcmp(argv[i], "--theme") && i + 1 < argc) theme_flag = argv[++i];
        else if (!strncmp(argv[i], "--theme=", 8)) theme_flag = argv[i] + 8;
        else if (!strcmp(argv[i], "--force")) g_force = true;
        else if (!strcmp(argv[i], "--quiet")) g_quiet = true;
        else if (!strcmp(argv[i], "--plain")) g_plain = true;
        else if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-V")) { puts("macbat " MACBAT_VERSION); return 0; }
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h") || !strcmp(argv[i], "help")) { usage(); return 0; }
        else if (np < 3) pos[np++] = argv[i];
    }
    if (theme_flag) resolve_theme(theme_flag); /* unknown names stop here, with the list */
    if (!smc_available()) die("applesmc not found - this does not look like an Intel Mac (try: sudo modprobe applesmc)");
    if (!np && !json && !g_plain && isatty(0) && isatty(1)) return cmd_tui(argv);
    const char *cmd = np ? pos[0] : "status";
    if (!strcmp(cmd, "status")) return cmd_status(json);
    if (!strcmp(cmd, "limit")) return cmd_limit(pos[1]);
    if (!strcmp(cmd, "discharge")) return cmd_discharge(pos[1]);
    if (!strcmp(cmd, "restore")) return cmd_restore();
    if (!strcmp(cmd, "install")) return cmd_install();
    if (!strcmp(cmd, "uninstall")) return cmd_uninstall();
    if (!strcmp(cmd, "keys")) return cmd_keys();
    if (!strcmp(cmd, "fan") || !strcmp(cmd, "fans")) return cmd_fan(pos[1], pos[2], json);
    if (!strcmp(cmd, "theme") || !strcmp(cmd, "themes")) return cmd_theme(pos[1]);
    if (!strcmp(cmd, "topup")) return cmd_topup(pos[1]);
    if (!strcmp(cmd, "daemon")) return cmd_daemon();
    usage();
    return 2;
}
