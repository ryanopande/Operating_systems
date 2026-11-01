// lineio.c — reading one command line from standard input.

#include <stdio.h>
#include <string.h>

#include "errors.h"
#include "lineio.h"

int read_line(char *buf, size_t size)
{
    if (fgets(buf, (int)size, stdin) == NULL)
        return 0;

    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') {
        buf[len - 1] = '\0';
    } else if (!feof(stdin)) {
        // the line did not fit
        int c;
        while ((c = getchar()) != '\n' && c != EOF)
            ;
        shell_error("Command line too long.");
        buf[0] = '\0';
    }
    return 1;
}