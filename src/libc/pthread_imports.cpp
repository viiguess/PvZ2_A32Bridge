// pthread, semaphores and thread creation. Guest threads run on host threads, each with its own JIT.
#include "libc/pthread_imports.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "libc/forward.h"
#include "libc/guest_abi.h"
#include "libc/time_imports.h"

// ------------------------------------------------------------------ sync objects
static std::mutex g_sync_mu;
static std::unordered_map<u32, pthread_mutex_t*> g_mutex;
static std::unordered_map<u32, pthread_cond_t*> g_cond;
static std::unordered_map<u32, sem_t*> g_sem;

static pthread_mutex_t* mutex_get(u32 guest_addr, int type = -1) {
  std::lock_guard<std::mutex> lock(g_sync_mu);
  auto it = g_mutex.find(guest_addr);
  if (it != g_mutex.end() && type < 0)
    return it->second;
  if (type < 0) {
    u32 first_word = *(u32*)H(guest_addr);
    type = (first_word & 0xC000) == 0x4000 ? 1 : (first_word & 0xC000) == 0x8000 ? 2 : 0;
  }
  auto* mutex = new pthread_mutex_t;
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, type == 1   ? PTHREAD_MUTEX_RECURSIVE
                                   : type == 2 ? PTHREAD_MUTEX_ERRORCHECK
                                               : PTHREAD_MUTEX_NORMAL);
  pthread_mutex_init(mutex, &attr);
  g_mutex[guest_addr] = mutex;  // (a replaced mutex leaks; init on live mutex is a guest bug anyway)
  return mutex;
}

static pthread_cond_t* cond_get(u32 guest_addr, bool init = false) {
  std::lock_guard<std::mutex> lock(g_sync_mu);
  auto it = g_cond.find(guest_addr);
  if (it != g_cond.end() && !init)
    return it->second;
  auto* cond = new pthread_cond_t;
  pthread_cond_init(cond, nullptr);
  g_cond[guest_addr] = cond;
  return cond;
}

static sem_t* sem_get(u32 guest_addr) {
  std::lock_guard<std::mutex> lock(g_sync_mu);
  auto it = g_sem.find(guest_addr);
  return it == g_sem.end() ? nullptr : it->second;
}

// ------------------------------------------------------------------ threads
struct GThread {
  pthread_t h;
  u32 retval = 0;
  bool detached = false;
  bool done = false;
};

static std::mutex g_th_mu;
static std::map<u32, GThread*> g_threads;

struct StartInfo {
  u32 fn, arg, stack, id;
  GThread* gt;
};

static thread_local GThread* t_gt;

static void* thread_main(void* start_info) {
  StartInfo* start = (StartInfo*)start_info;
  thread_create_for_guest(start->stack, start->id);
  t_gt = start->gt;
  u32 result = guest_call(start->fn, {start->arg});
  start->gt->retval = result;
  delete start;
  return nullptr;
}

// pthread
static void import_pthread_mutex_init(Ctx& ctx, const Stub&) {
  int type = ctx.a(1) ? *(int*)ctx.p(1) : 0;
  mutex_get(ctx.a(0), type);
  ctx.ret(0);
}

static void import_pthread_mutex_destroy(Ctx& ctx, const Stub&) {
  std::lock_guard<std::mutex> lock(g_sync_mu);
  auto it = g_mutex.find(ctx.a(0));
  if (it != g_mutex.end()) {
    pthread_mutex_destroy(it->second);
    delete it->second;
    g_mutex.erase(it);
  }
  ctx.ret(0);
}

static void import_pthread_mutex_lock(Ctx& ctx, const Stub&) {
  ctx.ret(pthread_mutex_lock(mutex_get(ctx.a(0))));
}

static void import_pthread_mutex_unlock(Ctx& ctx, const Stub&) {
  ctx.ret(pthread_mutex_unlock(mutex_get(ctx.a(0))));
}

static void import_pthread_mutex_trylock(Ctx& ctx, const Stub&) {
  ctx.ret(pthread_mutex_trylock(mutex_get(ctx.a(0))));
}

static void import_pthread_mutexattr_init(Ctx& ctx, const Stub&) {
  *(u32*)ctx.p(0) = 0;
  ctx.ret(0);
}

