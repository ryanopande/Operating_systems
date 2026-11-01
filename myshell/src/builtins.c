// builtins.c — commands the shell handles itself instead of running a program.

#include <stdlib.h>
#include <string.h>

#include "builtins.h"

int is_exit_command(const Pipeline *pl)
{
    return pl->number_of_commands == 1 && strcmp(pl->cmds[0].argv[0], "exit") == 0;
}

int exit_command_status(const Pipeline *pl)
{
    const char *arg = pl->cmds[0].argv[1];
    return arg ? atoi(arg) : EXIT_SUCCESS;
}