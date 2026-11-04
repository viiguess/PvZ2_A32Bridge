// Thread bookkeeping: every host thread that touches the guest lazily gets a Thread with its own stack and TLS.
#include "runtime/thread.h"

#include <pthread.h>

#include <atomic>
#include <mutex>
#include <vector>

#include "runtime/jit.h"
#include "runtime/runtime.h"

static thread_local Thread* t_thread;
static pthread_key_t g_key;
static std::once_flag g_key_once;
static u32 g_next_id = 1;

static std::mutex g_threads_mu;
static std::vector<Thread*> g_all_threads;

static void thread_dtor(void* raw) {
  Thread* thr = (Thread*)raw;
  {
    std::lock_guard<std::mutex> lock(g_threads_mu);
    for (size_t i = 0; i < g_all_threads.size(); i++)
      if (g_all_threads[i] == thr) {
        g_all_threads.erase(g_all_threads.begin() + i);
        break;
      }
  }
  for (Cpu* cpu : thr->cpus)
    delete cpu;
  gfree(thr->stack_lo);
  gfree(thr->tls);
  delete thr;
}

static Thread* thread_new(u32 stack_size, u32 id = 0) {
  std::call_once(g_key_once, [] { pthread_key_create(&g_key, thread_dtor); });
  Thread* thr = new Thread;
  thr->stack_lo = gmalloc(stack_size);
  thr->stack_top = (thr->stack_lo + stack_size) & ~7u;
  thr->sp_hint = thr->stack_top;
  thr->tls = gmalloc(256);
  memset(G + thr->tls, 0, 256);
  thr->id = id ? id : __atomic_fetch_add(&g_next_id, 1, __ATOMIC_RELAXED);
  pthread_setspecific(g_key, thr);
  t_thread = thr;
  {
    std::lock_guard<std::mutex> lock(g_threads_mu);
    g_all_threads.push_back(thr);
  }
  return thr;
}

Thread* thread() {
  return t_thread ? t_thread : thread_new(4u << 20);
}

Thread* thread_create_for_guest(u32 stack_size, u32 id) {
  return thread_new(stack_size, id);
}

u32 thread_alloc_id() {
  return __atomic_fetch_add(&g_next_id, 1, __ATOMIC_RELAXED);
}

u32 thread_errno_addr() {
  return thread()->tls;
}

u32 thread_tls_addr() {
  return thread()->tls;
}

u32 thread_self_id() {
  return thread()->id;
}

u32 thread_stack_lo() {
  return thread()->stack_lo;
}

u32 thread_stack_size() {
  Thread* thr = thread();
  return thr->stack_top - thr->stack_lo;
}

void thread_set_sp_hint(u32 sp) {
  t_thread->sp_hint = sp;
}

void thread_request_exit() {
  Thread* thr = thread();
  thr->exit_req = true;
  if (thr->running)
    thr->running->jit->HaltExecution();
}