static void import_pthread_mutexattr_settype(Ctx& ctx, const Stub&) {
  *(u32*)ctx.p(0) = ctx.a(1);
  ctx.ret(0);
}

static void import_pthread_cond_init(Ctx& ctx, const Stub&) {
  cond_get(ctx.a(0), true);
  ctx.ret(0);
}

static void import_pthread_cond_destroy(Ctx& ctx, const Stub&) {
  std::lock_guard<std::mutex> lock(g_sync_mu);
  auto it = g_cond.find(ctx.a(0));
  if (it != g_cond.end()) {
    pthread_cond_destroy(it->second);
    delete it->second;
    g_cond.erase(it);
  }
  ctx.ret(0);
}

static void import_pthread_cond_wait(Ctx& ctx, const Stub&) {
  ctx.ret(pthread_cond_wait(cond_get(ctx.a(0)), mutex_get(ctx.a(1))));
}

static void import_pthread_cond_timedwait(Ctx& ctx, const Stub&) {
  timespec timeout = timespec_from_guest(ctx.a(2));
  ctx.ret(pthread_cond_timedwait(cond_get(ctx.a(0)), mutex_get(ctx.a(1)), &timeout));
}

static void import_pthread_cond_signal(Ctx& ctx, const Stub&) {
  ctx.ret(pthread_cond_signal(cond_get(ctx.a(0))));
}

static void import_pthread_cond_broadcast(Ctx& ctx, const Stub&) {
  ctx.ret(pthread_cond_broadcast(cond_get(ctx.a(0))));
}

static void import_sem_init(Ctx& ctx, const Stub&) {
  auto* sem = new sem_t;
  sem_init(sem, 0, ctx.a(2));
  std::lock_guard<std::mutex> lock(g_sync_mu);
  g_sem[ctx.a(0)] = sem;
  ctx.ret(0);
}

static void import_sem_destroy(Ctx& ctx, const Stub&) {
  std::lock_guard<std::mutex> lock(g_sync_mu);
  auto it = g_sem.find(ctx.a(0));
  if (it != g_sem.end()) {
    sem_destroy(it->second);
    delete it->second;
    g_sem.erase(it);
  }
  ctx.ret(0);
}

static void import_sem_post(Ctx& ctx, const Stub&) {
  sem_t* sem = sem_get(ctx.a(0));
  ctx.ret(sem ? sem_post(sem) : -1);
}

static void import_sem_wait(Ctx& ctx, const Stub&) {
  sem_t* sem = sem_get(ctx.a(0));
  int result = -1;
  if (sem)
    while ((result = sem_wait(sem)) && errno == EINTR) {
    }
  ctx.ret(result);
}

static void import_sem_trywait(Ctx& ctx, const Stub&) {
  sem_t* sem = sem_get(ctx.a(0));
  ctx.ret(sem ? sem_trywait(sem) : -1);
}

static void import_sem_timedwait(Ctx& ctx, const Stub&) {
  sem_t* sem = sem_get(ctx.a(0));
  timespec timeout = timespec_from_guest(ctx.a(1));
  ctx.ret(sem ? sem_timedwait(sem, &timeout) : -1);
}

static void import_sem_getvalue(Ctx& ctx, const Stub&) {
  sem_t* sem = sem_get(ctx.a(0));
  int value = 0;
  if (sem)
    sem_getvalue(sem, &value);
  *(int*)ctx.p(1) = value;
  ctx.ret(0);
}

static void import_pthread_key_create(Ctx& ctx, const Stub&) {
  pthread_key_t key;
  int result = pthread_key_create(&key, nullptr);
  *(u32*)ctx.p(0) = key;
  ctx.ret(result);
}

static void import_pthread_key_delete(Ctx& ctx, const Stub&) {
  ctx.ret(pthread_key_delete(ctx.a(0)));
}

static void import_pthread_getspecific(Ctx& ctx, const Stub&) {
  ctx.ret((u32)(uintptr_t)pthread_getspecific(ctx.a(0)));
}

static void import_pthread_setspecific(Ctx& ctx, const Stub&) {
  ctx.ret(pthread_setspecific(ctx.a(0), (void*)(uintptr_t)ctx.a(1)));
}

static std::recursive_mutex g_once_mu;

