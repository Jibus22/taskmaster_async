#include "run_client.h"

#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>

#include "ft_log.h"
#include "ft_readline.h"

/* Debugging macro. */
#if 1
FILE *rl_debug_fp = NULL;
void close_debugfile(void) {
    if (rl_debug_fp) fclose(rl_debug_fp);
}
#define client_debug(...)                                       \
    do {                                                        \
        if (rl_debug_fp == NULL) {                              \
            rl_debug_fp = fopen("/tmp/client_debug.txt", "a");  \
            atexit(close_debugfile); /* avoid valgrind errors*/ \
        }                                                       \
        fprintf(rl_debug_fp, ", " __VA_ARGS__);                 \
        fflush(rl_debug_fp);                                    \
    } while (0)
#else
#define client_debug(...)
#endif

/* ================================= getters ================================ */

/* initialize & record node in a static pointer at the first call and get its
 * address on subsequent calls. This avoid node to be a global variable */
static t_tm_node *get_node(t_tm_node *node) {
    static bool init = false;
    static t_tm_node *my_node = NULL;

    if (!init) {
        init = true;
        my_node = node;
    }
    return my_node;
}

static t_tm_node *get_newnode(t_tm_node *newnode, bool init) {
    static t_tm_node *my_newnode = NULL;

    if (init) my_newnode = newnode;
    return my_newnode;
}

DECL_CMD_HANDLER(cmd_status);
DECL_CMD_HANDLER(cmd_start);
DECL_CMD_HANDLER(cmd_stop);
DECL_CMD_HANDLER(cmd_restart);
DECL_CMD_HANDLER(cmd_reload);
DECL_CMD_HANDLER(cmd_exit);
DECL_CMD_HANDLER(cmd_help);

/* returns address of taskmaster commands */
static t_tm_cmd *get_commands() {
    static t_tm_cmd command[TM_CMD_NB] = {
        {cmd_status, "status", FREE_NB_ARGS, 0},
        {cmd_start, "start", MANY_ARGS, 0},
        {cmd_stop, "stop", MANY_ARGS, 0},
        {cmd_restart, "restart", MANY_ARGS, 0},
        {cmd_reload, "reload", NO_ARGS, 0},
        {cmd_exit, "exit", NO_ARGS, 0},
        {cmd_help, "help", NO_ARGS, 0}};
    return command;
}

/* =============================== initialization =========================== */

static void log_exit() { ft_log(FT_LOG_INFO, "exited"); }

static void *destroy_str_array(char **array, uint32_t sz) {
    while (--sz >= 0) {
        free(array[sz]);
        array[sz] = NULL;
    }
    return NULL;
}

/* Add taskmaster commands and program names to completion */
static char **get_completion(const t_tm_node *node, const t_tm_cmd *commands,
                             uint32_t compl_nb) {
    uint32_t i = 0;
    t_pgm *pgm = node->head;
    char **completions = malloc(compl_nb * sizeof(*completions));

    if (!completions) return NULL;

    while (i < TM_CMD_NB) {
        completions[i] = strdup(commands[i].name);
        if (!completions[i]) return destroy_str_array(completions, i);
        i++;
    }
    while (i < compl_nb && pgm) {
        if (pgm->privy.ev == PGM_EV_DEL) {
            pgm = pgm->privy.next;
            continue;
        }
        completions[i] = strdup(pgm->usr.name);
        if (!completions[i]) return destroy_str_array(completions, i);
        pgm = pgm->privy.next;
        i++;
    }
    return completions;
}

/* add or reload completions strings to ft_readline */
static void add_cli_completion() {
    char **completion = NULL;
    t_tm_node *node = get_node(NULL);
    t_tm_cmd *command = get_commands();
    int32_t compl_nb = TM_CMD_NB + node->pgm_nb;

    completion = get_completion(node, command, compl_nb);
    if (!completion) goto error;
    if (ft_readline_add_completion(completion, compl_nb)) goto error;
    return;
error:
    ft_log(FT_LOG_ERR, "failed to add completion");
}

/* ========================== user input sanitizer ========================== */

static void err_usr_input(t_tm_node *node, int32_t err) {
    static const char cmd_errors[CMD_ERR_NB][CMD_ERR_BUFSZ] = {
        "\0",
        "empty line",
        "command not found",
        "too many arguments",
        "argument missing",
        "bad argument"};

    err -= CMD_ERR_OFFSET; /* make err code start from 0 instead of being neg */
    fprintf(stderr, "%s: command error: %s\n", node->tm_name, cmd_errors[err]);
}

/* Checks number and validity of arguments according to the command */
static int32_t sanitize_arg(const t_tm_node *node, t_tm_cmd *command,
                            const char *args) {
    t_pgm *pgm;
    int32_t i = 0, arg_len;
    uint32_t match_nb = 0;
    bool found;

    while (args[i] == ' ') i++;
    while (args[i]) {
        found = false;
        if (command->flag == NO_ARGS) return CMD_TOO_MANY_ARGS;

        for (pgm = node->head; pgm && !found; pgm = pgm->privy.next) {
            arg_len = strlen(pgm->usr.name);
            if (!strncmp(args + i, pgm->usr.name, arg_len) &&
                (args[i + arg_len] == ' ' || args[i + arg_len] == 0)) {
                if (match_nb == node->pgm_nb) return CMD_TOO_MANY_ARGS;
                if (!match_nb) command->args = (char *)(args + i);
                match_nb++;
                found = true;
                i += arg_len;
            }
        }

        if (!found) return CMD_BAD_ARG;
        while (args[i] == ' ') i++;
    }

    if (command->flag == MANY_ARGS && !match_nb) return CMD_ARG_MISSING;
    return EXIT_SUCCESS;
}

