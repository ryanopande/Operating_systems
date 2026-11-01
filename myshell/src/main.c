#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "builtins.h"
#include "executor.h"
#include "lineio.h"
#include "parser.h"
#include "shell.h"

// entry point of the local shell (Phase 1): read a line, parse it, run it, repeat.
// the same parser and executor are reused by the server in Phase 2.
int main(void)
{
    static Pipeline pipeline;   // static: the pipeline holds large buffers, keep them off the stack
    char line[MAX_LINE];

    // Ctrl-C should stop the running command, not the shell itself. children put the
    // default handler back before exec (see executor.c), so they can still be interrupted.
    signal(SIGINT, SIG_IGN);

    for (;;) {
        fputs(PROMPT, stdout);
        fflush(stdout);   // the prompt has no newline, so flush it to show it right away

        // end of input (Ctrl-D): print a newline so the terminal prompt starts on a new line
        if (!read_line(line, sizeof line)) {
            putchar('\n');
            break;
        }

        // syntax errors were already printed by the parser; just show the next prompt
        // (empty lines also end up here)
        if (parse_line(line, &pipeline) != PARSE_OK)
            continue;

        // exit is handled here and not in a child process, because only the shell
        // itself can end the shell
        if (is_exit_command(&pipeline))
            return exit_command_status(&pipeline);

        execute_pipeline(&pipeline);
    }

    return EXIT_SUCCESS;
}