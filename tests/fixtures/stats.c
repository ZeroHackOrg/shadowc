/* Equivalence fixture: nested loops, arrays, modulo arithmetic. */
#include <stdio.h>

static int acc(int n, int m) {
    int s = 0;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < m; ++j)
            s = (s + (i * j) % 97) % 251;
    return s;
}

int main(void) {
    int s = 0;
    for (int k = 1; k <= 12; ++k) s += acc(k, k + 1);
    printf("stats=%d\n", s);
    return 0;
}
