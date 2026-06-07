#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "parser.h"

static void skip_ws(const char **s)
{
    while (**s && isspace((unsigned char)**s))
        (*s)++;
}

static int append_arg(parsed_line_t *p, const char *start, size_t len)
{
    char *arg = (char *)malloc(len + 1);
    if (!arg)
        return -1;

    memcpy(arg, start, len);
    arg[len] = '\0';

    char **newv = (char **)realloc(p->argv, sizeof(char *) * (p->argc + 1));
    if (!newv)
    {
        free(arg);
        return -1;
    }

    p->argv = newv;
    p->argv[p->argc++] = arg;
    return 0;
}

int parse_line(const char *line, parsed_line_t *out)
{
    if (!line || !out)
    {
        errno = EINVAL;
        return -1;
    }

    out->argv = NULL;
    out->argc = 0;

    const char *s = line;
    skip_ws(&s);

    while (*s)
    {
        char buf[4096];
        size_t bi = 0;

        while (*s && !isspace((unsigned char)*s))
        {
            if (*s == '\'')
            {
                s++;
                while (*s && *s != '\'')
                {
                    if (bi + 1 >= sizeof(buf))
                    {
                        errno = E2BIG;
                        free_parsed(out);
                        return -1;
                    }
                    buf[bi++] = *s++;
                }
                if (*s != '\'')
                {
                    errno = EINVAL;
                    free_parsed(out);
                    return -1;
                }
                s++;
            }
            else if (*s == '"')
            {
                s++;
                while (*s && *s != '"')
                {
                    if (*s == '\\' && s[1])
                        s++;
                    if (bi + 1 >= sizeof(buf))
                    {
                        errno = E2BIG;
                        free_parsed(out);
                        return -1;
                    }
                    buf[bi++] = *s++;
                }
                if (*s != '"')
                {
                    errno = EINVAL;
                    free_parsed(out);
                    return -1;
                }
                s++;
            }
            else if (*s == '\\')
            {
                s++;
                if (*s)
                {
                    if (bi + 1 >= sizeof(buf))
                    {
                        errno = E2BIG;
                        free_parsed(out);
                        return -1;
                    }
                    buf[bi++] = *s++;
                }
            }
            else
            {
                if (bi + 1 >= sizeof(buf))
                {
                    errno = E2BIG;
                    free_parsed(out);
                    return -1;
                }
                buf[bi++] = *s++;
            }
        }

        if (append_arg(out, buf, bi) < 0)
        {
            free_parsed(out);
            return -1;
        }
        skip_ws(&s);
    }

    return 0;
}

void free_parsed(parsed_line_t *p)
{
    if (!p)
        return;

    for (int i = 0; i < p->argc; i++)
        free(p->argv[i]);
    free(p->argv);

    p->argv = NULL;
    p->argc = 0;
}