/* Search for a registered command into line & sanitize its args.
 * Returns index of command */
static int32_t find_cmd(const t_tm_node *node, t_tm_cmd *command,
                        const char *line) {
    int32_t cmd_len, ret;

    if (!line[0]) return CMD_EMPTY_LINE;
    for (int32_t i = 0; i < TM_CMD_NB; i++) {
        cmd_len = strlen(command[i].name);
        if (!strncmp(line, command[i].name, cmd_len) &&
            (line[cmd_len] == ' ' || !line[cmd_len])) {
            ret = sanitize_arg(node, &command[i], line + cmd_len);
            return ((i * (ret >= 0)) + (ret * (ret < 0)));
        }
    }
    return CMD_NOT_FOUND;
}

/* Always append a NULL char at the end of dst, size should be at least
 * stlren(src)+1 */
static size_t strcpy_safe(char *dst, const char *src, size_t size) {
    uint32_t i = 0;

    if (!size) return strlen(src);
    while (src[i] && i + 1 < size) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
    if (!src[i]) return i;
    return strlen(src);
}

/* Remove extra spaces */
static void format_user_input(char *line) {
    int32_t i = 0, space;

    while (line[i] == ' ') i++;
    if (!line[i]) {
        line[0] = 0;
        return;
    }
    if (i) strcpy_safe(line, line + i, strlen(line + i) + 1);

    i = 0;
    while (line[i]) {
        space = 0;
        while (line[i] == ' ') {
            space++;
            i++;
        }
        if (!line[i]) {
            line[i - space] = 0;
            return;
        }
        if (space > 1) {
            strcpy_safe(line + (i - space + 1), line + i, strlen(line + i) + 1);
            i = i - space + 1;
        }
        i += (space == 0);
    }
}

/* ============================== lists processors ========================== */

static void pgm_list_remove(t_tm_node *node, t_pgm *pgm) {
    t_pgm *ptr = node->head, *last = NULL;

    while (ptr && ptr != pgm) {
        last = ptr;
        ptr = ptr->privy.next;
    }
    if (!ptr) return;
    if (last)
        last->privy.next = ptr->privy.next;
    else
        node->head = ptr->privy.next;
}

/* insert pgm_new after pgm_pos */
static void pgm_list_insert_after(t_pgm *pgm_pos, t_pgm *pgm_new) {
    pgm_new->privy.next = pgm_pos->privy.next;
    pgm_pos->privy.next = pgm_new;
}

static void pgm_list_add_front(t_tm_node *node, t_pgm *pgm) {
    pgm->privy.next = node->head;
    node->head = pgm;
}

typedef int32_t (*t_handle)(t_pgm *pgm, void *arg);

/* to each pgm, execute handle callback. handle takes pgm and arg as argument */
static int32_t process_pgm(t_pgm *pgm, t_handle handle, void *arg) {
    int32_t ret = 0;
    t_pgm *next;

    while (pgm) {
        next = pgm->privy.next;
        ret = handle(pgm, arg);
        if (ret) return ret;
        pgm = next;
    }
    return 0;
}

typedef int32_t (*t_proc_handle)(t_pgm *pgm, const void *arg, t_process *last,
                                 t_process **current);
/* process each proc in one pgm. the handle callback is able to update the
 * proc list*/
static int32_t process_proc(t_pgm *pgm, t_proc_handle handle, const void *arg) {
    t_process *proc = pgm->privy.proc_head, *last = NULL, *next = NULL;

    while (proc) {
        next = proc->next;
        if (handle(pgm, arg, last, &proc)) return 1;
        last = proc;
        proc = next;
    }
    return 0;
}

/* ============================= timer primitives =========================== */

static void block_sigalrm(sigset_t *old_sigset) {
    sigset_t block_alarm;

    sigemptyset(&block_alarm);
    sigaddset(&block_alarm, SIGALRM);
    sigprocmask(SIG_BLOCK, &block_alarm, old_sigset);
}

static void unblock_sigalrm(sigset_t *old_sigset) {
    sigset_t block_alarm;

    sigemptyset(&block_alarm);
    sigaddset(&block_alarm, SIGALRM);
    sigprocmask(SIG_UNBLOCK, &block_alarm, NULL);
    sigprocmask(SIG_BLOCK, old_sigset, NULL);
}

/* wrapper to call a timer function inside blocking sigalrm guards */
static void safe_timer_fn_call(t_pgm *pgm, int32_t type,
                               void (*cb)(t_pgm *, int32_t)) {
    sigset_t old_sigset;
    block_sigalrm(&old_sigset);
    cb(pgm, type);
    unblock_sigalrm(&old_sigset);
}

static void set_timer(t_timer *timer);

static void delete_timer(t_timer *timer) {
    t_tm_node *node = get_node(NULL);
    t_timer *tmr = node->timer_hd, *last = NULL;

    while (tmr && tmr != timer) {
        last = tmr;
        tmr = tmr->next;
    }

    if (!tmr) return;
    if (last)
        last->next = tmr->next;
    else {
        node->timer_hd = tmr->next;
        set_timer(timer->next);
    }
    free(timer);
}

/* take arg as t_proc_state type and apply it to *current->state */
static int set_proc_state(t_pgm *pgm, const void *arg, t_process *last,
                          t_process **current) {
    UNUSED_PARAM(pgm);
    UNUSED_PARAM(last);
    t_process *proc = *current;
    t_proc_state *state = (t_proc_state *)arg;
    proc->state = *state;
    return 0;
}

