typedef struct
{
    char **argv;
    int argc;
} parsed_line_t;

int parse_line(const char *line, parsed_line_t *out);
void free_parsed(parsed_line_t *p);
