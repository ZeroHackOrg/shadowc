/* Equivalence fixture: switch-heavy state machine (CFF lowerSwitch stress). */
#include <stdio.h>

static int step(int s, int x) {
    switch (s) {
    case 0: return x < 0 ? 1 : 2;
    case 1: return x & 1 ? 3 : 4;
    case 2: return x > 100 ? 5 : (x & 3);
    case 3: return 0;
    case 4: return x % 7;
    case 5: return (x * x) & 0x1f;
    default: return 9;
    }
}

int main(void) {
    int s = 0;
    for (int i = -5; i <= 5; ++i) s = step(s, s + i);
    printf("switch=%d\n", s);
    return 0;
}