/* function triggered by the SIGALRM handler when timer is TIMER_EV_START.
 * logs. */
static void handle_timer_start(t_timer *timer) {
    t_pgm *pgm = timer->pgm;
    t_proc_state state = PROC_ST_RUNNING;
    time_t elapsed = (pgm->usr.starttime / 1000) - (timer->time - time(NULL));

    if (pgm->usr.numprocs == pgm->privy.proc_cnt &&
        (elapsed >= (pgm->usr.starttime / 1000)))
        ft_log(FT_LOG_INFO,
               "(%d) %s successfully started. <%d/%d> seconds elapsed. "
               "<%d/%d> "
               "procs",
               pgm->privy.pgid, pgm->usr.name, elapsed,
               (pgm->usr.starttime / 1000), pgm->privy.proc_cnt,
               pgm->usr.numprocs);
    else
        ft_log(FT_LOG_INFO,
               "(%d) %s failed to start successfully. <%d/%d> seconds "
               "elapsed. <%d/%d> "
               "procs",
               pgm->privy.pgid, pgm->usr.name, elapsed,
               (pgm->usr.starttime / 1000), pgm->privy.proc_cnt,
               pgm->usr.numprocs);
    process_proc(timer->pgm, set_proc_state, &state);
}

/* function triggered by the SIGALRM handler when timer is TIMER_EV_STOP.
 * logs & eventually kill the pgm */
static void handle_timer_stop(t_timer *timer) {
    t_pgm *pgm = timer->pgm;
    time_t elapsed = (pgm->usr.stoptime / 1000) - (timer->time - time(NULL));

    if (!pgm->privy.proc_cnt) {
        ft_log(FT_LOG_INFO,
               "(%d) %s correctly terminated after <%d/%d> seconds elapsed. "
               "<%d/%d> procs left",
               pgm->privy.pgid, pgm->usr.name, elapsed,
               (pgm->usr.stoptime / 1000), pgm->privy.proc_cnt,
               pgm->usr.numprocs);
    } else {
        ft_log(FT_LOG_INFO,
               "(%d) %s didn't terminated correctly after <%d/%d> seconds "
               "elapsed. <%d/%d> procs left",
               pgm->privy.pgid, pgm->usr.name, elapsed,
               (pgm->usr.stoptime / 1000), pgm->privy.proc_cnt,
               pgm->usr.numprocs);
        kill(-(pgm->privy.pgid), SIGKILL);
    }
}

static void set_timer(t_timer *timer) {
    struct itimerval new = {0};
    void (*cb[2])(t_timer *) = {handle_timer_start, handle_timer_stop};

    if (!timer) {
        /* if timer is NULL, disarm it */
        if (setitimer(ITIMER_REAL, &new, NULL) == -1)
            ft_log(FT_LOG_ERR, "setitimer() failed: %s", strerror(errno));
        return;
    }

    new.it_value.tv_sec = timer->time - time(NULL);
    if (new.it_value.tv_sec <= 0) {
        /* if we already reached or exceeded the time, no need to set a timer
         * and rather trigger immediately the needed function */
        cb[timer->type - 1](timer);
        delete_timer(timer);
        return;
    }
    if (setitimer(ITIMER_REAL, &new, NULL) == -1)
        ft_log(FT_LOG_ERR, "setitimer() failed: %s", strerror(errno));
}

/* return the first timer related to pgm encountered, or NULL */
static t_timer *get_pgm_timer(t_pgm *pgm) {
    t_tm_node *node = get_node(NULL);
    t_timer *timer = node->timer_hd;

    while (timer && timer->pgm != pgm) timer = timer->next;
    return timer;
}

/* trigger every timers related to pgm. unused is to have a prototype compatible
 * with safe_timer_fn_call() callback parameter. */
static void trigger_pgm_timer(t_pgm *pgm, int32_t unused) {
    UNUSED_PARAM(unused);
    t_tm_node *node = get_node(NULL);
    t_timer *timer = node->timer_hd;
    void (*cb[2])(t_timer *) = {handle_timer_start, handle_timer_stop};

    while ((timer = get_pgm_timer(pgm))) {
        cb[timer->type - 1](timer); /* execute timer cb before deletion */
        delete_timer(timer);
    }
}

/* trigger the right function according to the type of the 1st timer in
 * the list */
static void sigalrm_handler(int signb) {
    UNUSED_PARAM(signb);
    t_tm_node *node = get_node(NULL);
    t_timer *tmr = node->timer_hd;
    void (*cb[2])(t_timer *) = {handle_timer_start, handle_timer_stop};

    if (!tmr) {
        ft_log(FT_LOG_WARNING, "SIGALRM triggered but no timer left");
        return;
    }
    cb[tmr->type - 1](tmr);
    delete_timer(tmr);
}

/* add a timer link to the list and set the timer if it is in 1st position */
static void add_timer(t_pgm *pgm, int32_t type) {
    t_tm_node *node = get_node(NULL);
    t_timer *tmr = node->timer_hd, *last = NULL, *timer;

    timer = malloc(1 * sizeof(*timer));
    if (!timer) {
        ft_log(FT_LOG_ERR, "malloc() failed: %s", strerror(errno));
        return;
    }
    timer->pgm = pgm, timer->type = type, timer->next = NULL;
    timer->time = time(NULL) + ((type == TIMER_EV_START ? pgm->usr.starttime
                                                        : pgm->usr.stoptime) /
                                1000);

    /* find where to insert the new timer in the list */
    while (tmr) {
        if (tmr->time > timer->time) break;
        last = tmr;
        tmr = tmr->next;
    }
    /* insert the timer in the list */
    if (last)
        last->next = timer;
    else
        node->timer_hd = timer;
    timer->next = tmr;

    if (!last) set_timer(timer); /* The timer is in 1st pos so must be set */
}

