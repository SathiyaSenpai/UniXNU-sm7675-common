/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <sys/errno.h>

#include "QcomQmp.h"
#include "QcomRpmh.h"
#include "QcomScm.h"
#include "Sm7675RemoteProc.h"

extern "C" int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *) __attribute__((weak_import));

namespace {
// Use the established WPSS AP alias and AP RSC active TCS.
constexpr uint64_t kIpcc = 0x406000, kAoss = 0xc300000, kRsc = 0x17a20000;
constexpr unsigned kIpccIrq = 32 + 229, kRscIrq = 32 + 5, kRscOffset = 0xd00;
constexpr unsigned kTcs = 1, kTcsBit = 1u << kTcs;
constexpr unsigned kVoteSlots = 3;
constexpr uint32_t kClients[] = {24, QCOM_IPCC_CLIENT_LPASS};

using Doorbell = void (*)(uint32_t);
Doorbell gDoorbell[2];
IOMemoryMap *gIpccMap, *gAossMap, *gRscMap;
volatile uint32_t *gIpcc, *gRsc;
bool gIpccAttached, gRscAttached, gRscOwned, gRscUncertain, gQmpInitialized;
volatile unsigned gRscComplete;
unsigned gQmpEvent;
QcomQmp gQmp;
QcomRscLayout gRscLayout;
Sm7675RpmhVote gVotes[2][kVoteSlots];

LCK_GRP_DECLARE(gLockGroup, "sm7675-remoteproc");
LCK_MTX_DECLARE(gSmemLock, &gLockGroup);
LCK_MTX_DECLARE(gAttachLock, &gLockGroup);
LCK_MTX_DECLARE(gQmpLock, &gLockGroup);
LCK_MTX_DECLARE(gRscLock, &gLockGroup);

volatile uint32_t *map(IOMemoryMap *&mapping, uint64_t address, uint64_t length)
{
    if (!mapping) {
        IODeviceMemory *memory = IODeviceMemory::withRange(address, length);
        if (!memory) return nullptr;
        mapping = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
        memory->release();
    }
    return mapping ? reinterpret_cast<volatile uint32_t *>(mapping->getVirtualAddress()) : nullptr;
}

uint32_t read(volatile uint32_t *region, unsigned offset)
{
#ifdef SM7675_REMOTEPROC_HOST
    return sm7675_remote_test_read(region, offset);
#else
    return region[offset / 4];
#endif
}
void write(volatile uint32_t *region, unsigned offset, uint32_t value)
{
#ifdef SM7675_REMOTEPROC_HOST
    sm7675_remote_test_write(region, offset, value);
#else
    region[offset / 4] = value;
    __asm__ volatile("dsb sy" ::: "memory");
#endif
}

void ring(uint32_t client, uint32_t signal) { write(gIpcc, QCOM_IPCC_SEND, qcom_ipcc_id(client, signal)); }
void kickAop(void *) { ring(QCOM_IPCC_CLIENT_AOP, QCOM_IPCC_SIGNAL_GLINK_QMP); }

void ipccIrq(void *)
{
    for (unsigned n = 0; n < 32; ++n) {
        const uint32_t id = read(gIpcc, QCOM_IPCC_RECV_ID);
        if (id == QCOM_IPCC_NONE) break;
        write(gIpcc, QCOM_IPCC_RECV_CLEAR, id);
        if (id == qcom_ipcc_id(QCOM_IPCC_CLIENT_AOP, QCOM_IPCC_SIGNAL_GLINK_QMP)) {
            thread_wakeup((event_t)&gQmpEvent);
            continue;
        }
        const uint32_t client = id >> 16, signal = id & 0xffff;
        if (signal != QCOM_IPCC_SIGNAL_GLINK_QMP && signal != QCOM_IPCC_SIGNAL_SMP2P) continue;
        for (unsigned owner = 0; owner < 2; ++owner) {
            if (client != kClients[owner]) continue;
            if (const Doorbell callback = __atomic_load_n(&gDoorbell[owner], __ATOMIC_ACQUIRE)) callback(signal);
        }
    }
}

void rscIrq(void *)
{
    if (!(read(gRsc, kRscOffset + gRscLayout.irq_status) & kTcsBit)) return;
    __atomic_store_n(&gRscComplete, 1u, __ATOMIC_RELEASE);
    write(gRsc, kRscOffset + gRscLayout.irq_clear, kTcsBit);
    thread_wakeup((event_t)&gRscComplete);
}

void waitBriefly(event_t event)
{
    assert_wait_timeout(event, THREAD_UNINT, 5, kMillisecondScale);
    thread_block(THREAD_CONTINUE_NULL);
}

int waitQmp(uint32_t ms)
{
    uint64_t deadline;
    clock_interval_to_deadline(ms, kMillisecondScale, &deadline);
    do {
        const int state = qcom_qmp_step(&gQmp);
        if (state == QcomQmpOpen) return 0;
        if (state < 0) return EIO;
        waitBriefly((event_t)&gQmpEvent);
    } while (mach_absolute_time() < deadline);
    return ETIMEDOUT;
}

uint32_t valueFor(const Sm7675RpmhVote *votes, uint32_t address)
{
    for (unsigned i = 0; i < kVoteSlots; ++i)
        if (votes[i].address == address) return votes[i].value;
    return 0;
}

int applyVotes(const Sm7675RpmhVote *old, const Sm7675RpmhVote *next, const Sm7675RpmhVote *other)
{
    uint32_t addresses[2 * kVoteSlots], values[2 * kVoteSlots];
    unsigned changed = 0;
    const Sm7675RpmhVote *const sets[] = {old, next};
    for (const auto *set : sets) {
        for (unsigned i = 0; i < kVoteSlots && set[i].address; ++i) {
            const uint32_t address = set[i].address;
            const uint32_t shared = valueFor(other, address);
            const uint32_t before = valueFor(old, address);
            const uint32_t after = valueFor(next, address);
            const uint32_t previous = before > shared ? before : shared;
            const uint32_t wanted = after > shared ? after : shared;
            if (previous == wanted || (set == next && valueFor(old, address))) continue;
            addresses[changed] = address;
            values[changed++] = wanted;
        }
    }
    if (!changed) return 0;
    // Each owner has fixed resource addresses, so one replacement fits in a TCS request.
    if (changed > kVoteSlots) return EINVAL;
    const unsigned control = kRscOffset + gRscLayout.tcs_stride * kTcs + gRscLayout.control;
    if (gRscUncertain || (read(gRsc, control) & (1u << 24))) return EBUSY;
    if (!gRscOwned) {
        write(gRsc, kRscOffset, read(gRsc, kRscOffset) | kTcsBit);
        gRscOwned = true;
    }
    QcomRpmhWrite writes[20];
    const unsigned n = qcom_rpmh_active_request(&gRscLayout, kTcs, addresses, values, changed, writes, 20);
    if (!n) return EINVAL;
    __atomic_store_n(&gRscComplete, 0u, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < n; ++i) write(gRsc, kRscOffset + writes[i].offset, writes[i].value);
    uint64_t deadline;
    clock_interval_to_deadline(100, kMillisecondScale, &deadline);
    while (!__atomic_load_n(&gRscComplete, __ATOMIC_ACQUIRE) && mach_absolute_time() < deadline)
        waitBriefly((event_t)&gRscComplete);
    if (!__atomic_load_n(&gRscComplete, __ATOMIC_ACQUIRE)) {
        // The votes may already have reached the AOP. Do not replay or lower them from an unknown state.
        gRscUncertain = true;
        return ETIMEDOUT;
    }
    const unsigned count = qcom_rpmh_finish(&gRscLayout, kTcs, writes, 20);
    for (unsigned i = 0; i < count; ++i) write(gRsc, kRscOffset + writes[i].offset, writes[i].value);
    return 0;
}
} // namespace

