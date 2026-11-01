// parser.h — turns a raw command line into a pipeline structure.

#ifndef PARSER_H
#define PARSER_H

#include "shell.h"

// the five redirection operators the shell understands
typedef enum
{
    REDIR_IN,          // <   read stdin from file
    REDIR_OUT,         // >   truncate file, write stdout to it
    REDIR_OUT_APPEND,  // >>  append stdout to file
    REDIR_ERR,         // 2>  truncate file, write stderr to it
    REDIR_ERR_APPEND   // 2>> append stderr to file
} RedirType;

// one redirection exactly as it was typed. they are stored in command-line order
// because a real shell opens them left to right: every file is created or opened
// (so "echo A > first.txt > out.txt" leaves an empty first.txt behind) and the
// first one that fails stops the command before it runs.
typedef struct Redirect
{
    RedirType type;
    char     *file;   // points into the pipeline arena
} Redirect;

typedef struct Command
{
    char *argv[MAX_ARGS + 1]; // null-terminated argument vector
    int   argc; // number of entries in argv before the null
    Redirect redirs[MAX_REDIRS]; // redirections in the order they were typed
    int   number_of_redirs; // number of entries used in redirs
} Command;


typedef struct Pipeline
{
    Command cmds[MAX_CMDS];
    int     number_of_commands;
    char    arena[2 * MAX_LINE];
} Pipeline;


enum
{
    PARSE_OK    = 0,   // pipeline is filled in and ready to run
    PARSE_EMPTY = 1,   // the line contained only whitespace
    PARSE_ERROR = -1   // malformed input; a message was already shown
};

int parse_line(const char *line, Pipeline *pl);

#endif
