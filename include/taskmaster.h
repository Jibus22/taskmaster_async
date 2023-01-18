#ifndef TASKMASTER_H
#define TASKMASTER_H

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TM_LOGFILE "./taskmaster.log"

#define handle_error(msg) \
  do {                    \
    perror(msg);          \
    exit(EXIT_FAILURE);   \
  } while (0)

#define goto_error(msg) \
  do {                  \
    perror(msg);        \
    goto error;         \
  } while (0)

#define DESTROY_PTR(ptr) \
  do {                   \
    if ((ptr)) {         \
      free((ptr));       \
      ptr = NULL;        \
    }                    \
  } while (0)

#define UNUSED_PARAM(a) (void)(a);

#define SIGNAL_BUF_SIZE (32) /* buffer size to store signal name */
typedef struct s_signal {
  char name[SIGNAL_BUF_SIZE]; /* name of signal */
  uint8_t nb;                 /* number corresponding to the signal */
} t_signal;

typedef enum e_autorestart {
  autorestart_false,
  autorestart_true,
  autorestart_unexpected,
  autorestart_max
} t_autorestart;

/* data of a program fetch in config file */
typedef struct s_pgm_usr {
  char *name; /* pgm name */
  char **cmd; /* launch command */
  struct {
    char **array_val; /* environment passed to the processus */
    uint16_t array_size;
  } env;
  char *std_out;    /* which file processus logs out (default /dev/null) */
  char *std_err;    /* which file processus logs err (default /dev/null) */
  char *workingdir; /* working directory of processus */
  struct s_exit_code {
    int16_t *array_val; /* array of expected exit codes */
    uint16_t array_size;
  } exitcodes;
  uint16_t numprocs;         /* number of processus to run */
  mode_t umask;              /* umask of processus (default permissions) */
  t_autorestart autorestart; /* autorestart permissions */
  uint8_t startretries;      /* how many times a processus can restart */
  bool autostart;            /* start at launch of taskmaster or not */
  t_signal stopsignal;       /* which signal to use when using 'stop' command */
  uint32_t starttime;        /* time until it is considered a processus is well
                                launched. in ms*/
  uint32_t stoptime;         /* time allowed to a processus to stop before it is
                              killed. in ms*/
} t_pgm_usr;

typedef struct s_process {
  pid_t pid;           /* processus pid */
  int32_t restart_cnt; /* how many times the processus restarted */
  int32_t w_status;    /* waitpid() status of processus */
  int32_t updated; /* flag to notify wether the proc has been updated or not */
  struct s_process *next;
} t_process;

typedef enum e_pgm_event {
  PGM_NO_EV,
  PGM_EV_RESTART,
  PGM_EV_EXIT,
  PGM_MAX_EV,
} t_pgm_event;

/* data of a program dynamically filled at runtime for taskmaster operations */
typedef struct s_pgm_private {
  struct log {
    int32_t out; /* fd for logging out */
    int32_t err; /* fd for logging err */
  } log;
  pid_t pgid;       /* process group id */
  int32_t updated;  /* notify wether the pgm had been updated or not */
  t_pgm_event ev;   /* event affected to the pgm */
  int32_t proc_cnt; /* count of active processus */
  t_process *proc_head;
  struct s_pgm *next; /* next link of the linked list */
} t_pgm_private;

/* concatenation of all data a pgm need in taskmaster */
typedef struct s_pgm {
  t_pgm_usr usr;
  t_pgm_private privy;
} t_pgm;

typedef enum e_timer_ev {
  NO_TIMER_EV,
  TIMER_EV_START,
  TIMER_EV_STOP,
  MAX_TIMER_EV_NB,
} t_timer_ev;

typedef struct s_timer {
  t_pgm *pgm;   /* pgm concerned by the timer */
  time_t time;  /* time when the timer much trigger */
  int32_t type; /* type of action to achieve (is it timing a start or a stop) */
  struct s_timer *next;
} t_timer;

typedef struct s_tm_node {
  char *tm_name;     /* taskmaster name (argv[0]) */
  FILE *config_file; /* configuration file */
  t_pgm *head;       /* head of list of programs */
  t_timer *timer_hd; /* head of list of timer  */
  uint32_t pgm_nb;   /* number of programs */
  pid_t shell_pgid;  /* shell pgid */
  int32_t exit;      /* exit taskmaster if true */
} t_tm_node;

/* parsing.c */
uint8_t load_config_file(t_tm_node *node);
uint8_t sanitize_config(t_tm_node *node);
uint8_t fulfill_config(t_tm_node *node);

/* run_client.c */
uint8_t run_client(t_tm_node *node);

/* debug.c */
void print_pgm_list(t_pgm *pgm);

/* destroy.c */
void destroy_pgm_list(t_pgm **head);
void destroy_taskmaster(t_tm_node *node);

#endif
