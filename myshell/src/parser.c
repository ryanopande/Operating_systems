// parser.c — tokenizer and syntax checker for myshell two steps tokenizing and parsing.

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "errors.h"
#include "parser.h"

// kinds of token the tokenizer can produce.
typedef enum 
{
    TOK_WORD,        // a program name, argument or file name 
    TOK_PIPE,        // |   
    TOK_IN,          // <  
    TOK_OUT,         // >  
    TOK_OUT_APPEND,  // >> 
    TOK_ERR,         // 2> 
    TOK_ERR_APPEND   // 2>> 
} TokenType;

typedef struct 
{
    TokenType type;
    char     *text;   
} Token;


#define MAX_TOKENS (MAX_LINE / 2 + 1)

// characters that terminate a word and start an operator

static int is_operator_char(char c)
{
    return c == '|' || c == '<' || c == '>';
}

// copy one word starting at *cursor into the arena, removing surrounding quotes. 
static char *copy_word(const char **cursor, char **arena_pos)
{
    const char *read  = *cursor;
    char       *start = *arena_pos;
    char       *write = start;

    while (*read && !isspace((unsigned char)*read) && !is_operator_char(*read)) 
    {
        if (*read == '"' || *read == '\'') 
        {
            char quote = *read++;               // skip the opening quote
            while (*read && *read != quote)
                *write++ = *read++;
            if (*read != quote) 
            {
                shell_error("Unterminated quote.");
                return NULL;
            }
            read++;                             // skip the closing quote
        } else 
        {
            *write++ = *read++;
        }
    }

    *write++   = '\0';
    *cursor    = read;
    *arena_pos = write;
    return start;
}

// split `line` into tokens.

static int tokenize(const char *line, Token *tokens, Pipeline *pl)
{
    const char *p         = line;
    char       *arena_pos = pl -> arena;
    int         count     = 0;

    for (;;) 
    {
        while (*p && isspace((unsigned char)*p))
            p++;
        if (*p == '\0')
            break;

        if (count == MAX_TOKENS) 
        {
            shell_error("Too many tokens on command line.");
            return -1;
        }

        Token *tok = &tokens[count];
        tok -> text  = NULL;

        if (*p == '|') 
        {
            tok -> type = TOK_PIPE;
            p += 1;
        } 
        else if (*p == '<') 
        {
            tok -> type = TOK_IN;
            p += 1;
        }
        else if (*p == '>') 
        {
            tok -> type = (p[1] == '>') ? TOK_OUT_APPEND : TOK_OUT;
            p += (tok -> type == TOK_OUT_APPEND) ? 2 : 1;
        }
        else if (*p == '2' && p[1] == '>') 
        {
            tok -> type = (p[2] == '>') ? TOK_ERR_APPEND : TOK_ERR;
            p += (tok -> type == TOK_ERR_APPEND) ? 3 : 2;
        } 
        else 
        {
            tok -> type = TOK_WORD;
            tok -> text = copy_word(&p, &arena_pos);
            if (tok -> text == NULL)
                return -1;
        }
        count++;
    }
    return count;
}

// reset a Command so every field starts out "not set"
static void init_command(Command *cmd)
{
    memset(cmd, 0, sizeof *cmd);
    cmd -> argv[0] = NULL;
}

// the specification prescribes a distinct message for each redirection operator that is missing its file name.
static const char *missing_file_message(TokenType type)
{
    switch (type) 
    {
    case TOK_IN:
        return "Input file not specified.";
    case TOK_OUT:
    case TOK_OUT_APPEND:
        return "Output file not specified.";
    default: // TOK_ERR, TOK_ERR_APPEND
        return "Error output file not specified.";
    }
}

// translate the tokenizer's operator token into the redirection type stored on a Command.
static RedirType redir_type_of(TokenType type)
{
    switch (type) 
    {
    case TOK_IN:
        return REDIR_IN;
    case TOK_OUT:
        return REDIR_OUT;
    case TOK_OUT_APPEND:
        return REDIR_OUT_APPEND;
    case TOK_ERR:
        return REDIR_ERR;
    default: // TOK_ERR_APPEND
        return REDIR_ERR_APPEND;
    }
}

// append a redirection to `cmd`. nothing is overwritten: a real shell opens every
// redirection from left to right, so "echo A > first.txt > out.txt" must still create
// first.txt and "cat < missing.txt < input.txt" must fail on missing.txt. the executor
// replays this list in order, and for the same stream the last one wins automatically
// because its dup2() happens last.
static int add_redirection(Command *cmd, TokenType type, char *file)
{
    if (cmd -> number_of_redirs == MAX_REDIRS) 
    {
        shell_error("Too many redirections.");
        return -1;
    }

    Redirect *redir = &cmd -> redirs[cmd -> number_of_redirs++];
    redir -> type = redir_type_of(type);
    redir -> file = file;
    return 0;
}

int parse_line(const char *line, Pipeline *pl)
{
    Token tokens[MAX_TOKENS];
    int   number_of_tokens = tokenize(line, tokens, pl);

    if (number_of_tokens < 0)
        return PARSE_ERROR;
    if (number_of_tokens == 0)
        return PARSE_EMPTY;

    pl -> number_of_commands = 1;
    init_command(&pl -> cmds[0]);
    Command *cur = &pl -> cmds[0];

    for (int i = 0; i < number_of_tokens; i++) 
    {
        Token *tok = &tokens[i];

        switch (tok -> type) 
        {
        case TOK_WORD:
            if (cur -> argc == MAX_ARGS) 
            {
                shell_error("Too many arguments.");
                return PARSE_ERROR;
            }
            cur -> argv[cur -> argc++] = tok -> text;
            cur -> argv[cur -> argc]   = NULL; 
            break;

        case TOK_PIPE:
            if (cur -> argc == 0) 
            {
                shell_error(pl -> number_of_commands == 1 ? "Command missing before pipe."
                                        : "Empty command between pipes.");
                return PARSE_ERROR;
            }
            if (pl -> number_of_commands == MAX_CMDS) 
            {
                shell_error("Too many commands in pipeline.");
                return PARSE_ERROR;
            }
            cur = &pl -> cmds[pl -> number_of_commands++];
            init_command(cur);
            break;

        default:
            if (i + 1 >= number_of_tokens || tokens[i + 1].type != TOK_WORD) 
            {
                shell_error(missing_file_message(tok -> type));
                return PARSE_ERROR;
            }
            if (add_redirection(cur, tok -> type, tokens[++i].text) < 0)
                return PARSE_ERROR;
            break;
        }
    }

    if (cur -> argc == 0) 
    {
        shell_error(pl -> number_of_commands > 1 ? "Command missing after pipe."
                                : "Command not specified.");
        return PARSE_ERROR;
    }

    return PARSE_OK;
}
