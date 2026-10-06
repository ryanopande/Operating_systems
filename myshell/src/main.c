#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "errors.h"
#include "executor.h"
#include "parser.h"
#include "shell.h"


static int read_line(char *buf, size_t size)
{
    if (fgets(buf, (int)size, stdin) == NULL)
        return 0;

    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') {
        buf[len - 1] = '\0';
    } else if (!feof(stdin)) {
        int c;
        while ((c = getchar()) != '\n' && c != EOF)
            ;
        shell_error("Command line too long.");
        buf[0] = '\0';
    }
    return 1;
}

static int is_exit_builtin(const Pipeline *pl)
{
    return pl->number_of_commands == 1 && strcmp(pl->cmds[0].argv[0], "exit") == 0;
}

int main(void)
{
    static Pipeline pipeline;
    char line[MAX_LINE];

    signal(SIGINT, SIG_IGN);

    for (;;) {
        fputs(PROMPT, stdout);
        fflush(stdout);   

        if (!read_line(line, sizeof line)) {
            putchar('\n');   
            break;
        }

        if (parse_line(line, &pipeline) != PARSE_OK)
            continue;        

        if (is_exit_builtin(&pipeline)) {
            const char *arg = pipeline.cmds[0].argv[1];
            return arg ? atoi(arg) : EXIT_SUCCESS;
        }

        execute_pipeline(&pipeline);
    }

    return EXIT_SUCCESS;
}