/* =========================== client engine utils ========================== */

/* -------------------------- processus launching --------------------------- */

/* Reset to default interactive and job-control signals. */
static void reset_dfl_interactive_sig() {
    struct sigaction act;

    act.sa_handler = SIG_DFL;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    sigaction(SIGINT, &act, NULL);
    sigaction(SIGQUIT, &act, NULL);
    sigaction(SIGTSTP, &act, NULL);
    sigaction(SIGTTIN, &act, NULL);
    sigaction(SIGTTOU, &act, NULL);
    sigaction(SIGCHLD, &act, NULL);
}

static pid_t launch_proc(const t_pgm *pgm, pid_t pgid) {
    pid_t pid;

    pid = fork();
    if (pid == -1) handle_error("fork()");
    if (pid == 0) {
        pid = getpid();
        if (!pgid) pgid = pid;
        setpgid(pid, pgid);

        reset_dfl_interactive_sig();

        if (pgm->usr.umask)
            umask(pgm->usr.umask); /* default file mode creation */
        if (pgm->usr.workingdir) {
            if (chdir((char *)pgm->usr.workingdir) == -1) perror("chdir");
        }

        dup2(pgm->privy.log.out, STDOUT_FILENO);
        close(pgm->privy.log.out);
        dup2(pgm->privy.log.err, STDERR_FILENO);
        close(pgm->privy.log.err);

        if (execve(pgm->usr.cmd[0], pgm->usr.cmd, pgm->usr.env.array_val) == -1)
            handle_error("execve");
    }
    return pid;
}

/* create a new proc, init it and add it into the linked list */
static void add_new_proc(t_pgm *pgm, pid_t cpid) {
    t_process *new = calloc(1, sizeof(*new));

    if (!new) handle_error("calloc");
    new->next = pgm->privy.proc_head;
    pgm->privy.proc_head = new;
    new->pid = cpid;
    new->state = PROC_ST_STARTING;
    new->restart_cnt++;
}

/* if there is room for a new proc: fork, execve, and add new proc to the list*/
static void launch_new_proc(t_pgm *pgm) {
    pid_t cpid;

    if (pgm->privy.proc_cnt == pgm->usr.numprocs) return; /* guard */
    cpid = launch_proc(pgm, pgm->privy.pgid);
    if (cpid) add_new_proc(pgm, cpid);
    if (!pgm->privy.pgid) pgm->privy.pgid = cpid;
    setpgid(cpid, pgm->privy.pgid);
    pgm->privy.proc_cnt++;
    ft_log(FT_LOG_INFO, "(%d) %s <%d> started", pgm->privy.pgid, pgm->usr.name,
           pgm->privy.proc_head->pid);
}

/* ---------------------------- processus delete ---------------------------- */

static int32_t delete_proc(t_pgm *pgm, t_process *last,
                           t_process **current_proc) {
    t_process *current = *current_proc;

    if (last) {
        last->next = current->next;
        *current_proc = last;
    } else {
        pgm->privy.proc_head = current->next;
        *current_proc = NULL;
    }
    free(current);
    pgm->privy.proc_cnt--;
    if (!pgm->privy.proc_cnt) pgm->privy.pgid = 0;
    return EXIT_SUCCESS;
}

/* ============================ job notification ============================ */

/* ---------------------------- processus update ---------------------------- */

static void update_proc_data(t_process *proc, pid_t pid) {
    proc->pid = pid, proc->restart_cnt++, proc->state = PROC_ST_RUNNING;
}

static void restart_proc(t_pgm *pgm, t_process *proc) {
    pid_t cpid = launch_proc(pgm, pgm->privy.pgid);
    if (cpid) update_proc_data(proc, cpid);
    if (!pgm->privy.pgid) pgm->privy.pgid = cpid;
    setpgid(cpid, pgm->privy.pgid);
    ft_log(FT_LOG_INFO, "(%d) %s <%d> restarted", pgm->privy.pgid,
           pgm->usr.name, proc->pid);
}

static int32_t proc_no_restart(t_pgm *pgm, t_process *proc) {
    int16_t unexpected = 1;

    for (int32_t i = 0; unexpected && i < pgm->usr.exitcodes.array_size; i++) {
        if (pgm->usr.exitcodes.array_val[i] == WEXITSTATUS(proc->w_status))
            unexpected = 0;
    }
    return (pgm->usr.autorestart == autorestart_false ||
            (pgm->usr.autorestart == autorestart_unexpected && !unexpected) ||
            proc->restart_cnt > pgm->usr.startretries);
}

/* remove, restart or notify proc according to new proc status */
static int32_t update_process(t_pgm *pgm, const void *arg, t_process *last,
                              t_process **current_proc) {
    t_process *current = *current_proc;
    UNUSED_PARAM(arg);

    if (!current->updated) return 0;
    if (WIFEXITED(current->w_status)) {
        ft_log(FT_LOG_INFO, "(%d) %s <%d> exited with status %d",
               pgm->privy.pgid, pgm->usr.name, current->pid,
               WEXITSTATUS(current->w_status));
        if (proc_no_restart(pgm, current)) {
            return delete_proc(pgm, last, current_proc);
        } else {
            restart_proc(pgm, current);
        }
    } else if (WIFSIGNALED(current->w_status)) {
        ft_log(FT_LOG_INFO, "(%d) %s <%d> terminated with signal %d",
               pgm->privy.pgid, pgm->usr.name, current->pid,
               WTERMSIG(current->w_status));
        delete_proc(pgm, last, current_proc);
        if (!pgm->privy.proc_cnt) safe_timer_fn_call(pgm, 0, trigger_pgm_timer);
        return EXIT_SUCCESS;
    } else if (WIFSTOPPED(current->w_status)) {
        ft_log(FT_LOG_INFO, "(%d) %s <%d> stopped with signal %d",
               pgm->privy.pgid, pgm->usr.name, current->pid,
               WSTOPSIG(current->w_status));
    } else
        ft_log(FT_LOG_INFO, "wat signal update_proc() ?\n");
    current->updated = false;
    current->w_status = 0;
    return EXIT_SUCCESS;
}

