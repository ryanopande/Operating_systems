// errors.c — error reporting functions for myshell.

#include "errors.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "errors.h"

void shell_error(const char *message)
{
    fprintf(stderr, "%s\n", message);
}

void shell_perror(const char *what)
{
    // strerror() is read before any other call could clobber errno. 
    fprintf(stderr, "myshell: %s: %s\n", what, strerror(errno));
}