int sm7675_remote_smem_alloc(volatile uint8_t *mutex, QcomSmem *smem, QcomSmemPartition *partition,
                             uint16_t item, uint32_t bytes)
{
    auto *reg = reinterpret_cast<volatile uint32_t *>(mutex);
    lck_mtx_lock(&gSmemLock);
    int rc = -EBUSY;
    for (unsigned attempt = 0; attempt < 1000; ++attempt) {
        write(reg, 0, 1);
        if (read(reg, 0) == 1) {
            rc = qcom_smem_alloc(smem, partition, item, bytes);
            write(reg, 0, 0);
            break;
        }
        IODelay(10);
    }
    lck_mtx_unlock(&gSmemLock);
    return rc;
}

int sm7675_remote_attach(Sm7675Remote owner, void (*doorbell)(uint32_t))
{
    if (static_cast<unsigned>(owner) >= 2 || !doorbell) return EINVAL;
    if (!armcpu_spi_attach) return ENXIO;
    lck_mtx_lock(&gAttachLock);
    int rc = 0;
    if (!gIpcc) gIpcc = map(gIpccMap, kIpcc, 0x1000);
    if (!gIpcc) rc = ENOMEM;
    if (!rc && !gIpccAttached) {
        rc = armcpu_spi_attach(kIpccIrq, false, ipccIrq, nullptr, nullptr, nullptr);
        gIpccAttached = !rc;
        if (!rc) write(gIpcc, QCOM_IPCC_RECV_ENABLE, qcom_ipcc_id(QCOM_IPCC_CLIENT_AOP, QCOM_IPCC_SIGNAL_GLINK_QMP));
    }
    if (!rc) {
        const unsigned i = static_cast<unsigned>(owner);
        __atomic_store_n(&gDoorbell[i], doorbell, __ATOMIC_RELEASE);
        write(gIpcc, QCOM_IPCC_RECV_ENABLE, qcom_ipcc_id(kClients[i], QCOM_IPCC_SIGNAL_SMP2P));
        write(gIpcc, QCOM_IPCC_RECV_ENABLE, qcom_ipcc_id(kClients[i], QCOM_IPCC_SIGNAL_GLINK_QMP));
    }
    lck_mtx_unlock(&gAttachLock);
    return rc;
}

