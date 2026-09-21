#include <cstdio>
#include <string>
#include <vector>
#include <stdexcept>

static unsigned glob = 0x1234u;

static unsigned countMuls(unsigned n) {
    unsigned s = 0;
    for (unsigned i = 1; i <= n; ++i) s += i * i;
    return s;
}

static std::string buildPath(const std::string &base, unsigned id) {
    std::string out = base;
    for (int k = 0; k < 3; ++k) out += (k % 2) ? "-" : "/";
    out += std::to_string(id);
    return out;
}

int main() {
    std::vector<unsigned> v;
    for (unsigned i = 0; i < 5; ++i) v.push_back(i * glob);
    unsigned sum = countMuls(9);
    try {
        if (sum > 1000) throw std::runtime_error("big-sum");
        sum += 1;
    } catch (const std::exception &e) {
        printf("caught=%s ", e.what());
        sum -= 1;
    }
    std::string p = buildPath("rc", 42);
    unsigned total = sum;
    for (unsigned x : v) total += x;
    printf("sum=%u path=%s tot=%u\n", sum, p.c_str(), total);
    return 0;
}