/* wrapper to call process_proc() with update_proc() callback and logic around*/
static int32_t update_proc_ctrl(t_pgm *pgm, void *arg) {
    if (!pgm->privy.updated) return 0;
    process_proc(pgm, update_process, arg);
    pgm->privy.updated = false;
    return 0;
}

/* ---------------------------- processus notif ----------------------------- */

static int32_t notify_process(t_pgm *pgm, const void *arg, t_process *last,
                              t_process **current_proc) {
    UNUSED_PARAM(last);
    UNUSED_PARAM(pgm);
    t_process *current = *current_proc;
    const struct {
        pid_t pid;
        int32_t status;
    } *args = arg;

    if (current->pid == args->pid) {
        current->w_status = args->status;
        current->updated = true;
        pgm->privy.updated = true;
        return 1;
    }
    return 0;
}

/* wrapper to call process_proc() with the relevant callback and logic around*/
static int32_t wr_notify_process(t_pgm *pgm, void *arg) {
    return process_proc(pgm, notify_process, arg);
}

/* finds which process has pid as pid and flags it (wr_notify_process) */
static int32_t mark_process_status(t_tm_node *node, pid_t pid, int32_t status) {
    struct {
        pid_t pid;
        int32_t status;
    } arg = {pid, status};

    if (pid > 0) {
        if (process_pgm(node->head, wr_notify_process, &arg)) return 0;
        fprintf(stderr, "No child process %d.\n", pid);
        return -1;
    } else if (pid == 0 || errno == ECHILD) /* No processes ready to report. */
        return -1;
    else { /* Other weird errors.  */
        perror("waitpid");
        return -1;
    }
}

/* if any child has a new status, mark it */
static void update_pgm_status(t_tm_node *node) {
    int status;
    pid_t pid;

    do {
        pid = waitpid(WAIT_ANY, &status, WUNTRACED | WNOHANG);
    } while (!mark_process_status(node, pid, status));
}

/* ====================== command handlers primitives ======================= */

/* launch all not-yet-launched processes of a pgm & add start timer */
static void launch_pgm(t_pgm *pgm) {
    int32_t nb_new_proc = pgm->usr.numprocs - pgm->privy.proc_cnt;

    for (int32_t i = 0; i < nb_new_proc; i++) launch_new_proc(pgm);
    safe_timer_fn_call(pgm, TIMER_EV_START, add_timer);
}

static int32_t signal_stop_pgm(t_pgm *pgm) {
    t_proc_state state = PROC_ST_TERMINATING;

    if (!pgm->privy.proc_cnt) return 1;
    kill(-(pgm->privy.pgid), pgm->usr.stopsignal.nb);
    process_proc(pgm, set_proc_state, &state);
    safe_timer_fn_call(pgm, TIMER_EV_STOP, add_timer);
    return 0;
}

/* wait a new status from pgm and notify it */
static int32_t wait_one_pgm(t_pgm *pgm, const void *arg, t_process *last,
                            t_process **current) {
    UNUSED_PARAM(arg);
    struct {
        pid_t pid;
        int32_t status;
    } args;

    args.pid = waitpid((*current)->pid, &args.status, 0);
    if (!notify_process(pgm, &args, last, current)) perror("waitpid");
    return EXIT_SUCCESS;
}

/* end the pgm and wait it */
static int32_t exit_pgm(t_pgm *pgm, void *arg) {
    UNUSED_PARAM(arg);
    if (signal_stop_pgm(pgm)) return 0;
    process_proc(pgm, wait_one_pgm, NULL);
    update_proc_ctrl(pgm, NULL);
    return 0;
}

static int32_t status_pgm(t_pgm *pgm, void *arg) {
    UNUSED_PARAM(arg);
    printf("- [%d] %s: <%d/%d> started\n", pgm->privy.pgid, pgm->usr.name,
           pgm->privy.proc_cnt, pgm->usr.numprocs);
    return EXIT_SUCCESS;
}

static void status_proc(t_pgm *pgm) {
    char proc_st[PROC_ST_MAX][16] = {"starting", "running", "terminating"};
    status_pgm(pgm, NULL);
    for (t_process *proc = pgm->privy.proc_head; proc; proc = proc->next)
        printf("pid <%d> - %s - restarted <%d/%d> times\n", proc->pid,
               proc_st[proc->state], proc->restart_cnt - 1,
               pgm->usr.startretries);
}

/* --------------------------------- reload --------------------------------- */

static int32_t tm_strcmp(const char *s1, const char *s2) {
    const unsigned char *s1_cp = (const unsigned char *)s1;
    const unsigned char *s2_cp = (const unsigned char *)s2;
    uint8_t diff;

    if (!s1_cp && !s2_cp) return EXIT_SUCCESS;
    if (!s1_cp || !s2_cp) return EXIT_FAILURE;
    diff = (*s1_cp != *s2_cp);
    while (*s1_cp && *s2_cp && !diff) {
        s1_cp++;
        s2_cp++;
        diff = (*s1_cp != *s2_cp);
    }
    return (*s1_cp - *s2_cp);
}

