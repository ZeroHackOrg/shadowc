/* Equivalence fixture: recursive + iterative fibonacci, 64-bit math. */
#include <stdio.h>

static unsigned long fib_rec(unsigned long n) {
    if (n < 2) return n;
    return fib_rec(n - 1) + fib_rec(n - 2);
}

int main(void) {
    unsigned long a = 0, b = 1;
    unsigned long it = 0;
    for (int i = 0; i < 32; ++i) { unsigned long t = a + b; a = b; b = t; it = t; }
    printf("fib_rec20=%lu fib_it32=%lu\n", fib_rec(20), it);
    return 0;
}
