/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Execute the shared runtime against modeled MMIO and locks. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <mutex>
#include <deque>
#include <vector>
#include <thread>
#include "../QcomRpmh.h"

using event_t = void *;
using IOOptionBits = unsigned;
constexpr unsigned kIOMapAnywhere = 1, kIOMapInhibitCache = 2;
constexpr unsigned THREAD_UNINT = 0, THREAD_CONTINUE_NULL = 0, kMillisecondScale = 1;
static uint64_t gNow;
static uint32_t ipcc[1024], aoss[1024], rsc[16384];
static std::deque<uint32_t> incoming;
static std::vector<std::pair<uint32_t, uint32_t>> sent;
static std::vector<std::pair<unsigned, unsigned>> votes;
static void (*handlers[300])(void *);
static unsigned attachments[300], queries, checks;
static bool stallQmp, stallRsc;
#define CHECK(x) do {++checks; assert(x);} while (0)
#define LCK_GRP_DECLARE(name, label) [[maybe_unused]] int name
#define LCK_MTX_DECLARE(name, group) std::mutex name
static void lck_mtx_lock(std::mutex *m) {m->lock();}
static void lck_mtx_unlock(std::mutex *m) {m->unlock();}
static void thread_wakeup(event_t) {}
static void assert_wait_timeout(event_t, unsigned, unsigned, unsigned) {}
static void IODelay(unsigned) {}
static uint64_t mach_absolute_time() {return gNow;}
static void clock_interval_to_deadline(unsigned ms, unsigned, uint64_t *out) {*out = gNow + ms;}

struct IOMemoryMap {
    uintptr_t address;
    uintptr_t getVirtualAddress() {return address;}
};
struct IODeviceMemory {
    uintptr_t address;
    static IODeviceMemory *withRange(uint64_t address, uint64_t) {
        static IODeviceMemory objects[] = {{(uintptr_t)ipcc}, {(uintptr_t)aoss}, {(uintptr_t)rsc}};
        if (address == 0x406000) return &objects[0];
        if (address == 0xc300000) return &objects[1];
        if (address == 0x17a20000) return &objects[2];
        return nullptr;
    }
    IOMemoryMap *map(IOOptionBits) {
        static IOMemoryMap mappings[3];
        for (auto &m : mappings) if (!m.address || m.address == address) {m.address = address; return &m;}
        return nullptr;
    }
    void release() {}
};
extern "C" int armcpu_spi_attach(uint32_t n, bool, void (*handler)(void *), void *, uint32_t *, uint64_t *) {
    CHECK(n < 300 && !handlers[n]);
    handlers[n] = handler;
    ++attachments[n];
    return 0;
}
uint32_t sm7675_remote_test_read(volatile uint32_t *base, unsigned at) {
    if (base == ipcc && at == 0x10) return incoming.empty() ? 0xffffffffu : incoming.front();
    return base[at / 4];
}
void sm7675_remote_test_write(volatile uint32_t *base, unsigned at, uint32_t value) {
    base[at / 4] = value;
    if (base == ipcc && at == 0x1c && !incoming.empty()) {
        CHECK(incoming.front() == value);
        incoming.pop_front();
    }
    if (base == ipcc && at == 0xc) sent.emplace_back(value >> 16, value & 0xffff);
    if (base == rsc && at == 0xd00 + qcom_rsc_layout(rsc[0]).irq_clear)
        base[(0xd00 + qcom_rsc_layout(rsc[0]).irq_status) / 4] &= ~value;
}
static void thread_block(unsigned) {
    gNow += 5;
    if (aoss[0x80 / 4] && !stallQmp) {aoss[0x80 / 4] = 0; ++queries;}
    const auto layout = qcom_rsc_layout(rsc[0]);
    const unsigned control = 0xd00 + layout.tcs_stride + layout.control;
    if ((rsc[control / 4] & (1u << 24)) && !stallRsc) {
        const unsigned enabled = rsc[(0xd00 + layout.tcs_stride + layout.cmd_enable) / 4];
        for (unsigned i = 0; i < 4 && (enabled & (1u << i)); ++i)
            votes.emplace_back(rsc[(0xd00 + layout.tcs_stride + layout.cmd_stride * i + layout.cmd_addr) / 4],
                               rsc[(0xd00 + layout.tcs_stride + layout.cmd_stride * i + layout.cmd_data) / 4]);
        rsc[(0xd00 + layout.irq_status) / 4] |= 2;
        handlers[37](nullptr);
    }
}
#include "../Sm7675RemoteProc.cpp"