/* just compare two programs names. Returns 1 if they have the same name */
static int32_t find_same_pgm(t_pgm *pgm, void *arg) {
    return (tm_strcmp(pgm->usr.name, ((t_pgm *)arg)->usr.name) == 0);
}

/* looks for pgm into arg list, notify pgm to be deleted if not found */
static int32_t notify_removable_pgm(t_pgm *pgm, void *arg) {
    if (!process_pgm((t_pgm *)arg, find_same_pgm, pgm)) {
        ft_log(FT_LOG_DEBUG, "pgm %s - del", pgm->usr.name);
        pgm->privy.ev = PGM_EV_DEL;
    }
    return 0;
}

/* looks for new_pgm into main list (arg), notify new_pgm to be added if
 * not found & switch it from lists */
static int32_t notify_new_pgm(t_pgm *new_pgm, void *arg) {
    t_tm_node *newnode = get_newnode(NULL, false), *node = get_node(NULL);

    if (!process_pgm((t_pgm *)arg, find_same_pgm, new_pgm)) {
        ft_log(FT_LOG_DEBUG, "pgm %s - add", new_pgm->usr.name);
        new_pgm->privy.ev = PGM_EV_ADD;
        pgm_list_remove(newnode, new_pgm);
        pgm_list_add_front(node, new_pgm);
    }
    return 0;
}

static int32_t str_array_cmp(char *const *arr1, char *const *arr2) {
    int32_t i = 0;

    while (arr1[i] && arr2[i] && !tm_strcmp(arr1[i], arr2[i])) i++;
    return !(!arr1[i] && !arr2[i]);
}

#define CLIENT_SOFT_RELOAD 1
#define CLIENT_HARD_RELOAD 2

/* compares two pgm with the same name and returns a status code according to
 * the differences between them. */
static uint8_t pgm_compare(t_pgm *p1, t_pgm *p2) {
    if (tm_strcmp((char *)p1->usr.name, (char *)p2->usr.name)) return 0;
    bool soft_reload = (p1->usr.autostart != p2->usr.autostart ||
                        p1->usr.autorestart != p2->usr.autorestart ||
                        p1->usr.starttime != p2->usr.starttime ||
                        p1->usr.startretries != p2->usr.startretries ||
                        p1->usr.stopsignal.nb != p2->usr.stopsignal.nb ||
                        p1->usr.stoptime != p2->usr.stoptime);
    bool hard_reload =
        (str_array_cmp(p1->usr.cmd, p2->usr.cmd) ||
         p1->usr.numprocs != p2->usr.numprocs ||
         p1->usr.exitcodes.array_size != p2->usr.exitcodes.array_size ||
         tm_strcmp((char *)p1->usr.std_out, (char *)p2->usr.std_out) ||
         tm_strcmp((char *)p1->usr.std_err, (char *)p2->usr.std_err) ||
         p1->usr.env.array_size != p2->usr.env.array_size ||
         tm_strcmp((char *)p1->usr.workingdir, (char *)p2->usr.workingdir) ||
         p1->usr.umask != p2->usr.umask);

    for (uint32_t cnt = 0; cnt < p1->usr.env.array_size && !hard_reload; cnt++)
        if (tm_strcmp((char *)p1->usr.env.array_val[cnt],
                      (char *)p2->usr.env.array_val[cnt]) != 0)
            hard_reload = true;

    for (uint32_t cnt = 0; cnt < p1->usr.exitcodes.array_size && !hard_reload;
         cnt++)
        if (p1->usr.exitcodes.array_val[cnt] !=
            p2->usr.exitcodes.array_val[cnt])
            soft_reload = true;

    return ((hard_reload * CLIENT_HARD_RELOAD) +
            ((soft_reload && !hard_reload) * CLIENT_SOFT_RELOAD));
}

/* copies all values from pgm_new to pgm, which don't need a restart of pgm */
static int32_t pgm_soft_cpy(t_pgm *pgm, t_pgm *pgm_new) {
    pgm->usr.autostart = pgm_new->usr.autostart;
    pgm->usr.autorestart = pgm_new->usr.autorestart;
    pgm->usr.starttime = pgm_new->usr.starttime;
    pgm->usr.startretries = pgm_new->usr.startretries;
    pgm->usr.stopsignal = pgm_new->usr.stopsignal;
    pgm->usr.stoptime = pgm_new->usr.stoptime;
    for (uint32_t i = 0; i < pgm->usr.exitcodes.array_size; i++)
        pgm->usr.exitcodes.array_val[i] = pgm_new->usr.exitcodes.array_val[i];
    return EXIT_SUCCESS;
}

/* finds if the two pgm beeing compared are the same (same name) but have few
 * changes in the configuration which justify either a soft or hard reload, and
 * notify &/| accordingly */
static int32_t find_reloadable_pgm(t_pgm *pgm, void *arg) {
    t_tm_node *newnode = get_newnode(NULL, false);
    t_pgm *pgm_new = (t_pgm *)arg;
    int32_t ret = 0;

    ret = pgm_compare(pgm, pgm_new);
    if (ret == CLIENT_SOFT_RELOAD) {
        ft_log(FT_LOG_DEBUG, "%s soft reload", pgm->usr.name);
        pgm_soft_cpy(pgm, pgm_new);
    } else if (ret == CLIENT_HARD_RELOAD) {
        ft_log(FT_LOG_DEBUG, "%s hard reload", pgm->usr.name);
        pgm->privy.ev = PGM_EV_DEL;
        pgm_new->privy.ev = PGM_EV_ADD;
        pgm_list_remove(newnode, pgm_new);
        pgm_list_insert_after(pgm, pgm_new);
    }
    return ret;
}

