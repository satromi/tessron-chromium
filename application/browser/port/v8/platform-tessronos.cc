/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

// platform-tessronos.cc
// V8's operating system layer for a process on TessronOS (design 17.19).
//
// It stands where platform-posix.cc, platform-linux.cc, platform-posix-
// time.cc and debug/stack_trace_posix.cc stand on Linux.
//
// Memory is the process's own (include/ts/umem.h): a reservation holds
// addresses without pages, pages are made in it with the protection asked
// for and taken away again. A page is never executable -- a process can
// neither write code nor make written memory runnable -- so V8 runs
// --jitless and every request for an executable mapping fails.
//
// Threads, their local storage and the stack bounds come from
// lib/libpthread; time from lib/libcxxrt and the system's time zone.

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <optional>
#include <ostream>
#include <vector>

#include "src/base/abort-mode.h"
#include "src/base/debug/stack_trace.h"
#include "src/base/macros.h"
#include "src/base/platform/platform.h"
#include "src/base/platform/time.h"
#include "src/base/timezone-cache.h"

extern "C" {
typedef int ER_;
ER_ ts_map_mem(void** p_adr, long size, long align, unsigned int prot);
ER_ ts_unm_mem(void* adr, long size);
ER_ ts_ctl_mem(void* adr, long size, unsigned int prot);
ER_ dt_getsystz(int* p_minutes);
int ts_get_pid(void);
int tk_get_tid(void);
void ts_ext_prc(int exitcd) __attribute__((noreturn));
}

namespace {

// include/ts/umem.h
constexpr unsigned kMemNone = 0x00;
constexpr unsigned kMemRead = 0x01;
constexpr unsigned kMemRW = 0x03;

constexpr size_t kPageSize = 4096;

// The protection of a V8 permission, or false when it cannot be given.
bool ToProt(v8::base::OS::MemoryPermission access, unsigned* prot) {
  using P = v8::base::OS::MemoryPermission;
  switch (access) {
    case P::kNoAccess:
    case P::kNoAccessWillJitLater:
      *prot = kMemNone;
      return true;
    case P::kRead:
      *prot = kMemRead;
      return true;
    case P::kReadWrite:
      *prot = kMemRW;
      return true;
    case P::kReadWriteExecute:
    case P::kReadExecute:
      return false;
  }
  return false;
}

size_t RoundUp(size_t v) { return (v + kPageSize - 1) & ~(kPageSize - 1); }

}  // namespace

