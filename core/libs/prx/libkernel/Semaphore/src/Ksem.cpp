#include "prx/libkernel/Equeue/Equeue.hpp"
#include "prx/libc/include/CreationMask.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/File/include/FileFlags.hpp"
#include "prx/libkernel/Pthread/Posix/Common.hpp"
#include "prx/libkernel/Time/include/TimedWait.hpp"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

extern "C" int* APS5_VABI __error_nid_postfix();
extern "C" int APS5_VABI close_nid_postfix(int d);

namespace {

constexpr int GUEST_ENOENT = 2;
constexpr int GUEST_EFAULT = 14;
constexpr int GUEST_EBUSY = 16;
constexpr int GUEST_EEXIST = 17;
constexpr int GUEST_EINVAL = 22;
constexpr int GUEST_ENFILE = 23;
constexpr int GUEST_EMFILE = 24;
constexpr int GUEST_ENOSPC = 28;
constexpr int GUEST_EAGAIN = 35;
constexpr int GUEST_ETIMEDOUT = 60;
constexpr int GUEST_ENAMETOOLONG = 63;
constexpr int GUEST_EOVERFLOW = 84;
constexpr int GUEST_CLOCK_REALTIME = 0;

constexpr unsigned int GUEST_SEM_VALUE_MAX = 0x7fffffffu;
constexpr int GUEST_SEM_NSEMS_MAX = 30;
constexpr std::size_t GUEST_MAXPATHLEN = 1024;
constexpr std::uint16_t GUEST_ACCESSPERMS = 0777;
constexpr std::uint16_t GUEST_OWNER_READ_WRITE = 0600;
constexpr std::uint16_t GUEST_ANONYMOUS_MODE = 0770;

struct Ksem {
    unsigned int value = 0;
    std::uint16_t mode = 0;
    int waiters = 0;
    bool anonymous = false;
    bool dead = false;
    TimedWait::Condition available;
};

std::mutex g_ksemMutex;
std::map<int, std::shared_ptr<Ksem>> g_ksemDescriptors;
std::map<std::string, std::shared_ptr<Ksem>> g_ksemNames;
std::atomic<int> g_ksemCount = 0;

int Fail(int error) {
    *__error_nid_postfix() = error;
    return -1;
}

int ReserveDescriptor() {
#ifdef _WIN32
    return ::_open("NUL", _O_RDONLY | _O_BINARY);
#else
    return ::open("/dev/null", O_RDONLY | O_CLOEXEC);
#endif
}

void ReleaseDescriptor(int descriptor) {
#ifdef _WIN32
    ::_close(descriptor);
#else
    ::close(descriptor);
#endif
}

std::shared_ptr<Ksem> Allocate(std::uint16_t mode, unsigned int value, bool anonymous) {
    if (g_ksemCount.load() == GUEST_SEM_NSEMS_MAX) return nullptr;
    ++g_ksemCount;
    std::shared_ptr<Ksem> semaphore(new Ksem(), [](Ksem* released) {
        delete released;
        --g_ksemCount;
    });
    semaphore->value = value;
    semaphore->mode = mode;
    semaphore->anonymous = anonymous;
    return semaphore;
}

void RequireAccess(const Ksem& semaphore, const char* function) {
    if ((semaphore.mode & GUEST_OWNER_READ_WRITE) != GUEST_OWNER_READ_WRITE)
        throw std::runtime_error(std::string(function) + ": access to a semaphore whose mode denies the owner read or write depends on privilege");
}

int NameError(const char* name) {
    if (!name) return GUEST_EFAULT;
    if (strnlen(name, GUEST_MAXPATHLEN) == GUEST_MAXPATHLEN) return GUEST_ENAMETOOLONG;
    return 0;
}

std::shared_ptr<Ksem> Find(std::intptr_t id) {
    std::lock_guard lock(g_ksemMutex);
    const auto found = g_ksemDescriptors.find(static_cast<int>(id));
    if (found == g_ksemDescriptors.end() || found->second->dead) return nullptr;
    return found->second;
}

int Create(bool named, const char* name, std::intptr_t* id, std::uint16_t mode, unsigned int value, int flags) {
    if (value > GUEST_SEM_VALUE_MAX) return Fail(GUEST_EINVAL);
    mode = static_cast<std::uint16_t>(mode & ~File::GetCreationMask_nid_no_patch() & GUEST_ACCESSPERMS);
    const int descriptor = ReserveDescriptor();
    if (descriptor < 0) {
        const int error = errno == EMFILE ? GUEST_EMFILE : errno == ENFILE ? GUEST_ENFILE : 0;
        if (error == 0) throw std::runtime_error("ksem: reserving a descriptor failed, errno=" + std::to_string(errno));
        return Fail(named ? error : GUEST_ENOSPC);
    }
    if (!id) {
        ReleaseDescriptor(descriptor);
        return Fail(GUEST_EFAULT);
    }
    *id = descriptor;
    int error = named ? NameError(name) : 0;
    if (error == 0 && named && name[0] != '/') error = GUEST_EINVAL;
    if (error != 0) {
        ReleaseDescriptor(descriptor);
        return Fail(error);
    }
    std::unique_lock lock(g_ksemMutex);
    std::shared_ptr<Ksem> semaphore;
    if (!named) {
        semaphore = Allocate(mode, value, true);
        if (!semaphore) error = GUEST_ENOSPC;
    } else if (const auto found = g_ksemNames.find(name); found == g_ksemNames.end()) {
        if ((flags & SCE_KERNEL_O_CREAT) != 0) {
            semaphore = Allocate(mode, value, false);
            if (semaphore) g_ksemNames.emplace(name, semaphore);
            else error = GUEST_ENFILE;
        } else {
            error = GUEST_ENOENT;
        }
    } else if ((flags & (SCE_KERNEL_O_CREAT | SCE_KERNEL_O_EXCL)) == (SCE_KERNEL_O_CREAT | SCE_KERNEL_O_EXCL)) {
        error = GUEST_EEXIST;
    } else {
        RequireAccess(*found->second, "ksem_open");
        semaphore = found->second;
    }
    if (error != 0) {
        lock.unlock();
        ReleaseDescriptor(descriptor);
        return Fail(error);
    }
    g_ksemDescriptors[descriptor] = std::move(semaphore);
    return 0;
}

int Wait(std::intptr_t id, bool tryOnly, const KernelTimespec* abstime) {
    if (abstime && (abstime->tv_nsec < 0 || abstime->tv_nsec >= 1000000000)) return Fail(GUEST_EINVAL);
    const auto semaphore = Find(id);
    if (!semaphore) return Fail(GUEST_EINVAL);
    std::unique_lock lock(g_ksemMutex);
    while (semaphore->value == 0) {
        ++semaphore->waiters;
        int error = 0;
        if (tryOnly) {
            error = GUEST_EAGAIN;
        } else if (!abstime) {
            semaphore->available.Wait(lock);
        } else {
            for (;;) {
                KernelUseconds usec = 0;
                PosixThread::RelativeMicroseconds(GUEST_CLOCK_REALTIME, abstime, &usec);
                if (usec == 0) {
                    error = GUEST_ETIMEDOUT;
                    break;
                }
                if (semaphore->available.WaitUntil(lock, TimedWait::DeadlineNanos(usec))) break;
            }
        }
        --semaphore->waiters;
        if (error != 0) return Fail(error);
    }
    --semaphore->value;
    return 0;
}

}