/* applies find_reloadable_pgm() to the main list (arg) against pgm_new */
static int32_t notify_reloadable_pgm(t_pgm *pgm_new, void *arg) {
    process_pgm((t_pgm *)arg, find_reloadable_pgm, pgm_new);
    return 0;
}

/* ========================= command handlers utils ========================= */

static char *get_next_word(const char *str) {
    int32_t i = 0;

    while (str[i] == ' ') i++;
    if (i) return (char *)(str + i);
    while (str[i] && str[i] != ' ') i++;
    while (str[i] == ' ') i++;
    if (!str[i]) return NULL;
    return (char *)(str + i);
}

/* compare pgm names with the current argument and returns the corresponding
 * pgm adress if it match */
static t_pgm *get_pgm(const t_tm_node *node, char **args) {
    if (!*args) return NULL;
    for (t_pgm *pgm = node->head; pgm; pgm = pgm->privy.next) {
        if (!strncmp(pgm->usr.name, *args, strlen(pgm->usr.name))) {
            *args = get_next_word(*args);
            return pgm;
        }
    }
    return NULL;
}

/* ============================== command handlers ========================== */

/* status can have 0 or 1 argument */
DECL_CMD_HANDLER(cmd_status) {
    t_tm_cmd *cmd = command;
    char *args = cmd->args;
    t_pgm *pgm;

    if (cmd->args) {
        while ((pgm = get_pgm(node, &args))) status_proc(pgm);
    } else {
        process_pgm(node->head, status_pgm, NULL);
    }

    return EXIT_SUCCESS;
}

/* start has many arguments which must match with a pgm name */
DECL_CMD_HANDLER(cmd_start) {
    t_tm_cmd *cmd = command;
    char *args = cmd->args;
    t_pgm *pgm;

    while ((pgm = get_pgm(node, &args))) launch_pgm(pgm);
    return EXIT_SUCCESS;
}

/* stop has many arguments which must match with a pgm name */
DECL_CMD_HANDLER(cmd_stop) {
    t_tm_cmd *cmd = command;
    char *args = cmd->args;
    t_pgm *pgm;

    while ((pgm = get_pgm(node, &args))) signal_stop_pgm(pgm);
    return EXIT_SUCCESS;
}

/* restart has many arguments which must match with a pgm name */
DECL_CMD_HANDLER(cmd_restart) {
    t_tm_cmd *cmd = command;
    char *args = cmd->args;
    t_pgm *pgm;

    while ((pgm = get_pgm(node, &args))) {
        pgm->privy.ev = PGM_EV_RESTART;
        signal_stop_pgm(pgm);
    }
    return EXIT_SUCCESS;
}

/* reload config has 0 argument */
DECL_CMD_HANDLER(cmd_reload) {
    UNUSED_PARAM(command);
    t_tm_node node_reload = {.tm_name = node->tm_name, 0};

    if (!(node_reload.config_file_stream =
              fopen(node->config_file_name, "r"))) {
        fprintf(stderr, "%s: %s: %s\n", node_reload.tm_name,
                node->config_file_name, strerror(errno));
        goto error;
    }
    if (load_config_file(&node_reload)) goto error;
    if (sanitize_config(&node_reload)) goto error;
    if (fulfill_config(&node_reload)) goto error;

    get_newnode(&node_reload, true); /* init newnode getter */
    process_pgm(node->head, notify_removable_pgm, node_reload.head);
    process_pgm(node_reload.head, notify_new_pgm, node->head);
    process_pgm(node_reload.head, notify_reloadable_pgm, node->head);
    node->pgm_nb = node_reload.pgm_nb;
    get_newnode(NULL, true); /* reset newnode getter */

    add_cli_completion();
    destroy_taskmaster(&node_reload);
    return EXIT_SUCCESS;
error:
    ft_log(FT_LOG_INFO, "failed to reload %s", node->config_file_name);
    return EXIT_FAILURE;
}

/* exit has 0 argument */
DECL_CMD_HANDLER(cmd_exit) {
    UNUSED_PARAM(command);
    process_pgm(node->head, exit_pgm, NULL);
    node->exit = true;
    return EXIT_SUCCESS;
}

/* help has 0 argument */
DECL_CMD_HANDLER(cmd_help) {
    UNUSED_PARAM(node);
    UNUSED_PARAM(command);
    fputs(
        "start <name>\t\tStart processes\n"
        "stop <name>\t\tStop processes\n"
        "restart <name>\t\tRestart all processes\n"
        "reload\t\tReload the configuration file\n"
        "status <name>\t\tGet status for <name> processes\n"
        "status\t\tGet status for all programs\n"
        "exit\t\tExit the taskmaster shell and server.\n",
        stdout);
    fflush(stdout);
    return EXIT_SUCCESS;
}

/* =========================== event handlers =============================== */

/* generic declaration for pgm event handlers */
#define DECL_PGM_EV_HANDLER(name) static void name(t_pgm *pgm)

DECL_PGM_EV_HANDLER(no_ev) { UNUSED_PARAM(pgm); }

/* launch pgm if all processus are down */
DECL_PGM_EV_HANDLER(restart_ev) {
    if (pgm->privy.proc_cnt > 0) return;
    launch_pgm(pgm);
    pgm->privy.ev = PGM_NO_EV;
}

DECL_PGM_EV_HANDLER(add_ev) {
    if (pgm->privy.proc_cnt > 0 || !pgm->usr.autostart) return;
    launch_pgm(pgm);
    pgm->privy.ev = PGM_NO_EV;
}