static void import_pthread_once(Ctx& ctx, const Stub&) {
  std::lock_guard<std::recursive_mutex> lock(g_once_mu);
  u32* cond = (u32*)ctx.p(0);
  if (*cond == 2) {
    ctx.ret(0);
    return;
  }
  *cond = 1;
  guest_call(ctx.a(1), {});
  *cond = 2;
  ctx.ret(0);
}

static GuestPthreadAttr* attr_arg(Ctx& ctx, int index) {
  return (GuestPthreadAttr*)ctx.p(index);
}

static void import_pthread_attr_init(Ctx& ctx, const Stub&) {
  GuestPthreadAttr* attr = attr_arg(ctx, 0);
  memset(attr, 0, sizeof *attr);
  attr->stack_size = DEFAULT_GUEST_STACK;
  attr->guard_size = DEFAULT_GUARD_SIZE;
  ctx.ret(0);
}

static void import_pthread_attr_setstacksize(Ctx& ctx, const Stub&) {
  attr_arg(ctx, 0)->stack_size = ctx.a(1);
  ctx.ret(0);
}

static void import_pthread_attr_setdetachstate(Ctx& ctx, const Stub&) {
  GuestPthreadAttr* attr = attr_arg(ctx, 0);
  attr->flags = ctx.a(1) ? (attr->flags | ATTR_DETACHED) : (attr->flags & ~ATTR_DETACHED);
  ctx.ret(0);
}

static void import_pthread_attr_setstack(Ctx& ctx, const Stub&) {
  GuestPthreadAttr* attr = attr_arg(ctx, 0);
  attr->stack_base = ctx.a(1);
  attr->stack_size = ctx.a(2);
  ctx.ret(0);
}

static void import_pthread_attr_getstack(Ctx& ctx, const Stub&) {
  GuestPthreadAttr* attr = attr_arg(ctx, 0);
  *(u32*)ctx.p(1) = attr->stack_base;
  *(u32*)ctx.p(2) = attr->stack_size;
  ctx.ret(0);
}

static void import_pthread_attr_setschedpolicy(Ctx& ctx, const Stub&) {
  attr_arg(ctx, 0)->sched_policy = ctx.a(1);
  ctx.ret(0);
}

static void import_pthread_attr_setschedparam(Ctx& ctx, const Stub&) {
  attr_arg(ctx, 0)->sched_priority = *(u32*)ctx.p(1);  // sched_param.sched_priority
  ctx.ret(0);
}

static void import_pthread_attr_getschedparam(Ctx& ctx, const Stub&) {
  *(u32*)ctx.p(1) = attr_arg(ctx, 0)->sched_priority;
  ctx.ret(0);
}

// pthread_getattr_np(thread, attr): reports the calling thread's guest stack.
static void import_pthread_getattr_np(Ctx& ctx, const Stub&) {
  GuestPthreadAttr* attr = attr_arg(ctx, 1);
  memset(attr, 0, sizeof *attr);
  attr->stack_base = thread_stack_lo();
  attr->stack_size = thread_stack_size();
  ctx.ret(0);
}

static void import_pthread_setschedparam(Ctx& ctx, const Stub&) {
  ctx.ret(0);
}

static void import_pthread_getschedparam(Ctx& ctx, const Stub&) {
  *(u32*)ctx.p(1) = 0;
  *(u32*)ctx.p(2) = 0;
  ctx.ret(0);
}

static void import_pthread_self(Ctx& ctx, const Stub&) {
  ctx.ret(thread_self_id());
}

static void import_pthread_equal(Ctx& ctx, const Stub&) {
  ctx.ret(ctx.a(0) == ctx.a(1));
}

static void import_pthread_create(Ctx& ctx, const Stub&) {
  u32 stack_size = DEFAULT_GUEST_STACK;
  bool detached = false;
  if (ctx.a(1)) {
    GuestPthreadAttr* attr = attr_arg(ctx, 1);
    if (attr->stack_size >= MIN_GUEST_STACK)
      stack_size = attr->stack_size;
    detached = attr->flags & ATTR_DETACHED;
  }
  auto* thread = new GThread;
  thread->detached = detached;
  u32 id = thread_alloc_id();
  auto* start = new StartInfo{ctx.a(2), ctx.a(3), stack_size, id, thread};
  pthread_attr_t host_attr;
  pthread_attr_init(&host_attr);
  pthread_attr_setstacksize(&host_attr, 512 * 1024);  // host stack only runs the emulator
  {
    std::lock_guard<std::mutex> lock(g_th_mu);
    int result = pthread_create(&thread->h, &host_attr, thread_main, start);
    if (result) {
      delete start;
      delete thread;
      ctx.ret(result);
      return;
    }
    if (detached)
      pthread_detach(thread->h);
    g_threads[id] = thread;
  }
  *(u32*)ctx.p(0) = id;
  ctx.ret(0);
}