void KsemDescriptorClosed(int descriptor) {
    std::shared_ptr<Ksem> closed;
    std::lock_guard lock(g_ksemMutex);
    const auto found = g_ksemDescriptors.find(descriptor);
    if (found == g_ksemDescriptors.end()) return;
    closed = std::move(found->second);
    g_ksemDescriptors.erase(found);
}

bool IsKsemDescriptor(int descriptor) {
    std::lock_guard lock(g_ksemMutex);
    return g_ksemDescriptors.contains(descriptor);
}

extern "C" {

int APS5_VABI ksem_init_nid_postfix(std::intptr_t* id, unsigned int value) {
    return Create(false, nullptr, id, GUEST_ANONYMOUS_MODE, value, 0);
}

int APS5_VABI ksem_open_nid_postfix(std::intptr_t* id, const char* name, int flags, std::uint16_t mode, unsigned int value) {
    if ((flags & ~(SCE_KERNEL_O_CREAT | SCE_KERNEL_O_EXCL)) != 0) return Fail(GUEST_EINVAL);
    return Create(true, name, id, mode, value, flags);
}

int APS5_VABI ksem_unlink_nid_postfix(const char* name) {
    if (const int error = NameError(name)) return Fail(error);
    std::shared_ptr<Ksem> removed;
    std::lock_guard lock(g_ksemMutex);
    const auto found = g_ksemNames.find(name);
    if (found == g_ksemNames.end()) return Fail(GUEST_ENOENT);
    RequireAccess(*found->second, "ksem_unlink");
    removed = std::move(found->second);
    g_ksemNames.erase(found);
    return 0;
}

int APS5_VABI ksem_close_nid_postfix(std::intptr_t id) {
    const auto semaphore = Find(id);
    if (!semaphore || semaphore->anonymous) return Fail(GUEST_EINVAL);
    return close_nid_postfix(static_cast<int>(id));
}

int APS5_VABI ksem_destroy_nid_postfix(std::intptr_t id) {
    const auto semaphore = Find(id);
    if (!semaphore || !semaphore->anonymous) return Fail(GUEST_EINVAL);
    {
        std::lock_guard lock(g_ksemMutex);
        if (semaphore->waiters != 0) return Fail(GUEST_EBUSY);
        semaphore->dead = true;
    }
    return close_nid_postfix(static_cast<int>(id));
}

int APS5_VABI ksem_post_nid_postfix(std::intptr_t id) {
    const auto semaphore = Find(id);
    if (!semaphore) return Fail(GUEST_EINVAL);
    std::lock_guard lock(g_ksemMutex);
    if (semaphore->value == GUEST_SEM_VALUE_MAX) return Fail(GUEST_EOVERFLOW);
    ++semaphore->value;
    if (semaphore->waiters > 0) semaphore->available.NotifyOne();
    return 0;
}

int APS5_VABI ksem_wait_nid_postfix(std::intptr_t id) {
    return Wait(id, false, nullptr);
}

int APS5_VABI ksem_timedwait_nid_postfix(std::intptr_t id, const KernelTimespec* abstime) {
    return Wait(id, false, abstime);
}

int APS5_VABI ksem_trywait_nid_postfix(std::intptr_t id) {
    return Wait(id, true, nullptr);
}

int APS5_VABI ksem_getvalue_nid_postfix(std::intptr_t id, int* value) {
    const auto semaphore = Find(id);
    if (!semaphore) return Fail(GUEST_EINVAL);
    int current = 0;
    {
        std::lock_guard lock(g_ksemMutex);
        current = static_cast<int>(semaphore->value);
    }
    if (!value) return Fail(GUEST_EFAULT);
    *value = current;
    return 0;
}

}