namespace v8 {
namespace base {

const char* g_gc_fake_mmap = nullptr;

// ---------------------------------------------------------------------------
// Start-up and the machine

void OS::Initialize(const char* const gc_fake_mmap) {
  g_gc_fake_mmap = gc_fake_mmap;
}

bool OS::IsHardwareEnforcedShadowStacksEnabled() { return false; }

void OS::EnsureAlternativeSignalStackIsAvailableForCurrentThread() {}

bool OS::ArmUsingHardFloat() { return true; }

int OS::ActivationFrameAlignment() { return 16; }

size_t OS::AllocatePageSize() { return kPageSize; }

size_t OS::CommitPageSize() { return kPageSize; }

void OS::SetRandomMmapSeed(int64_t seed) {}

// The kernel places a reservation where it is free; a hint of none is best.
void* OS::GetRandomMmapAddr() { return nullptr; }

// ---------------------------------------------------------------------------
// Memory

void* OS::Allocate(void* hint, size_t size, size_t alignment,
                   MemoryPermission access,
                   std::optional<SharedMemoryHandle> handle) {
  unsigned prot;
  if (handle.has_value() || !ToProt(access, &prot)) return nullptr;
  if (alignment < kPageSize) alignment = kPageSize;
  void* p = hint;
  if (ts_map_mem(&p, static_cast<long>(RoundUp(size)),
                 static_cast<long>(alignment), prot) < 0) {
    return nullptr;
  }
  return p;
}

void OS::Free(void* address, size_t size) {
  CHECK_EQ(0, ts_unm_mem(address, static_cast<long>(RoundUp(size))));
}

void OS::Release(void* address, size_t size) {
  CHECK_EQ(0, ts_unm_mem(address, static_cast<long>(RoundUp(size))));
}

bool OS::SetPermissions(void* address, size_t size, MemoryPermission access) {
  unsigned prot;
  if (!ToProt(access, &prot)) return false;
  return ts_ctl_mem(address, static_cast<long>(RoundUp(size)), prot) == 0;
}

// The pages of the program's own data are not the process's to protect
// (only what it asked for with ts_map_mem is); they stay writable.
void OS::SetDataReadOnly(void* address, size_t size) {
  (void)ts_ctl_mem(address, static_cast<long>(RoundUp(size)), kMemRead);
}

bool OS::RecommitPages(void* address, size_t size, MemoryPermission access) {
  return SetPermissions(address, size, access);
}

// The pages may be dropped; keeping them is allowed and keeps their access.
bool OS::DiscardSystemPages(void* address, size_t size) { return true; }

bool OS::DecommitPages(void* address, size_t size) {
  return ts_ctl_mem(address, static_cast<long>(RoundUp(size)), kMemNone) == 0;
}

bool OS::SealPages(void* address, size_t size) { return false; }

bool OS::SetMemoryRegionName(const void* address, size_t size,
                             const char* name) {
  return true;
}

bool OS::HasLazyCommits() { return false; }

bool OS::CanReserveAddressSpace() { return true; }

std::optional<AddressSpaceReservation> OS::CreateAddressSpaceReservation(
    void* hint, size_t size, size_t alignment, MemoryPermission max_permission,
    std::optional<SharedMemoryHandle> handle) {
  if (handle.has_value()) return {};
  void* p = Allocate(hint, size, alignment, MemoryPermission::kNoAccess);
  if (p == nullptr) return {};
  return AddressSpaceReservation(p, RoundUp(size));
}

void OS::FreeAddressSpaceReservation(AddressSpaceReservation reservation) {
  Free(reservation.base(), reservation.size());
}

// No memory is shared between two mappings in a process here.
void* OS::AllocateShared(size_t size, MemoryPermission access) {
  return nullptr;
}

void* OS::AllocateShared(void* address, size_t size, MemoryPermission access,
                         SharedMemoryHandle handle, uint64_t offset) {
  return nullptr;
}

void OS::FreeShared(void* address, size_t size) { Free(address, size); }

void* OS::RemapShared(void* old_address, void* new_address, size_t size) {
  return nullptr;
}

bool OS::RemapPages(const void* address, size_t size, void* new_address,
                    MemoryPermission access) {
  return false;
}

std::optional<SharedMemoryHandle> OS::CreateSharedMemoryHandleForTesting(
    size_t size) {
  return {};
}

void OS::DestroySharedMemoryHandle(SharedMemoryHandle handle) {}

std::optional<OS::MemoryRange> OS::GetFirstFreeMemoryRangeWithin(
    OS::Address boundary_start, OS::Address boundary_end, size_t minimum_size,
    size_t alignment) {
  return {};
}

bool AddressSpaceReservation::Allocate(void* address, size_t size,
                                       OS::MemoryPermission access) {
  DCHECK(Contains(address, size));
  if (access == OS::MemoryPermission::kNoAccess) return true;
  return OS::SetPermissions(address, size, access);
}

bool AddressSpaceReservation::Free(void* address, size_t size) {
  DCHECK(Contains(address, size));
  return OS::DecommitPages(address, size);
}

bool AddressSpaceReservation::AllocateShared(void* address, size_t size,
                                             OS::MemoryPermission access,
                                             SharedMemoryHandle handle,
                                             uint64_t offset) {
  return false;
}

bool AddressSpaceReservation::FreeShared(void* address, size_t size) {
  return false;
}

bool AddressSpaceReservation::SetPermissions(void* address, size_t size,
                                             OS::MemoryPermission access) {
  DCHECK(Contains(address, size));
  return OS::SetPermissions(address, size, access);
}

bool AddressSpaceReservation::RecommitPages(void* address, size_t size,
                                            OS::MemoryPermission access) {
  DCHECK(Contains(address, size));
  return OS::RecommitPages(address, size, access);
}

bool AddressSpaceReservation::DiscardSystemPages(void* address, size_t size) {
  DCHECK(Contains(address, size));
  return OS::DiscardSystemPages(address, size);
}

bool AddressSpaceReservation::DecommitPages(void* address, size_t size) {
  DCHECK(Contains(address, size));
  return OS::DecommitPages(address, size);
}

// A part of a reservation is used as a reservation of its own.
std::optional<AddressSpaceReservation>
AddressSpaceReservation::CreateSubReservation(
    void* address, size_t size, OS::MemoryPermission max_permission) {
  DCHECK(Contains(address, size));
  return AddressSpaceReservation(address, size);
}

bool AddressSpaceReservation::FreeSubReservation(
    AddressSpaceReservation reservation) {
  return OS::DecommitPages(reservation.base(), reservation.size());
}

// ---------------------------------------------------------------------------
// The process

const char* OS::GetGCFakeMMapFile() { return g_gc_fake_mmap; }

void OS::Sleep(TimeDelta interval) {
  usleep(static_cast<useconds_t>(interval.InMicroseconds()));
}

void OS::Abort() {
  fflush(stdout);
  fflush(stderr);
  fputs("v8: abort\n", stderr);
  // The callers, by the frame chain (the sources keep frame pointers), for
  // looking up in the program's symbols
  void** fp = reinterpret_cast<void**>(__builtin_frame_address(0));
  for (int i = 0; i < 24 && fp != nullptr; i++) {
    fprintf(stderr, "v8:   from %p\n", fp[1]);
    void** next = reinterpret_cast<void**>(fp[0]);
    if (next <= fp) break;
    fp = next;
  }
  fflush(stderr);
  if (g_abort_mode == AbortMode::kImmediateCrash) {
    __builtin_trap();
  }
  ts_ext_prc(134);
}

void OS::DebugBreak() { __builtin_trap(); }

void OS::ExitProcess(int exit_code) {
  fflush(stdout);
  fflush(stderr);
  ts_ext_prc(exit_code);
}

int OS::GetCurrentProcessId() { return ts_get_pid(); }

int OS::GetCurrentThreadIdInternal() { return tk_get_tid(); }

int OS::GetLastError() { return errno; }

// No time is counted per task: the time since start stands for it.
int OS::GetUserTime(uint32_t* secs, uint32_t* usecs) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  *secs = static_cast<uint32_t>(t.tv_sec);
  *usecs = static_cast<uint32_t>(t.tv_nsec / 1000);
  return 0;
}

int OS::GetPeakMemoryUsageKb() { return -1; }

double OS::TimeCurrentMillis() {
  struct timespec t;
  clock_gettime(CLOCK_REALTIME, &t);
  return static_cast<double>(t.tv_sec) * 1000.0 +
         static_cast<double>(t.tv_nsec) / 1000000.0;
}

void OS::SignalCodeMovingGC() {}

void OS::AdjustSchedulingParams() {}

std::vector<OS::SharedLibraryAddress> OS::GetSharedLibraryAddresses() {
  return {};
}

// ---------------------------------------------------------------------------
// Time zone: the system's offset (dt_getsystz); summer time is not kept

namespace {

class TessronOSTimezoneCache : public TimezoneCache {
 public:
  const char* LocalTimezone(double time_ms) override {
    int m = Minutes();
    if (m == 540) return "JST";
    if (m == 0) return "UTC";
    snprintf(name_, sizeof(name_), "UTC%c%02d:%02d", m < 0 ? '-' : '+',
             (m < 0 ? -m : m) / 60, (m < 0 ? -m : m) % 60);
    return name_;
  }
  double DaylightSavingsOffset(double time_ms) override { return 0.0; }
  double LocalTimeOffset(double time_ms, bool is_utc) override {
    return static_cast<double>(Minutes()) * 60000.0;
  }
  void Clear(TimeZoneDetection) override {}
  ~TessronOSTimezoneCache() override = default;

