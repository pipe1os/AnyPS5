#include "SceTypes.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern "C" {
int APS5_VABI ksem_init_nid_postfix(std::intptr_t* id, unsigned int value);
int APS5_VABI ksem_open_nid_postfix(std::intptr_t* id, const char* name, int flags, std::uint16_t mode, unsigned int value);
int APS5_VABI ksem_unlink_nid_postfix(const char* name);
int APS5_VABI ksem_close_nid_postfix(std::intptr_t id);
int APS5_VABI ksem_destroy_nid_postfix(std::intptr_t id);
int APS5_VABI ksem_post_nid_postfix(std::intptr_t id);
int APS5_VABI ksem_wait_nid_postfix(std::intptr_t id);
int APS5_VABI ksem_timedwait_nid_postfix(std::intptr_t id, const KernelTimespec* abstime);
int APS5_VABI ksem_trywait_nid_postfix(std::intptr_t id);
int APS5_VABI ksem_getvalue_nid_postfix(std::intptr_t id, int* value);
int APS5_VABI close_nid_postfix(int d);
int APS5_VABI clock_gettime_nid_postfix(int clockId, KernelTimespec* tp);
int* APS5_VABI __error_nid_postfix();
}

namespace {

constexpr int ENOENT_GUEST = 2;
constexpr int EFAULT_GUEST = 14;
constexpr int EBUSY_GUEST = 16;
constexpr int EEXIST_GUEST = 17;
constexpr int EINVAL_GUEST = 22;
constexpr int ENOSPC_GUEST = 28;
constexpr int EAGAIN_GUEST = 35;
constexpr int ETIMEDOUT_GUEST = 60;
constexpr int ENAMETOOLONG_GUEST = 63;
constexpr int EOVERFLOW_GUEST = 84;
constexpr int O_CREAT_GUEST = 0x200;
constexpr int O_EXCL_GUEST = 0x800;
constexpr unsigned int VALUE_MAX = 0x7fffffffu;

void Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::abort();
    }
}

bool Fails(int result, int error) {
    return result == -1 && *__error_nid_postfix() == error;
}

int Value(std::intptr_t id) {
    int value = -1;
    Require(ksem_getvalue_nid_postfix(id, &value) == 0, "getvalue reads an open semaphore");
    return value;
}

KernelTimespec FromNow(std::int64_t milliseconds) {
    KernelTimespec now{};
    clock_gettime_nid_postfix(0, &now);
    std::int64_t nanos = now.tv_nsec + milliseconds * 1000000;
    now.tv_sec += nanos / 1000000000;
    now.tv_nsec = nanos % 1000000000;
    if (now.tv_nsec < 0) {
        now.tv_nsec += 1000000000;
        --now.tv_sec;
    }
    return now;
}

void Anonymous() {
    std::intptr_t id = 0x1234;
    Require(Fails(ksem_init_nid_postfix(&id, VALUE_MAX + 1), EINVAL_GUEST) && id == 0x1234, "a value above SEM_VALUE_MAX is EINVAL before anything is written");
    Require(Fails(ksem_init_nid_postfix(nullptr, 0), EFAULT_GUEST), "a null id pointer is EFAULT");
    Require(ksem_init_nid_postfix(&id, 2) == 0 && id >= 0, "init writes a descriptor");
    Require(Value(id) == 2, "init sets the value");
    Require(ksem_trywait_nid_postfix(id) == 0 && ksem_wait_nid_postfix(id) == 0 && Value(id) == 0, "waits take the value down");
    Require(Fails(ksem_trywait_nid_postfix(id), EAGAIN_GUEST) && Value(id) == 0, "trywait on zero is EAGAIN");
    Require(ksem_post_nid_postfix(id) == 0 && Value(id) == 1, "post adds one");
    Require(Fails(ksem_getvalue_nid_postfix(id, nullptr), EFAULT_GUEST), "getvalue into a null pointer is EFAULT");
    Require(Fails(ksem_close_nid_postfix(id), EINVAL_GUEST) && Value(id) == 1, "close refuses an unnamed semaphore");
    Require(ksem_destroy_nid_postfix(id) == 0, "destroy removes an unnamed semaphore");
    Require(Fails(ksem_post_nid_postfix(id), EINVAL_GUEST), "a destroyed id is EINVAL");
    Require(Fails(ksem_destroy_nid_postfix(id), EINVAL_GUEST), "destroying twice is EINVAL");
    Require(Fails(ksem_post_nid_postfix(-1), EINVAL_GUEST) && Fails(ksem_wait_nid_postfix(-1), EINVAL_GUEST), "a descriptor that is not open is EINVAL");

    Require(ksem_init_nid_postfix(&id, VALUE_MAX) == 0 && Value(id) == static_cast<int>(VALUE_MAX), "init accepts SEM_VALUE_MAX");
    Require(Fails(ksem_post_nid_postfix(id), EOVERFLOW_GUEST) && Value(id) == static_cast<int>(VALUE_MAX), "post at SEM_VALUE_MAX is EOVERFLOW");
    Require(ksem_destroy_nid_postfix(id) == 0, "the full semaphore is destroyed");
}

