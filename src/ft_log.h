#ifndef FT_LOG_H
#define FT_LOG_H

#include <syslog.h>

#define FT_LOG_EMERG LOG_EMERG     /* 0 system is unusable */
#define FT_LOG_ALERT LOG_ALERT     /* 1 action must be taken immediately */
#define FT_LOG_CRIT LOG_CRIT       /* 2 critical conditions */
#define FT_LOG_ERR LOG_ERR         /* 3 error conditions */
#define FT_LOG_WARNING LOG_WARNING /* 4 warning conditions */
#define FT_LOG_NOTICE LOG_NOTICE   /* 5 normal but significant condition */
#define FT_LOG_INFO LOG_INFO       /* 6 informational */
#define FT_LOG_DEBUG LOG_DEBUG     /* 7 debug-level messages */

/* initialize the connection with a file descriptor. If identity or logfile
 * aren't provided, default values are assumed.
 * identity must be a malloc'd address, then not freed by the user. Whereas
 * logfile must be string literal */
int ft_openlog(char *identity, const char *logfile);

/* Logs at this format: <time><identity><level>:<user_format> to the file given
 * in ft_openlog() by the user or to <identity>.log.
 * Main API function. ft_log() can try to initialize itself but it is better
 * practice to call once ft_openlog() before.
 * 'level' macros to use can be found in ft_log.h */
void ft_log(int level, const char *format, ...);

#endif
