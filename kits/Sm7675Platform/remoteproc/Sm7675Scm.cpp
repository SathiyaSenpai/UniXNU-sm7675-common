// SPDX-License-Identifier: GPL-2.0-only
/* SCM call sequencing adapted from Uke OEM drivers/firmware/qcom_scm-smc.c
 * and qcom_scm.c, revision 5712080a4b0c0b1a879e6d145c601c649ddaddfd.
 * Copyright (c) 2015,2019 The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 * XNU supplies the mutex, completion wait, IRQ worker and raw SMCCC operation.
 */
#include <IOKit/IOLib.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <sys/errno.h>
#include "QcomScm.h"
#include "Sm7675Scm.h"

extern "C" int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *) __attribute__((weak_import));
extern "C" {volatile uint32_t native_scm_stats[12];}

namespace {
constexpr uint32_t kWaitService = 0x24, kWaitAck = 1, kWaitResume = 2, kWaitGet = 3, kWaitInfo = 4;
constexpr int64_t kWaitSleep = 2, kWaitWake = 3;
constexpr unsigned kWaitMs = 10000;
struct Frame {uint64_t fn, info, a, b, c, d, session, a7;};
struct Reply {Sm7675ScmResult result; uint64_t session;};
#ifdef SM7675_SCM_HOST
Reply sm7675_scm_test_invoke(const Frame &);
#endif
struct WaitQueue {
    WaitQueue *next;
    uint32_t id, credits, generation, waiters;
};
WaitQueue *gQueues;
bool gReady, gPoisoned;
volatile unsigned gWork;
LCK_GRP_DECLARE(gScmGroup, "sm7675-scm");
LCK_MTX_DECLARE(gInitLock, &gScmGroup);
LCK_MTX_DECLARE(gCallLock, &gScmGroup);
LCK_MTX_DECLARE(gQueueLock, &gScmGroup);

Frame waitFrame(uint32_t command, uint32_t context = 0, bool fast = false) {
    return {qcom_scm_fn(kWaitService, command) | (fast ? 1u << 31 : 0),
            command == kWaitGet || command == kWaitInfo ? 0u : 1u, context, 0, 0, 0, 0, 0};
}

Reply invoke(const Frame &frame) {
#ifdef SM7675_SCM_HOST
    return sm7675_scm_test_invoke(frame);
#else
    register uint64_t x0 __asm__("x0") = frame.fn, x1 __asm__("x1") = frame.info,
        x2 __asm__("x2") = frame.a, x3 __asm__("x3") = frame.b, x4 __asm__("x4") = frame.c,
        x5 __asm__("x5") = frame.d, x6 __asm__("x6") = frame.session, x7 __asm__("x7") = frame.a7;
    __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
                     :: "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
    return {{(int64_t)x0, x1, x2, x3}, x6};
#endif
}

Sm7675ScmResult raw(Frame frame) {
    for (unsigned resumed = 0; resumed < 100000; ++resumed) {
        const Reply reply = invoke(frame);
        if (reply.result.x0 != QCOM_SCM_INTERRUPTED) return reply.result;
        frame.fn = QCOM_SCM_INTERRUPTED;
        frame.session = reply.session;
    }
    __atomic_store_n(&gPoisoned, true, __ATOMIC_RELEASE);
    return {-ETIMEDOUT, 0, 0, 0};
}

WaitQueue *lookup(uint32_t id) {
    for (auto *queue = gQueues; queue; queue = queue->next)
        if (queue->id == id) return queue;
    auto *queue = static_cast<WaitQueue *>(IOMallocZero(sizeof(WaitQueue)));
    if (!queue) return nullptr;
    queue->id = id;
    queue->next = gQueues;
    gQueues = queue;
    return queue;
}

int notify(uint32_t id, uint32_t flags) {
    if (flags != 1 && flags != 2) return -EPROTO;
    lck_mtx_lock(&gQueueLock);
    WaitQueue *queue = lookup(id);
    int result = queue ? 0 : -ENOMEM;
    if (queue) {
        if (flags == 1) {
            if (queue->credits == UINT32_MAX) result = -EOVERFLOW;
            else ++queue->credits;
        } else {
            // complete_all followed by reinit_completion: release current
            // waiters without retaining a token for a future waiter.
            ++queue->generation;
        }
        thread_wakeup((event_t)queue);
    }
    lck_mtx_unlock(&gQueueLock);
    return result;
}

int await(uint32_t id) {
    lck_mtx_lock(&gQueueLock);
    WaitQueue *queue = lookup(id);
    if (!queue) {lck_mtx_unlock(&gQueueLock); return -ENOMEM;}
    const uint32_t generation = queue->generation;
    ++queue->waiters;
    uint64_t deadline;
    clock_interval_to_deadline(kWaitMs, kMillisecondScale, &deadline);
    int result = 0;
    while (!queue->credits && queue->generation == generation) {
        if (lck_mtx_sleep_deadline(&gQueueLock, LCK_SLEEP_DEFAULT, (event_t)queue,
                                   THREAD_UNINT, deadline) == THREAD_TIMED_OUT) {
            result = -ETIMEDOUT;
            break;
        }
    }
    if (!result && queue->credits) --queue->credits;
    --queue->waiters;
    lck_mtx_unlock(&gQueueLock);
    if (result) __atomic_store_n(&gPoisoned, true, __ATOMIC_RELEASE);
    return result;
}

void serviceWaitQueues() {
    for (unsigned count = 0; count < 32; ++count) {
        const auto reply = raw(waitFrame(kWaitGet, 0, true));
        if (reply.x0 || !reply.x2) return;
        if (reply.x1 > UINT32_MAX || reply.x2 > UINT32_MAX || notify((uint32_t)reply.x1, (uint32_t)reply.x2)) {
            __atomic_store_n(&gPoisoned, true, __ATOMIC_RELEASE);
            return;
        }
        __atomic_fetch_add(&native_scm_stats[5], 1u, __ATOMIC_RELAXED);
        if (!reply.x3) return;
    }
    __atomic_store_n(&gWork, 1u, __ATOMIC_RELEASE);
}

void irq(void *) {
    __atomic_fetch_add(&native_scm_stats[4], 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&gWork, 1u, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&gWork);
}

void worker(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "scm-waitq");
    for (;;) {
        if (__atomic_exchange_n(&gWork, 0u, __ATOMIC_ACQ_REL)) serviceWaitQueues();
        assert_wait((event_t)&gWork, THREAD_UNINT);
        if (__atomic_load_n(&gWork, __ATOMIC_ACQUIRE)) clear_wait(current_thread(), THREAD_AWAKENED);
        else thread_block(THREAD_CONTINUE_NULL);
    }
}

