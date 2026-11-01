#include "../src/restart_watchdog.h"
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

int main()
{
    using W = inq::RestartWatchdog;
    using L = W::Loading;
    W w;
    CHECK(!w.Step(0, 1, L::Idle, true));
    CHECK(!w.Step(11999, 1, L::Idle, true));
    CHECK(w.Step(12000, 1, L::Idle, true));
    w.Reset();
    CHECK(!w.Step(12000, 1, L::Idle, true)); // new request, same stage
    CHECK(!w.Step(23000, 2, L::Idle, true)); // stage progress
    CHECK(!w.Step(34000, 3, L::Idle, true));
    CHECK(!w.Step(45000, 4, L::Idle, true));
    CHECK(w.Step(57000, 4, L::Idle, true));

    CHECK(!w.Step(57001, 0, L::Idle, true)); // completion/cancel
    CHECK(!w.Step(100000, 4, L::Idle, true));
    CHECK(!w.Step(111999, 4, L::Idle, true));
    CHECK(!w.Step(112000, 4, L::Busy, true));
    CHECK(!w.Step(1000000, 4, L::Busy, true)); // stuck disk never times out
    CHECK(!w.Step(1000001, 4, L::Idle, true));
    CHECK(!w.Step(1012000, 4, L::Idle, true));
    CHECK(w.Step(1012001, 4, L::Idle, true)); // full fresh idle window

    CHECK(!w.Step(1012002, 4, L::Unknown, true));
    CHECK(!w.Step(2000000, 4, L::Unknown, true)); // missing UI/read failure
    CHECK(!w.Step(2000001, 4, L::Idle, true));
    CHECK(!w.Step(2012000, 4, L::Idle, true));
    CHECK(w.Step(2012001, 4, L::Idle, true));
    CHECK(!w.Step(2012002, 4, L::Idle, false)); // disabled
    CHECK(!w.Step(3000000, 4, L::Idle, true));
    CHECK(!w.Step(1, 4, L::Idle, true)); // defensive clock rollback
    CHECK(w.Step(12001, 4, L::Idle, true));
    w.Reset();
    CHECK(!w.Step(50000, 1, L::Idle, true));
    CHECK(!w.Step(60000, 2, L::Busy, true));
    CHECK(!w.Step(90000, 3, L::Busy, true));
    CHECK(!w.Step(100000, 3, L::Idle, true));
    CHECK(w.Step(112000, 3, L::Idle, true));
    std::printf("restart_watchdog_test: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