 private:
  static int Minutes() {
    int m = 0;
    if (dt_getsystz(&m) < 0) m = 0;
    return m;
  }
  char name_[16];
};

}  // namespace

TimezoneCache* OS::CreateTimezoneCache() { return new TessronOSTimezoneCache(); }

// ---------------------------------------------------------------------------
// Files and output

FILE* OS::FOpen(const char* path, const char* mode) {
  return fopen(path, mode);
}

bool OS::Remove(const char* path) { return remove(path) == 0; }

char OS::DirectorySeparator() { return '/'; }

bool OS::isDirectorySeparator(const char ch) {
  return ch == DirectorySeparator();
}

FILE* OS::OpenTemporaryFile() { return nullptr; }

const char* const OS::LogFileOpenMode = "w+";

OS::MemoryMappedFile* OS::MemoryMappedFile::open(const char* name,
                                                 FileMode mode) {
  return nullptr;
}

OS::MemoryMappedFile* OS::MemoryMappedFile::create(const char* name,
                                                   size_t size,
                                                   void* initial) {
  return nullptr;
}

void OS::Print(const char* format, ...) {
  va_list args;
  va_start(args, format);
  VPrint(format, args);
  va_end(args);
}

void OS::VPrint(const char* format, va_list args) {
  vprintf(format, args);
  fflush(stdout);
}

void OS::FPrint(FILE* out, const char* format, ...) {
  va_list args;
  va_start(args, format);
  VFPrint(out, format, args);
  va_end(args);
}

void OS::VFPrint(FILE* out, const char* format, va_list args) {
  vfprintf(out, format, args);
  fflush(out);
}

void OS::PrintError(const char* format, ...) {
  va_list args;
  va_start(args, format);
  VPrintError(format, args);
  va_end(args);
}

void OS::VPrintError(const char* format, va_list args) {
  vfprintf(stderr, format, args);
  fflush(stderr);
}

int OS::SNPrintF(char* str, int length, const char* format, ...) {
  va_list args;
  va_start(args, format);
  int result = VSNPrintF(str, length, format, args);
  va_end(args);
  return result;
}

int OS::VSNPrintF(char* str, int length, const char* format, va_list args) {
  int n = vsnprintf(str, length, format, args);
  if (n < 0 || n >= length) {
    if (length > 0) str[length - 1] = '\0';
    return -1;
  }
  return n;
}

void OS::StrNCpy(char* dest, int length, const char* src, size_t n) {
  strncpy(dest, src, n);
}

// ---------------------------------------------------------------------------
// Threads (lib/libpthread)

class Thread::PlatformData {
 public:
  PlatformData() : thread_(0) {}
  pthread_t thread_;
};

Thread::Thread(const Options& options)
    : data_(new PlatformData),
      stack_size_(options.stack_size()),
      priority_(options.priority()),
      start_semaphore_(nullptr) {
  const int kMinStack = 1024 * 1024;
  if (stack_size_ < kMinStack) stack_size_ = kMinStack;
  set_name(options.name());
}

Thread::~Thread() { delete data_; }

static void* ThreadEntry(void* arg) {
  Thread* thread = reinterpret_cast<Thread*>(arg);
  pthread_setname_np(pthread_self(), thread->name());
  thread->NotifyStartedAndDispatch();
  return nullptr;
}

void Thread::set_name(const char* name) {
  strncpy(name_, name, sizeof(name_) - 1);
  name_[sizeof(name_) - 1] = '\0';
}

bool Thread::Start() {
  pthread_attr_t attr;
  if (pthread_attr_init(&attr) != 0) return false;
  pthread_attr_setstacksize(&attr, static_cast<size_t>(stack_size_));
  int result = pthread_create(&data_->thread_, &attr, ThreadEntry, this);
  pthread_attr_destroy(&attr);
  return result == 0;
}

void Thread::Join() { pthread_join(data_->thread_, nullptr); }

Thread::LocalStorageKey Thread::CreateThreadLocalKey() {
  pthread_key_t key;
  int result = pthread_key_create(&key, nullptr);
  CHECK_EQ(0, result);
  return static_cast<LocalStorageKey>(key);
}

void Thread::DeleteThreadLocalKey(LocalStorageKey key) {
  pthread_key_delete(static_cast<pthread_key_t>(key));
}

void* Thread::GetThreadLocal(LocalStorageKey key) {
  return pthread_getspecific(static_cast<pthread_key_t>(key));
}

void Thread::SetThreadLocal(LocalStorageKey key, void* value) {
  pthread_setspecific(static_cast<pthread_key_t>(key), value);
}

// ---------------------------------------------------------------------------
// The stack of the calling thread: the one lib/libpthread made for it

Stack::StackSlot Stack::ObtainCurrentThreadStackStart() {
  void* lo = nullptr;
  void* hi = nullptr;
  pthread_ts_stack(&lo, &hi);
  return hi;
}

Stack::StackSlot Stack::ObtainCurrentThreadStackReservedLimit() {
  void* lo = nullptr;
  void* hi = nullptr;
  pthread_ts_stack(&lo, &hi);
  return lo;
}

void Stack::SetCurrentThreadStackBounds(uintptr_t, uintptr_t) {}

Stack::StackSlot Stack::GetCurrentStackPosition() {
  return __builtin_frame_address(0);
}

// ---------------------------------------------------------------------------
// Stack traces: nothing names the frames of a program at run time here

namespace debug {

bool EnableInProcessStackDumping() { return false; }

void DisableSignalStackDump() {}

StackTrace::StackTrace() : count_(0) {}

void StackTrace::Print() const {
  OS::Print("(no stack trace on this platform)\n");
}

void StackTrace::OutputToStream(std::ostream* os) const {
  (*os) << "(no stack trace on this platform)\n";
}

}  // namespace debug

}  // namespace base
}  // namespace v8
