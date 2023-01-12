#include "run_client.h"

#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>

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

/* =============================== initialization =========================== */

static void *destroy_str_array(char **array, uint32_t sz) {
    while (--sz >= 0) {
        free(array[sz]);
        array[sz] = NULL;
    }
    return NULL;
}

/* Add taskmaster commands and program names to completion */
static char **get_completion(const t_tm_node *node, const t_tm_cmd *commands,
                             uint32_t cmd_nb) {
    uint32_t i = 0;
    t_pgm *pgm = node->head;
    char **completions = malloc(cmd_nb * sizeof(*completions));

    if (!completions) return NULL;

    while (i < TM_CMD_NB) {
        completions[i] = strdup(commands[i].name);
        if (!completions[i]) return destroy_str_array(completions, i);
        i++;
    }
    while (i < cmd_nb && pgm) {
        completions[i] = strdup(pgm->usr.name);
        if (!completions[i]) return destroy_str_array(completions, i);
        pgm = pgm->privy.next;
        i++;
    }
    return completions;
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

/* ================================= getters ================================ */

static t_tm_node *get_node(t_tm_node *node) {
    static bool init = false;
    static t_tm_node *my_node = NULL;

    if (!init) {
        init = true;
        my_node = node;
    }
    return my_node;
}

/* ============================== lists processors ========================== */

typedef int32_t (*t_handle)(t_pgm *pgm, const void *arg);

/* to each pgm, execute handle callback. handle takes pgm and arg as argument */
static int32_t process_pgm(t_pgm *pgm, t_handle handle, const void *arg) {
    while (pgm) {
        if (handle(pgm, arg)) return 1;
        pgm = pgm->privy.next;
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

/* ======================== client engine primitives ======================== */

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
    if (pid == -1) return -1;
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
    client_debug("new: gpid:%d, pid:%d\n", getpgid(cpid), cpid);
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
    proc->pid = pid, proc->restart_cnt++;
}

static void restart_proc(t_pgm *pgm, t_process *proc) {
    pid_t cpid = launch_proc(pgm, pgm->privy.pgid);
    if (cpid) update_proc_data(proc, cpid);
    if (!pgm->privy.pgid) pgm->privy.pgid = cpid;
    setpgid(cpid, pgm->privy.pgid);
    client_debug("restart: gpid:%d, pid:%d\n", getpgid(cpid), cpid);
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
    UNUSED_PARAM(arg);
    t_process *current = *current_proc;

    client_debug("update_proc()\n");
    if (!current->updated) return 0;
    if (WIFEXITED(current->w_status)) {
        client_debug("w exited\n");
        if (proc_no_restart(pgm, current)) {
            return delete_proc(pgm, last, current_proc);
        } else {
            restart_proc(pgm, current);
        }
    } else if (WIFSIGNALED(current->w_status)) {
        client_debug("w signaled with %d\n", WTERMSIG(current->w_status));
        return delete_proc(pgm, last, current_proc);
    } else if (WIFSTOPPED(current->w_status)) {
        client_debug("w stopped\n");
    } else
        client_debug("wat signal update_proc() ?\n");
    current->updated = false;
    current->w_status = 0;
    return EXIT_SUCCESS;
}

/* wrapper to call process_proc() with update_proc() callback and logic around*/
static int32_t update_proc_ctrl(t_pgm *pgm, const void *arg) {
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
        client_debug("flag_process() [%d]\n", args->pid);
        current->w_status = args->status;
        current->updated = true;
        pgm->privy.updated = true;
        return 1;
    }
    return 0;
}

/* wrapper to call process_proc() with the relevant callback and logic around*/
static int32_t wr_notify_process(t_pgm *pgm, const void *arg) {
    return process_proc(pgm, notify_process, arg);
}

/* finds which process has pid as pid and flags it (wr_flag_process) */
static int32_t mark_process_status(t_tm_node *node, pid_t pid, int32_t status) {
    struct {
        pid_t pid;
        int32_t status;
    } arg = {pid, status};

    client_debug("mark_process_status(), pid:%d\n", pid);
    if (pid > 0) {
        if (process_pgm(node->head, wr_notify_process, &arg)) return 0;
        fprintf(stderr, "No child process %d.\n", pid);
        return -1;
    } else if (pid == 0 || errno == ECHILD) /* No processes ready to report. */
        return -1;
    else { /* Other weird errors.  */
        client_debug("error mark_process_status()");
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

/* launch all not-yet-launched processes of a pgm */
static void launch_pgm(t_pgm *pgm) {
    int32_t nb_new_proc = pgm->usr.numprocs - pgm->privy.proc_cnt;

    for (int32_t i = 0; i < nb_new_proc; i++) launch_new_proc(pgm);
}

static int32_t signal_stop_pgm(t_pgm *pgm) {
    if (!pgm->privy.proc_cnt) return 1;
    kill(-(pgm->privy.pgid), pgm->usr.stopsignal.nb);
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
static int32_t exit_pgm(t_pgm *pgm, const void *arg) {
    UNUSED_PARAM(arg);
    if (signal_stop_pgm(pgm)) return 0;
    process_proc(pgm, wait_one_pgm, NULL);
    update_proc_ctrl(pgm, NULL);
    return 0;
}

static int32_t status_pgm(t_pgm *pgm, const void *arg) {
    UNUSED_PARAM(arg);
    printf("- [%d] %s: <%d/%d> running\n", pgm->privy.pgid, pgm->usr.name,
           pgm->privy.proc_cnt, pgm->usr.numprocs);
    return EXIT_SUCCESS;
}

static void status_proc(t_pgm *pgm) {
    status_pgm(pgm, NULL);
    for (t_process *proc = pgm->privy.proc_head; proc; proc = proc->next)
        printf("pid <%d> - restarted <%d/%d> times\n", proc->pid,
               proc->restart_cnt, pgm->usr.startretries);
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

/* Compare pgm names with the current argument and returns the corresponding
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
        if (!pgm->privy.proc_cnt) continue;
        kill(-(pgm->privy.pgid), pgm->usr.stopsignal.nb);
    }
    return EXIT_SUCCESS;
}

/* reload config has 0 argument */
DECL_CMD_HANDLER(cmd_reload) {
    UNUSED_PARAM(node);
    t_tm_cmd *cmd = command;
    printf("commands: |%s|\n", cmd->name);
    return EXIT_SUCCESS;
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

DECL_PGM_EV_HANDLER(restart_ev) {
    if (pgm->privy.proc_cnt > 0) return;
    launch_pgm(pgm);
    pgm->privy.ev = PGM_NO_EV;
}

DECL_PGM_EV_HANDLER(exit_ev) {
    UNUSED_PARAM(pgm);
    // Si je choisis ce design pour exit() ce serait pour la simple raison de
    // ne pas bloquer le shell pendant que ça exit, parce que le handler
    // cmd_exit() aurait juste stop tlm et ici dans cet event handler on aurait
    // juste à checker si proc_cnt == 0 pour enclencher la suite de la logique
    // qui est: long_jump en dehors de la boucle dans le run_client(), pour
    // sortir.
    // Il faudrait cependant bien prendre la précaution de ne jamais reset un
    // event PGM_EV_EXIT est set.
}

static int32_t handle_event(t_pgm *pgm, const void *arg) {
    UNUSED_PARAM(arg);
    void (*handler[PGM_MAX_EV])(t_pgm * pgm) = {no_ev, restart_ev, exit_ev};

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

static void sigchild_handler(int signb) {
    UNUSED_PARAM(signb);
    pgm_notification(get_node(NULL));
}

/* Reset args of command */
static inline void clean_command(t_tm_cmd *command) {
    for (int32_t i = 0; i < TM_CMD_NB; i++) command[i].args = NULL;
}

static int32_t init_launch_pgm(t_pgm *pgm, const void *arg) {
    UNUSED_PARAM(arg);
    if (pgm->usr.autostart) launch_pgm(pgm);
    return EXIT_SUCCESS;
}

static void auto_start(const t_tm_node *node) {
    process_pgm(node->head, init_launch_pgm, NULL);
}

/* set sigaction structures for both default and child signals handling */
static void init_sigaction(struct sigaction *sigdfl_act,
                           struct sigaction *sigchld_act) {
    sigset_t block_mask;

    sigdfl_act->sa_handler = SIG_DFL;
    sigdfl_act->sa_flags = 0;
    sigemptyset(&(sigdfl_act->sa_mask));

    sigchld_act->sa_handler = sigchild_handler;
    /* if signal happen during a read(), restart it instead of fail it */
    sigchld_act->sa_flags = SA_RESTART;
    sigemptyset(&block_mask);
    /* Block other terminal-generated signals while handler runs. */
    sigaddset(&block_mask, SIGINT);
    sigaddset(&block_mask, SIGQUIT);
    sigaddset(&block_mask, SIGTSTP);
    sigaddset(&block_mask, SIGTTIN);
    sigaddset(&block_mask, SIGTTOU);
    sigchld_act->sa_mask = block_mask;
}

/* Main client function. Reads, sanitize & execute client input */
uint8_t run_client(t_tm_node *node) {
    char *line = NULL, **completion = NULL;
    int32_t cmd_nb = TM_CMD_NB + node->pgm_nb, hdlr_type;
    struct sigaction sigdfl_act, sigchld_act;
    t_tm_cmd command[TM_CMD_NB] = {{cmd_status, "status", FREE_NB_ARGS, 0},
                                   {cmd_start, "start", MANY_ARGS, 0},
                                   {cmd_stop, "stop", MANY_ARGS, 0},
                                   {cmd_restart, "restart", MANY_ARGS, 0},
                                   {cmd_reload, "reload", NO_ARGS, 0},
                                   {cmd_exit, "exit", NO_ARGS, 0},
                                   {cmd_help, "help", NO_ARGS, 0}};

    completion = get_completion(node, command, cmd_nb);
    ft_readline_add_completion(completion, cmd_nb);

    get_node(node);
    auto_start(node);

    init_sigaction(&sigdfl_act, &sigchld_act);
    sigaction(SIGCHLD, &sigchld_act, NULL);
    while (!node->exit && (line = ft_readline("taskmaster$ ")) != NULL) {
        sigaction(SIGCHLD, &sigdfl_act, NULL);
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
        sigaction(SIGCHLD, &sigchld_act, NULL);
    }
    return EXIT_SUCCESS;
}
