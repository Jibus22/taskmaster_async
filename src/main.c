#include <errno.h>
#include <signal.h>
#include <termio.h>

#include "ft_log.h"
#include "taskmaster.h"

static uint8_t usage(char *const *av) {
  fprintf(stderr, "Usage: %s [-f filename]\n", av[0]);
  return EXIT_FAILURE;
}

static uint8_t get_options(int ac, char *const *av, t_tm_node *node) {
  int32_t opt;

  while ((opt = getopt(ac, av, "f:")) != -1) {
    switch (opt) {
      case 'f':
        node->config_file_name = strdup(optarg);
        if (!node->config_file_name) handle_error("strdup");
        if (!(node->config_file_stream = fopen(optarg, "r"))) {
          fprintf(stderr, "%s: %s: %s\n", av[0], optarg, strerror(errno));
          return EXIT_FAILURE;
        }
        break;
      case '?':
      default:
        return usage(av);
    }
  }

  if (ac < 2 || optind < ac) return usage(av);

  return EXIT_SUCCESS;
}

/* Ignore interactive and job-control signals. */
static void ignore_interactive_sig() {
  struct sigaction act;

  act.sa_handler = SIG_IGN;
  sigemptyset(&act.sa_mask);
  act.sa_flags = 0;
  sigaction(SIGINT, &act, NULL);
  sigaction(SIGQUIT, &act, NULL);
  sigaction(SIGTSTP, &act, NULL);
  sigaction(SIGTTIN, &act, NULL);
  sigaction(SIGTTOU, &act, NULL);
  sigaction(SIGCHLD, &act, NULL);
}

/* make sure taskmaster is in foreground before continuing */
static int32_t init_shell(t_tm_node *node) {
  struct termios shell_tmodes;
  /* See if we are running interactively. */
  int32_t shell_terminal = STDIN_FILENO,
          shell_is_interactive = isatty(shell_terminal);

  if (shell_is_interactive) {
    /* Loop until we are in the foreground. */
    while (tcgetpgrp(shell_terminal) != (node->shell_pgid = getpgrp()))
      kill(-node->shell_pgid, SIGTTIN);

    ignore_interactive_sig();

    node->shell_pgid = getpid(); /* Put ourselves in our own process group. */
    if (setpgid(node->shell_pgid, node->shell_pgid) < 0)
      goto_error("Couldn't put the shell in its own process group");

    /* Grab control of the terminal. */
    tcsetpgrp(shell_terminal, node->shell_pgid);
    /* Save default terminal attributes for shell. */
    tcgetattr(shell_terminal, &shell_tmodes);
    return EXIT_SUCCESS;
  } else
    fprintf(stderr, "%s: can't be launched in non-interactive mode\n",
            node->tm_name);
error:
  destroy_taskmaster(node);
  return EXIT_FAILURE;
}

int main(int ac, char **av) {
  t_tm_node node = {.tm_name = av[0], 0};

  if (get_options(ac, av, &node)) goto error;
  if (ft_openlog(node.tm_name, TM_LOGFILE)) goto_error("ft_openlog");
  if (load_config_file(&node)) return EXIT_FAILURE;
  if (sanitize_config(&node)) return EXIT_FAILURE;
  if (fulfill_config(&node)) return EXIT_FAILURE;
  if (init_shell(&node)) return EXIT_FAILURE;
  run_client(&node);
  print_pgm_list(node.head);
  destroy_taskmaster(&node);
  return EXIT_SUCCESS;
error:
  destroy_taskmaster(&node);
  return EXIT_FAILURE;
}
