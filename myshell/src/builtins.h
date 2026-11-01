// builtins.h — commands the shell handles itself instead of running a program.

#ifndef BUILTINS_H
#define BUILTINS_H

#include "parser.h"

int is_exit_command(const Pipeline *pl);

// the status requested by "exit [n]"; EXIT_SUCCESS when no number is given.
int exit_command_status(const Pipeline *pl);

#endif