static void import_pthread_join(Ctx& ctx, const Stub&) {
  GThread* thread;
  {
    std::lock_guard<std::mutex> lock(g_th_mu);
    auto it = g_threads.find(ctx.a(0));
    if (it == g_threads.end()) {
      ctx.ret(3);
      return;
    }
    thread = it->second;
    g_threads.erase(it);
  }
  pthread_join(thread->h, nullptr);
  if (ctx.a(1))
    *(u32*)ctx.p(1) = thread->retval;
  delete thread;
  ctx.ret(0);
}

static void import_pthread_detach(Ctx& ctx, const Stub&) {
  std::lock_guard<std::mutex> lock(g_th_mu);
  auto it = g_threads.find(ctx.a(0));
  if (it != g_threads.end()) {
    pthread_detach(it->second->h);
    it->second->detached = true;
  }
  ctx.ret(0);
}

static void import_pthread_exit(Ctx& ctx, const Stub&) {
  if (t_gt)
    t_gt->retval = ctx.a(0);
  thread_request_exit();
}

void register_pthread_imports() {
  hle("pthread_mutex_init", import_pthread_mutex_init);
  hle("pthread_mutex_destroy", import_pthread_mutex_destroy);
  hle("pthread_mutex_lock", import_pthread_mutex_lock);
  hle("pthread_mutex_unlock", import_pthread_mutex_unlock);
  hle("pthread_mutex_trylock", import_pthread_mutex_trylock);
  hle("pthread_mutexattr_init", import_pthread_mutexattr_init);
  hle("pthread_mutexattr_settype", import_pthread_mutexattr_settype);
  hle("pthread_cond_init", import_pthread_cond_init);
  hle("pthread_cond_destroy", import_pthread_cond_destroy);
  hle("pthread_cond_wait", import_pthread_cond_wait);
  hle("pthread_cond_timedwait", import_pthread_cond_timedwait);
  hle("pthread_cond_signal", import_pthread_cond_signal);
  hle("pthread_cond_broadcast", import_pthread_cond_broadcast);
  hle("sem_init", import_sem_init);
  hle("sem_destroy", import_sem_destroy);
  hle("sem_post", import_sem_post);
  hle("sem_wait", import_sem_wait);
  hle("sem_trywait", import_sem_trywait);
  hle("sem_timedwait", import_sem_timedwait);
  hle("sem_getvalue", import_sem_getvalue);
  hle("pthread_key_create", import_pthread_key_create);
  hle("pthread_key_delete", import_pthread_key_delete);
  hle("pthread_getspecific", import_pthread_getspecific);
  hle("pthread_setspecific", import_pthread_setspecific);
  hle("pthread_once", import_pthread_once);
  hle("pthread_attr_init", import_pthread_attr_init);
  hle("pthread_attr_setstacksize", import_pthread_attr_setstacksize);
  hle("pthread_attr_setdetachstate", import_pthread_attr_setdetachstate);
  hle("pthread_attr_setstack", import_pthread_attr_setstack);
  hle("pthread_attr_getstack", import_pthread_attr_getstack);
  hle("pthread_attr_setschedparam", import_pthread_attr_setschedparam);
  hle("pthread_attr_getschedparam", import_pthread_attr_getschedparam);
  hle("pthread_attr_setschedpolicy", import_pthread_attr_setschedpolicy);
  hle("pthread_getattr_np", import_pthread_getattr_np);
  hle("pthread_setschedparam", import_pthread_setschedparam);
  hle("pthread_getschedparam", import_pthread_getschedparam);
  hle("pthread_self", import_pthread_self);
  hle("pthread_equal", import_pthread_equal);
  hle("pthread_create", import_pthread_create);
  hle("pthread_join", import_pthread_join);
  hle("pthread_detach", import_pthread_detach);
  hle("pthread_exit", import_pthread_exit);
}
