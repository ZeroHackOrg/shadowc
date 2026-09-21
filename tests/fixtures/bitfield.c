/* Equivalence fixture: bitfields, masks, unions, shift patterns. */
#include <stdio.h>

struct flags {
    unsigned a : 3;
    unsigned b : 5;
    unsigned c : 9;
    unsigned d : 15;
};

union tricky { unsigned v; unsigned char b[4]; };

int main(void) {
    struct flags f = {1u, 3u, 0x1ffu, 0x7fffu};
    unsigned packed = (f.a << 29) | (f.b << 24) | ((f.c & 0x1) << 23) | f.d;
    union tricky t = {0xDEADBEEFu};
    unsigned rev = (t.b[3] << 24) | (t.b[2] << 16) | (t.b[1] << 8) | t.b[0];
    printf("packed=%08x rev=%08x sum=%u\n", packed, rev, packed + rev);
    return 0;
}
