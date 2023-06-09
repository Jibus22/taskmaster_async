#include "taskmaster.h"

void print_pgm_list(t_pgm *head) {
#ifdef DEVELOPEMENT
    t_pgm_usr *pgm;
    while (head) {
        pgm = &head->usr;
        printf("-------------------\n");
        printf("addr: %p\nname: %s\nstdout: %s\nstderr: %s\nworkingdir: %s\n",
               pgm, pgm->name, pgm->std_out, pgm->std_err, pgm->workingdir);
        printf("cmd: (%p)\n", pgm->cmd);
        for (uint32_t i = 0; pgm->cmd && pgm->cmd[i]; i++)
            printf("\t(%s)\n", pgm->cmd[i]);
        printf("env: (%p)\n", pgm->env.array_val);
        for (uint32_t i = 0; pgm->env.array_val && i < pgm->env.array_size; i++)
            printf("\t(%s)\n", pgm->env.array_val[i]);
        printf("exitcodes: (%p)\n", pgm->exitcodes.array_val);
        for (uint32_t i = 0; i < pgm->exitcodes.array_size; i++)
            printf("\t(%d)\n", pgm->exitcodes.array_val[i]);
        printf(
            "numprocs: %d\numask: %o\nautorestart: %d\nstartretries: "
            "%d\nautostart: %d\nstopsignal: %s\nstarttime: %d\nstoptime: "
            "%d\nnext: %p\n",
            pgm->numprocs, pgm->umask, pgm->autorestart, pgm->startretries,
            pgm->autostart, pgm->stopsignal.name, pgm->starttime, pgm->stoptime,
            head->privy.next);
        fflush(stdout);
        head = head->privy.next;
    }
#else
    UNUSED_PARAM(head);
#endif
}
