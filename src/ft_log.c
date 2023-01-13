#include "ft_log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FT_LOGLVL_NB (FT_LOG_DEBUG + 1)

#define BUF_LOG_LEN (512)
#define FT_LOGFILE_PERM (0644)
#define FT_LOGFILE_FLAGS (O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC)
#define DFL_PGM_NAME "unknown"

static char *ident;
static int log_fd;
static char log_filename[128];

/* atexit() callback, cleanup before exit */
static void ft_log_exit() {
    if (ident) free(ident);
    if (log_fd) close(log_fd);
}

static void init_logfile() {
    if (*log_filename) return; /* don't init twice */
    strncat(log_filename, ident, sizeof(log_filename) - 1);
    strncat(log_filename, ".log", sizeof(log_filename) - 1);
}

/* init once ident and log_filename. Used only if no identity was provided by
 * the user before using ft_log(), so the lib try to identity by itself or
 * setting default values. */
static int init_ident() {
    if (ident) return EXIT_SUCCESS;
#ifdef __GNUC__
    ident = strdup(program_invocation_short_name);
#else
    ident = strdup(DFL_PGM_NAME);
#endif
    if (!ident) return EXIT_FAILURE;
    init_logfile();
    atexit(ft_log_exit);
    return EXIT_SUCCESS;
}

int ft_openlog(char *identity, const char *logfile) {
    if (ident) return 0; /* don't init & open twice */
    if (logfile) strncpy(log_filename, logfile, sizeof(log_filename) - 1);
    if (!identity) {
        if (!init_ident()) return 1;
    } else {
        ident = strdup(basename(identity));
        if (!ident) return 1;
        init_logfile();
        atexit(ft_log_exit);
    }

    log_fd = open(log_filename, FT_LOGFILE_FLAGS, FT_LOGFILE_PERM);
    if (log_fd == -1) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}

void ft_log(int level, const char *format, ...) {
    time_t curtime;
    struct tm loctime;
    size_t len;
    va_list args;
    char buf[BUF_LOG_LEN] = {0};
    const char log_lvl[FT_LOGLVL_NB][32] = {"[EMERG]", "[ALERT]",   "[CRIT]",
                                            "[ERR]",   "[WARNING]", "[NOTICE]",
                                            "[INFO]",  "[DEBUG]"};

    if (!ident)
        if (ft_openlog(NULL, NULL)) return;
    if (log_fd == 0 || log_fd == -1) return;
    if (level < 0 || level >= FT_LOGLVL_NB) level = FT_LOG_INFO;
    curtime = time(NULL);
    if (localtime_r(&curtime, &loctime) != &loctime) return;
    len = strftime(buf, sizeof(buf), "%F, %T ", &loctime);
    len += snprintf(buf + len, sizeof(buf) - len, "%s %s: ", ident,
                    log_lvl[level]);
    va_start(args, format);
    len += vsnprintf(buf + len, sizeof(buf) - len, format, args);
    va_end(args);
    len += snprintf(buf + len, sizeof(buf) - len, "\n");
    write(log_fd, buf, len);
}