void sm7675_remote_ring(Sm7675Remote owner, uint32_t signal)
{
    if (static_cast<unsigned>(owner) < 2 && gIpcc) ring(kClients[static_cast<unsigned>(owner)], signal);
}

int sm7675_remote_qmp(const char *message)
{
    if (!message || !gIpccAttached) return EINVAL;
    lck_mtx_lock(&gQmpLock);
    if (!gQmpInitialized) {
        if (auto *aoss = map(gAossMap, kAoss, 0x1000)) {
            qcom_qmp_init(&gQmp, aoss, 0x400, kickAop, nullptr);
            gQmpInitialized = true;
        }
    }
    int rc = gQmpInitialized ? waitQmp(4000) : ENOMEM;
    if (!rc) rc = qcom_qmp_send(&gQmp, message) == QcomQmpSending ? waitQmp(1000) : EBUSY;
    lck_mtx_unlock(&gQmpLock);
    return rc;
}

int sm7675_remote_vote(Sm7675Remote owner, const Sm7675RpmhVote *votes, unsigned count)
{
    if (static_cast<unsigned>(owner) >= 2 || count > kVoteSlots || (count && !votes)) return EINVAL;
    Sm7675RpmhVote next[kVoteSlots] = {};
    for (unsigned i = 0; i < count; ++i) {
        if (!votes[i].address || !votes[i].value || valueFor(next, votes[i].address)) return EINVAL;
        next[i] = votes[i];
    }
    lck_mtx_lock(&gRscLock);
    int rc = 0;
    if (!gRscAttached) {
        if (!gRsc) gRsc = map(gRscMap, kRsc, 0x10000);
        if (!gRsc || !armcpu_spi_attach) rc = ENXIO;
        else {
            gRscLayout = qcom_rsc_layout(read(gRsc, 0));
            rc = armcpu_spi_attach(kRscIrq, false, rscIrq, nullptr, nullptr, nullptr);
            gRscAttached = !rc;
        }
    }
    const unsigned i = static_cast<unsigned>(owner);
    if (!rc) rc = applyVotes(gVotes[i], next, gVotes[1 - i]);
    if (!rc) memcpy(gVotes[i], next, sizeof next);
    lck_mtx_unlock(&gRscLock);
    return rc;
}