static unsigned wpssSignals, adspSignals;
static void wpss(uint32_t signal) {wpssSignals |= 1u << signal;}
static void adsp(uint32_t signal) {adspSignals |= 1u << signal;}

int main(int argc, char **) {
    rsc[0] = argc > 1 ? 3u << 16 : 0;
    CHECK(sm7675_remote_attach(static_cast<Sm7675Remote>(2), wpss) == EINVAL);
    CHECK(!sm7675_remote_attach(Sm7675Remote::Wpss, wpss));
    CHECK(!sm7675_remote_attach(Sm7675Remote::Adsp, adsp));
    CHECK(attachments[261] == 1);
    incoming = {(24u << 16) | 2, (3u << 16), (3u << 16) | 2, 0};
    handlers[261](nullptr);
    CHECK(incoming.empty() && wpssSignals == 4 && adspSignals == 5);
    sm7675_remote_ring(Sm7675Remote::Adsp, 2);
    CHECK((sent.back() == std::pair<unsigned, unsigned>(3, 2)));

    // Model a descriptor inherited open from the bootloader.
    aoss[0] = QCOM_QMP_MESSAGE ? 0x4d41494c : 0;
    aoss[1] = 1;
    for (unsigned i : {3u, 4u, 5u, 6u, 9u, 10u, 11u, 12u}) aoss[i] = 0xffff;
    aoss[13] = 128; aoss[14] = 0x80;
    CHECK(!sm7675_remote_qmp("{class: image, name: wpss, val: on}"));
    CHECK(!sm7675_remote_qmp("{class: image, name: adsp, val: on}"));
    CHECK(queries == 2);
    stallQmp = true;
    CHECK(sm7675_remote_qmp("timeout") == ETIMEDOUT);
    uint32_t outstanding = aoss[0x84 / 4];
    CHECK(sm7675_remote_qmp("must not overwrite") == ETIMEDOUT);
    CHECK(aoss[0x84 / 4] == outstanding && queries == 2);
    stallQmp = false;
    CHECK(!sm7675_remote_qmp("acknowledged old message then new"));
    CHECK(queries == 4);
    auto concurrent = [] {
        for (unsigned i = 0; i < 100; ++i)
            assert(!sm7675_remote_qmp("{class: image, val: on}"));
    };
    std::thread left(concurrent), right(concurrent);
    left.join();
    right.join();
    CHECK(queries == 204);

    Sm7675RpmhVote w[] = {{0x30000, 3}, {0x30004, 4}, {0x30008, 3}};
    Sm7675RpmhVote a[] = {{0x30000, 6}, {0x3000c, 5}};
    const auto layout = qcom_rsc_layout(rsc[0]);
    const unsigned control = (0xd00 + layout.tcs_stride + layout.control) / 4;
    rsc[control] = 1u << 24;
    CHECK(sm7675_remote_vote(Sm7675Remote::Wpss, w, 3) == EBUSY);
    rsc[(0xd00 + layout.irq_status) / 4] = 2;
    handlers[37](nullptr);
    CHECK(rsc[(0xd00 + layout.irq_status) / 4] == 2);
    rsc[control] = 0;
    rsc[(0xd00 + layout.irq_status) / 4] = 0;
    CHECK(!sm7675_remote_vote(Sm7675Remote::Wpss, w, 3));
    CHECK(attachments[37] == 1 && votes.size() == 3);
    CHECK(!sm7675_remote_vote(Sm7675Remote::Adsp, a, 2));
    CHECK(votes.size() == 5 && votes[3].second == 6);
    CHECK(!sm7675_remote_vote(Sm7675Remote::Adsp, nullptr, 0));
    CHECK(votes.size() == 7 && votes[5].second == 3 && votes[6].second == 0);
    CHECK(!sm7675_remote_vote(Sm7675Remote::Adsp, nullptr, 0));
    CHECK(votes.size() == 7);
    CHECK(!sm7675_remote_vote(Sm7675Remote::Wpss, w, 3));
    CHECK(votes.size() == 7);
    CHECK(sm7675_remote_vote(Sm7675Remote::Adsp, a, 4) == EINVAL);
    a[1] = a[0];
    CHECK(sm7675_remote_vote(Sm7675Remote::Adsp, a, 2) == EINVAL);
    a[1] = {0x3000c, 5};
    stallRsc = true;
    CHECK(sm7675_remote_vote(Sm7675Remote::Adsp, a, 2) == ETIMEDOUT);
    stallRsc = false;
    CHECK(sm7675_remote_vote(Sm7675Remote::Adsp, a, 2) == EBUSY);
    CHECK(votes.size() == 7);
    printf("PASS shared IPCC, QMP and RPMh runtime: %u checks\n", checks);
}