DECL_PGM_EV_HANDLER(del_ev) {
    t_tm_node *node = get_node(NULL);
    exit_pgm(pgm, NULL);
    pgm_list_remove(node, pgm);
    destroy_pgm(pgm);
}

/* wrapper to execute event callbacks */
static int32_t handle_event(t_pgm *pgm, void *arg) {
    UNUSED_PARAM(arg);
    void (*handler[PGM_MAX_EV])(t_pgm * pgm) = {no_ev, restart_ev, add_ev,
                                                del_ev};

    handler[pgm->privy.ev](pgm);
    return EXIT_SUCCESS;
}

/* ============================= client engine ============================== */

/* notify any job activity then update pgm and finally handle events */
static void pgm_notification(t_tm_node *node) {
    update_pgm_status(node);
    process_pgm(node->head, update_proc_ctrl, NULL);
    process_pgm(node->head, handle_event, NULL);
}

static void sighup_handler(int signb) {
    UNUSED_PARAM(signb);
    t_tm_node *node = get_node(NULL);

    ft_log(FT_LOG_DEBUG, "SIGHUP received");
    cmd_reload(node, NULL);
    process_pgm(node->head, handle_event, NULL);
}

static void sigchild_handler(int signb) {
    UNUSED_PARAM(signb);
    pgm_notification(get_node(NULL));
}

/* Reset args of command */
static inline void clean_command(t_tm_cmd *command) {
    for (int32_t i = 0; i < TM_CMD_NB; i++) command[i].args = NULL;
}

static int32_t init_launch_pgm(t_pgm *pgm, void *arg) {
    UNUSED_PARAM(arg);
    if (pgm->usr.autostart) launch_pgm(pgm);
    return EXIT_SUCCESS;
}

static void auto_start(const t_tm_node *node) {
    process_pgm(node->head, init_launch_pgm, NULL);
}

/* set sigaction structures for both default and child signals handling */
static void init_sigaction(struct sigaction *sigchld_dfl_act,
                           struct sigaction *sigchld_handle_act,
                           struct sigaction *sigalrm_handle_act,
                           struct sigaction *sighup_handle_act) {
    sigset_t block_mask;

    /* sigchld_dfl_act */
    sigchld_dfl_act->sa_handler = SIG_DFL;
    sigchld_dfl_act->sa_flags = 0;
    sigemptyset(&(sigchld_dfl_act->sa_mask));

    /* sigchld_handle_act */
    sigemptyset(&block_mask);
    /* Block other terminal-generated signals while handler runs. */
    sigaddset(&block_mask, SIGINT);
    sigaddset(&block_mask, SIGQUIT);
    sigaddset(&block_mask, SIGTSTP);
    sigaddset(&block_mask, SIGTTIN);
    sigaddset(&block_mask, SIGTTOU);
    sigaddset(&block_mask, SIGHUP);
    /* block timer-generated signal while sigchld handler runs*/
    sigaddset(&block_mask, SIGALRM);
    sigchld_handle_act->sa_mask = block_mask;
    sigchld_handle_act->sa_handler = sigchild_handler;
    /* if signal happen during a read(), restart it instead of fail it */
    sigchld_handle_act->sa_flags = SA_RESTART;

    /* sigalrm_handle_act */
    /* block child-generated signal while sigalrm handler runs */
    sigdelset(&block_mask, SIGALRM);
    sigaddset(&block_mask, SIGCHLD);
    sigalrm_handle_act->sa_mask = block_mask;
    sigalrm_handle_act->sa_handler = sigalrm_handler;
    sigalrm_handle_act->sa_flags = SA_RESTART;

    /* sigalrm_handle_act */
    sigaddset(&block_mask, SIGALRM);
    sighup_handle_act->sa_mask = block_mask;
    sighup_handle_act->sa_handler = sighup_handler;
    sighup_handle_act->sa_flags = SA_RESTART;
}

/* Main client function. Reads, sanitize & execute client input */
uint8_t run_client(t_tm_node *node) {
    struct sigaction sigchld_dfl_act, sigchld_handle_act, sigalrm_handle_act,
        sighup_handle_act;
    t_tm_cmd *command = get_commands();
    char *line = NULL;
    int32_t hdlr_type;

    get_node(node); /* init node getter */
    ft_log(FT_LOG_INFO, "started");
    atexit(log_exit);

    add_cli_completion();

    init_sigaction(&sigchld_dfl_act, &sigchld_handle_act, &sigalrm_handle_act,
                   &sighup_handle_act);
    sigaction(SIGCHLD, &sigchld_handle_act, NULL);
    sigaction(SIGALRM, &sigalrm_handle_act, NULL);
    sigaction(SIGHUP, &sighup_handle_act, NULL);

    auto_start(node);
    while (!node->exit && (line = ft_readline("taskmaster$ ")) != NULL) {
        /* avoid reentrancy problems */
        sigaction(SIGCHLD, &sigchld_dfl_act, NULL);
        // TODO perhaps ign/dfl SIGALRM too an check timer queue at the end.

        ft_readline_add_history(line);
        format_user_input(line); /* maybe use this only to send to a client */
        hdlr_type = find_cmd(node, command, line);

        if (hdlr_type >= 0) {
            command[hdlr_type].handler(node, &command[hdlr_type]);
        } else if (hdlr_type != CMD_EMPTY_LINE)
            err_usr_input(node, hdlr_type);
        clean_command(command);
        free(line);
        pgm_notification(node);
        sigaction(SIGCHLD, &sigchld_handle_act, NULL);
    }
    return EXIT_SUCCESS;
}
