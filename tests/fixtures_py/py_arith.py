def fib(n):
    a, b = 0, 1
    for _ in range(n):
        a, b = b, a + b
    return a


def mix(x):
    return sum(i * i for i in range(1, x + 1))


print("pyfib40=%d mix=%d" % (fib(40), mix(12)))