void Wait() {
    std::intptr_t id = -1;
    Require(ksem_init_nid_postfix(&id, 1) == 0, "init for the waits");
    const KernelTimespec badNanos{0, 1000000000};
    Require(Fails(ksem_timedwait_nid_postfix(id, &badNanos), EINVAL_GUEST) && Value(id) == 1, "an invalid tv_nsec is EINVAL even with a count");
    Require(Fails(ksem_timedwait_nid_postfix(-1, &badNanos), EINVAL_GUEST), "the timeout is checked before the descriptor");
    const KernelTimespec past = FromNow(-1000);
    Require(ksem_timedwait_nid_postfix(id, &past) == 0 && Value(id) == 0, "an expired timeout still takes an available count");
    Require(Fails(ksem_timedwait_nid_postfix(id, &past), ETIMEDOUT_GUEST), "an expired timeout on zero is ETIMEDOUT");
    const auto start = std::chrono::steady_clock::now();
    const KernelTimespec soon = FromNow(50);
    Require(Fails(ksem_timedwait_nid_postfix(id, &soon), ETIMEDOUT_GUEST), "a future timeout on zero is ETIMEDOUT");
    Require(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(40), "the timed wait blocks until the deadline");

    std::atomic<int> result = -2;
    std::thread waiter([&] { result = ksem_wait_nid_postfix(id); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Require(result.load() == -2, "wait blocks on zero");
    Require(Fails(ksem_destroy_nid_postfix(id), EBUSY_GUEST), "destroy with a waiter is EBUSY");
    Require(ksem_post_nid_postfix(id) == 0, "post wakes the waiter");
    waiter.join();
    Require(result.load() == 0 && Value(id) == 0, "the woken waiter takes the count");

    std::atomic<int> timed = -2;
    std::thread timedWaiter([&] {
        const KernelTimespec later = FromNow(10000);
        timed = ksem_timedwait_nid_postfix(id, &later);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Require(ksem_post_nid_postfix(id) == 0, "post wakes the timed waiter");
    timedWaiter.join();
    Require(timed.load() == 0 && Value(id) == 0, "the timed waiter takes the count before its deadline");
    Require(ksem_destroy_nid_postfix(id) == 0, "destroy after the waits");
}

void Named() {
    std::intptr_t first = -1;
    std::intptr_t second = -1;
    Require(Fails(ksem_open_nid_postfix(&first, "/aps5-ksem", 0x1, 0600, 0), EINVAL_GUEST), "flags other than O_CREAT and O_EXCL are EINVAL");
    Require(Fails(ksem_open_nid_postfix(&first, "/aps5-ksem", O_CREAT_GUEST, 0600, VALUE_MAX + 1), EINVAL_GUEST), "a value above SEM_VALUE_MAX is EINVAL");
    Require(Fails(ksem_open_nid_postfix(&first, "aps5-ksem", O_CREAT_GUEST, 0600, 0), EINVAL_GUEST), "a name without a leading slash is EINVAL");
    Require(Fails(ksem_open_nid_postfix(&first, nullptr, O_CREAT_GUEST, 0600, 0), EFAULT_GUEST), "a null name is EFAULT");
    const std::string longName = "/" + std::string(1023, 'n');
    Require(Fails(ksem_open_nid_postfix(&first, longName.c_str(), O_CREAT_GUEST, 0600, 0), ENAMETOOLONG_GUEST), "a name of MAXPATHLEN bytes is ENAMETOOLONG");
    Require(Fails(ksem_unlink_nid_postfix(longName.c_str()), ENAMETOOLONG_GUEST), "unlink applies the same length limit");
    const std::string longestName = longName.substr(0, 1023);
    Require(ksem_open_nid_postfix(&first, longestName.c_str(), O_CREAT_GUEST, 0600, 0) == 0, "a name of MAXPATHLEN - 1 bytes is accepted");
    Require(ksem_close_nid_postfix(first) == 0 && ksem_unlink_nid_postfix(longestName.c_str()) == 0, "the longest name is closed and unlinked");

    Require(Fails(ksem_open_nid_postfix(&first, "/aps5-ksem", 0, 0600, 0), ENOENT_GUEST), "a missing name without O_CREAT is ENOENT");
    Require(ksem_open_nid_postfix(&first, "/aps5-ksem", O_CREAT_GUEST, 0600, 3) == 0 && Value(first) == 3, "O_CREAT creates with the value");
    Require(ksem_open_nid_postfix(&second, "/aps5-ksem", O_CREAT_GUEST, 0600, 9) == 0 && second != first, "a second open gets its own descriptor");
    Require(Value(second) == 3, "an existing name keeps its value");
    Require(ksem_post_nid_postfix(second) == 0 && Value(first) == 4, "both descriptors reach the same semaphore");
    std::intptr_t refused = -1;
    Require(Fails(ksem_open_nid_postfix(&refused, "/aps5-ksem", O_CREAT_GUEST | O_EXCL_GUEST, 0600, 0), EEXIST_GUEST) && refused >= 0,
        "O_CREAT | O_EXCL on an existing name is EEXIST after the id is written");
    Require(Fails(ksem_post_nid_postfix(refused), EINVAL_GUEST), "the id written by a failed open is not a semaphore");
    Require(Fails(ksem_destroy_nid_postfix(first), EINVAL_GUEST), "destroy refuses a named semaphore");
    Require(Fails(ksem_unlink_nid_postfix("aps5-ksem"), ENOENT_GUEST), "unlink of an unknown name is ENOENT");
    Require(ksem_unlink_nid_postfix("/aps5-ksem") == 0, "unlink removes the name");
    Require(Fails(ksem_unlink_nid_postfix("/aps5-ksem"), ENOENT_GUEST), "a second unlink is ENOENT");
    Require(ksem_trywait_nid_postfix(first) == 0 && Value(second) == 3, "an unlinked semaphore stays usable while open");
    std::intptr_t third = -1;
    Require(ksem_open_nid_postfix(&third, "/aps5-ksem", O_CREAT_GUEST, 0600, 7) == 0 && Value(third) == 7 && Value(first) == 3,
        "reusing an unlinked name creates a new semaphore");
    Require(ksem_close_nid_postfix(first) == 0, "close releases a named descriptor");
    Require(Fails(ksem_post_nid_postfix(first), EINVAL_GUEST), "a closed descriptor is EINVAL");
    Require(close_nid_postfix(static_cast<int>(second)) == 0, "close() also closes a semaphore descriptor");
    Require(Fails(ksem_post_nid_postfix(second), EINVAL_GUEST), "a descriptor closed with close() is no longer a semaphore");
    Require(ksem_close_nid_postfix(third) == 0 && ksem_unlink_nid_postfix("/aps5-ksem") == 0, "the new semaphore is closed and unlinked");
}

void Limit() {
    std::vector<std::intptr_t> ids;
    for (int i = 0; i < 30; ++i) {
        std::intptr_t id = -1;
        Require(ksem_init_nid_postfix(&id, 0) == 0, "thirty semaphores fit SEM_NSEMS_MAX");
        ids.push_back(id);
    }
    std::intptr_t id = -1;
    Require(Fails(ksem_init_nid_postfix(&id, 0), ENOSPC_GUEST), "an unnamed semaphore past SEM_NSEMS_MAX is ENOSPC");
    Require(Fails(ksem_open_nid_postfix(&id, "/aps5-ksem-limit", O_CREAT_GUEST, 0600, 0), 23), "a named semaphore past SEM_NSEMS_MAX is ENFILE");
    Require(ksem_destroy_nid_postfix(ids.back()) == 0, "destroy frees a slot");
    ids.pop_back();
    Require(ksem_open_nid_postfix(&id, "/aps5-ksem-limit", O_CREAT_GUEST, 0600, 0) == 0, "the freed slot can be used");
    Require(ksem_close_nid_postfix(id) == 0, "the named semaphore stays while its name exists");
    Require(Fails(ksem_init_nid_postfix(&id, 0), ENOSPC_GUEST), "a linked name still counts");
    Require(ksem_unlink_nid_postfix("/aps5-ksem-limit") == 0 && ksem_init_nid_postfix(&id, 0) == 0, "unlinking the closed name frees its slot");
    ids.push_back(id);
    for (const auto open : ids) Require(ksem_destroy_nid_postfix(open) == 0, "every semaphore is destroyed");
}

}

int main(int argc, char** argv) {
    const std::string test = argc > 1 ? argv[1] : "";
    if (test == "anonymous") Anonymous();
    else if (test == "wait") Wait();
    else if (test == "named") Named();
    else if (test == "limit") Limit();
    else Require(false, "unknown test case");
    return 0;
}