int ready() {
    lck_mtx_lock(&gInitLock);
    int error = 0;
    if (__atomic_load_n(&gPoisoned, __ATOMIC_ACQUIRE)) error = -EIO;
    else if (!gReady) {
        const auto info = raw(waitFrame(kWaitInfo, 0, true));
        const uint32_t contexts = (uint32_t)info.x1 & 0xff, intid = (uint32_t)info.x2 & 0xffff;
        if (info.x0 || !contexts || intid < 32 || intid >= 1020 || !armcpu_spi_attach) error = -ENXIO;
        else {
            const int attached = armcpu_spi_attach(intid, true, irq, nullptr, nullptr, nullptr);
            if (attached) error = attached;
            else {
                thread_t thread = nullptr;
                if (kernel_thread_start(worker, nullptr, &thread) != KERN_SUCCESS) error = -ENOMEM;
                else {thread_deallocate(thread); gReady = true;}
            }
        }
        native_scm_stats[0] = error ? (uint32_t)error : 1;
        native_scm_stats[1] = contexts;
        native_scm_stats[2] = intid;
        printf("UKE SCM: WAITQ contexts=%u intid=%u init=%d\n", contexts, intid, error);
        if (error) __atomic_store_n(&gPoisoned, true, __ATOMIC_RELEASE);
    }
    lck_mtx_unlock(&gInitLock);
    return error;
}

Sm7675ScmResult transact(const Frame &original) {
    Frame frame = original;
    for (unsigned replies = 0; replies < 100000; ++replies) {
        const auto result = raw(frame);
        if (result.x0 != kWaitSleep && result.x0 != kWaitWake) return result;
        if (result.x1 > UINT32_MAX || result.x2 > UINT32_MAX || result.x3 > UINT32_MAX) {
            __atomic_store_n(&gPoisoned, true, __ATOMIC_RELEASE);
            return {-EPROTO, 0, 0, 0};
        }
        const uint32_t queue = (uint32_t)result.x1, context = (uint32_t)result.x2;
        printf("UKE SCM: WAITQ op=%lld queue=%u context=%u flags=%llu\n", (long long)result.x0,
               queue, context, (unsigned long long)result.x3);
        int error;
        if (result.x0 == kWaitSleep) {
            __atomic_fetch_add(&native_scm_stats[6], 1u, __ATOMIC_RELAXED);
            error = await(queue);
            frame = waitFrame(kWaitResume, context);
        } else {
            __atomic_fetch_add(&native_scm_stats[7], 1u, __ATOMIC_RELAXED);
            error = notify(queue, (uint32_t)result.x3);
            frame = waitFrame(kWaitAck, context);
        }
        if (error) {
            __atomic_store_n(&gPoisoned, true, __ATOMIC_RELEASE);
            return {error, 0, 0, 0};
        }
    }
    __atomic_store_n(&gPoisoned, true, __ATOMIC_RELEASE);
    return {-ETIMEDOUT, 0, 0, 0};
}
} // namespace

Sm7675ScmResult sm7675_scm_call(uint32_t service, uint32_t command, uint32_t arginfo,
                               uint64_t a, uint64_t b, uint64_t c) {
    if (__atomic_load_n(&gPoisoned, __ATOMIC_ACQUIRE)) return {-EIO, 0, 0, 0};
    if (const int error = ready()) return {error, 0, 0, 0};
    const Frame original = {qcom_scm_fn(service, command), arginfo, a, b, c, 0, 0, 0};
    for (unsigned busy = 0; busy < 20; ++busy) {
        lck_mtx_lock(&gCallLock);
        const auto result = __atomic_load_n(&gPoisoned, __ATOMIC_ACQUIRE) ?
            Sm7675ScmResult{-EIO, 0, 0, 0} : transact(original);
        lck_mtx_unlock(&gCallLock);
        if (result.x0 != QCOM_SCM_V2_EBUSY) {
            printf("UKE SCM: terminal svc=%u cmd=%u pas=%llu x0=%lld x1=%llu\n", service, command,
                   (unsigned long long)a, (long long)result.x0, (unsigned long long)result.x1);
            return result;
        }
        IOSleep(30);
    }
    return {QCOM_SCM_V2_EBUSY, 0, 0, 0};
}
