/* Equivalence fixture: printf/string-heavy program (no string vault here). */
#include <stdio.h>
#include <string.h>

static const char *messages[] = {"alpha", "beta-gamma", "delta/epsilon/zeta"};

int main(void) {
    size_t total = 0;
    for (int i = 0; i < 3; ++i) total += strlen(messages[i]);
    printf("msgs=%zu %s-%s %s\n", total, messages[0], messages[1], messages[2]);
    return 0;
}
