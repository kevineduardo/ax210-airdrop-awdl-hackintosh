// SPDX-License-Identifier: MIT
// Unit tests for the awdlctl argument parser (no IOKit calls are made).
#define AWDLCTL_NO_MAIN
#include <stdarg.h>
#include "../../tools/awdlctl/awdlctl.c"

static int failures;

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static int parse(struct awdlctl_cmd *c, int n, ...)
{
    char *argv[8] = { "awdlctl" };
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++)
        argv[i + 1] = va_arg(ap, char *);
    va_end(ap);
    return awdlctl_parse(n + 1, argv, c);
}

int main(void)
{
    struct awdlctl_cmd c;
    CHECK(parse(&c, 0) == 2);
    CHECK(parse(&c, 1, "show") == 0 && c.kind == KIND_NONE);
    CHECK(parse(&c, 2, "show", "x") == 2);
    CHECK(parse(&c, 2, "pump", "on") == 0 && c.kind == KIND_BOOL && c.value == 1 && !strcmp(c.key, "AWDLTxPumpOn"));
    CHECK(parse(&c, 2, "follow", "off") == 0 && c.value == 0 && !strcmp(c.key, "AWDLFollowPeerOn"));
    CHECK(parse(&c, 2, "hometx", "yes") == 2);
    CHECK(parse(&c, 2, "lead", "30000") == 0 && c.kind == KIND_NUM && c.value == 30000);
    CHECK(parse(&c, 2, "lead", "999") == 2);
    CHECK(parse(&c, 2, "lead", "60001") == 2);
    CHECK(parse(&c, 2, "lead", "12ab") == 2);
    CHECK(parse(&c, 2, "dwellx", "0") == 0 && c.value == 0);
    CHECK(parse(&c, 2, "dwellx", "201") == 2);
    CHECK(parse(&c, 2, "pumpus", "250") == 0 && !strcmp(c.key, "AWDLPumpUs"));
    CHECK(parse(&c, 2, "pumpus", "-1") == 2);
    CHECK(parse(&c, 2, "bogus", "1") == 2);
    CHECK(parse(&c, 1, "pump") == 2);
    if (failures) return 1;
    printf("awdlctl parser: all checks passed\n");
    return 0;
}
