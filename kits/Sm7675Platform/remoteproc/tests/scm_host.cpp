/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Execute the actual shared SCM implementation with a scripted secure world. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <mutex>
#include <deque>
#include <vector>
#include <thread>
#include <functional>
#include <atomic>
using event_t = void *;
using wait_result_t = int;
using thread_t = void *;
constexpr unsigned THREAD_UNINT = 0, THREAD_CONTINUE_NULL = 0, kMillisecondScale = 1;
constexpr int THREAD_AWAKENED = 1, THREAD_TIMED_OUT = 2, LCK_SLEEP_DEFAULT = 0, KERN_SUCCESS = 0;
#define LCK_GRP_DECLARE(name, label) [[maybe_unused]] int name
#define LCK_MTX_DECLARE(name, group) std::mutex name
static unsigned checks, attached, sleeps;
static void (*irqHandler)(void *);
static std::function<void()> onSleep;
#define CHECK(x) do {++checks; assert(x);} while (0)
static void lck_mtx_lock(std::mutex *m) {m->lock();}
static void lck_mtx_unlock(std::mutex *m) {m->unlock();}
static void thread_wakeup(event_t) {}
static void assert_wait(event_t, unsigned) {}
static void thread_set_thread_name(thread_t, const char *) {}
static thread_t current_thread() {return nullptr;}
static void clear_wait(thread_t, int) {}
static void thread_block(unsigned) {}
static void thread_deallocate(thread_t) {}
static int kernel_thread_start(void (*)(void *, wait_result_t), void *, thread_t *t) {*t = nullptr; return 0;}
static void *IOMallocZero(size_t bytes) {return calloc(1, bytes);}
static void IOSleep(unsigned ms) {CHECK(ms == 30); ++sleeps;}
static void clock_interval_to_deadline(unsigned ms, unsigned, uint64_t *out) {*out = ms;}
static int lck_mtx_sleep_deadline(std::mutex *m, int, event_t, unsigned, uint64_t) {
    m->unlock();
    const bool awaken = bool(onSleep);
    if (onSleep) {auto callback = std::move(onSleep); onSleep = nullptr; callback();}
    m->lock();
    return awaken ? THREAD_AWAKENED : THREAD_TIMED_OUT;
}
extern "C" int armcpu_spi_attach(uint32_t intid, bool edge, void (*handler)(void *), void *, uint32_t *, uint64_t *) {
    CHECK(intid == 962 && edge && !irqHandler);
    ++attached; irqHandler = handler; return 0;
}
#include "../Sm7675Scm.cpp"
namespace {
struct Step {Frame frame; Reply reply;};
std::deque<Step> script;
std::vector<Frame> observed;
std::mutex scriptMutex;
std::atomic<unsigned> inSecure{0}, maxSecure{0};
Reply sm7675_scm_test_invoke(const Frame &frame) {
    const unsigned active = ++inSecure;
    unsigned previous = maxSecure.load();
    while (previous < active && !maxSecure.compare_exchange_weak(previous, active)) {}
    std::this_thread::yield();
    std::lock_guard<std::mutex> lock(scriptMutex);
    observed.push_back(frame);
    CHECK(!script.empty());
    const auto step = script.front(); script.pop_front();
    CHECK(frame.fn == step.frame.fn && frame.info == step.frame.info && frame.a == step.frame.a &&
          frame.b == step.frame.b && frame.c == step.frame.c && frame.d == step.frame.d &&
          frame.session == step.frame.session && frame.a7 == step.frame.a7);
    --inSecure;
    return step.reply;
}
}
static Frame pas() {return {qcom_scm_fn(2, 1), qcom_scm_args(2, 0, 2, 0), 36, 0x90000000, 0, 0, 0, 0};}
static void add(Frame frame, int64_t status = 0, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0, uint64_t session = 0) {
    script.push_back({frame, {{status, a, b, c}, session}});
}
static Sm7675ScmResult call() {auto f = pas(); return sm7675_scm_call(2, 1, (uint32_t)f.info, f.a, f.b, f.c);}
static void reset() {
    CHECK(script.empty());
    while (gQueues) {auto *next = gQueues->next; free(gQueues); gQueues = next;}
    gPoisoned = false; gWork = 0; observed.clear(); sleeps = 0; onSleep = nullptr;
}
int main() {
    add(waitFrame(kWaitInfo, 0, true), 0, 2, 962);
    add(pas()); CHECK(!call().x0 && attached == 1 && gReady);
    reset();
    // Interrupted SMC retains the QCOM_A6 session and original arguments.
    add(pas(), 1, 0, 0, 0, 0xabcdef);
    auto interrupted = pas(); interrupted.fn = 1; interrupted.session = 0xabcdef;
    add(interrupted); CHECK(!call().x0);
    reset();
    // Sleep: IRQ only queues work, GET_CTX runs outside the normal call lock.
    add(pas(), 2, 7, 0x42);
    add(waitFrame(kWaitGet, 0, true), 0, 7, 1);
    add(waitFrame(kWaitResume, 0x42));
    onSleep = [] {irqHandler(nullptr); CHECK(gWork == 1); gWork = 0; serviceWaitQueues();};
    CHECK(!call().x0 && !gPoisoned && observed.size() == 3);
    reset();
    // Wake: deliver completion before ACK; a later sleep consumes its token.
    add(pas(), 3, 9, 0x66, 1);
    add(waitFrame(kWaitAck, 0x66), 2, 9, 0x67);
    add(waitFrame(kWaitResume, 0x67));
    CHECK(!call().x0 && !gPoisoned);
    reset();
    // complete_all releases existing waiters and does not credit future ones.
    onSleep = [] {CHECK(!notify(12, 2));};
    CHECK(!await(12)); CHECK(await(12) == -ETIMEDOUT);
    reset();
    CHECK(!notify(13, 1) && !await(13));
    CHECK(!notify(13, 2)); CHECK(await(13) == -ETIMEDOUT);
    reset();
    // More-pending GET entries are drained; wake-one tokens remain independent.
    add(waitFrame(kWaitGet, 0, true), 0, 10, 1, 1);
    add(waitFrame(kWaitGet, 0, true), 0, 11, 1, 0);
    serviceWaitQueues(); CHECK(!await(10) && !await(11));
    reset();
    // Busy after WAITQ_ACK must retry the original PAS, never repeat ACK.
    add(pas(), 3, 15, 77, 1);
    add(waitFrame(kWaitAck, 77), -12);
    add(pas()); CHECK(!call().x0 && sleeps == 1);
    reset();
    // A missing IRQ is uncertain ownership: poison all subsequent normal calls.
    add(pas(), 2, 1, 8);
    CHECK(call().x0 == -ETIMEDOUT && gPoisoned);
    const auto old = observed.size(); CHECK(call().x0 == -EIO && observed.size() == old);
    reset();
    add(pas(), 3, 1, 8, 99); CHECK(call().x0 == -EPROTO && gPoisoned);
    reset();
    add(pas(), 2, UINT64_MAX, 0); CHECK(call().x0 == -EPROTO && gPoisoned);
    reset();
    // Concurrent ADSP/WPSS callers share one non-atomic serialization lock.
    for (unsigned n = 0; n < 100; ++n) add(pas());
    std::thread a([] {for (unsigned n = 0; n < 50; ++n) assert(!call().x0);});
    std::thread b([] {for (unsigned n = 0; n < 50; ++n) assert(!call().x0);});
    a.join(); b.join(); CHECK(maxSecure == 1 && script.empty() && attached == 1);
    reset();
    printf("PASS shared SCM WAITQ/IRQ/ACK/RESUME/session/timeout/concurrency: %u checks\n", checks);
}
