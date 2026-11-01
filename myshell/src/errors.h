// errors.h — error reporting functions for myshell.

#ifndef ERRORS_H
#define ERRORS_H

// Print a fixed, user-facing message followed by a newline. 
void shell_error(const char *message);

// Print "<message>: <name>" followed by a newline, e.g. "Command not found: foo".
void shell_error_arg(const char *message, const char *name);

// Print "myshell: <what>: <strerror(errno)>". 
void shell_perror(const char *what);

#endif
