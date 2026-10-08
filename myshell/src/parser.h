// parser.h — turns a raw command line into a pipeline structure.

#ifndef PARSER_H
#define PARSER_H

#include "shell.h"

typedef struct Command 
{
    char *argv[MAX_ARGS + 1]; // null-terminated argument vector
    int   argc; // number of entries in argv before the null
    char *in_file; // target of "<"
    char *out_file; // target of ">" or ">>" git
    int   out_append; // 1 when ">>" was used, 0 for ">"
    char *err_file; // target of "2>" or "2>>"
    int   err_append; // 1 when "2>>" was used, 0 for "2>"
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
