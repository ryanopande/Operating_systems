// shell.h — constants shared by every module of myshell.

#ifndef SHELL_H
#define SHELL_H

// Longest command line accepted 
#define MAX_LINE 4096

// Maximum number of argv entries per command 
#define MAX_ARGS 128

// Maximum number of commands in one pipeline 
#define MAX_CMDS 64

// The prompt shown before every line is read
#define PROMPT "$ "

// Exit codes used by a child process that failed to execute

#define EXIT_REDIRECT_FAILED 1   // could not open a redirection target 
#define EXIT_CANNOT_EXEC     126 // found the program but could not run it
#define EXIT_NOT_FOUND       127 // program does not exist in PATH  

#endif 