#include "uevloop/utils/linked-list.h"
#include "uevloop/utils/module.h"
#include <bits/time.h>
#include "uevloop/system/containers/application.h"
#include "uevloop/system/containers/system-pools.h"
#include "uevloop/system/containers/system-queues.h"
#include "uevloop/system/event-loop.h"
#include "uevloop/system/scheduler.h"
#include "uevloop/utils/circular-queue.h"
#include "uevloop/utils/closure.h"
#include "uevloop/utils/object-pool.h"
#include <bits/types/timer_t.h>
#include <err.h>
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#define ULOG_ENABLED
#include "ulog.h"
#define _XOPEN_SOURCE /* See feature_test_macros(7) */
#include <time.h>
void *my_alloc(size_t size);
void my_dealloc(void *ptr);
#define MCO_ALLOC(size) my_alloc(size)
#define MCO_DEALLOC(ptr, size) my_dealloc(ptr)
#define MINICORO_IMPL
#include "minicoro.h"
char timestamp[256];
char* get_timestamp();
int allocs = 0;

typedef struct _coro_stack {
  // should match mco_desc.storage_size
  // which is determined on runtime (depending on arch, mechanism?)
    char str[58656];
} coro_stack_t;


// The log2 of our pool size.
#define CORO_STACK_POOL_SIZE_LOG2N   (5)
UEL_DECLARE_OBJPOOL_BUFFERS(coro_stack_t, CORO_STACK_POOL_SIZE_LOG2N, coro_stack_pool);
uel_objpool_t coro_stack_pool;
// my_pool now is a pool with 32 (2**5) obj_t

void my_dealloc(void *ptr){
  allocs--;
  ULOG_WARNING("De-allocating pointer %p, allocs: %d\n", ptr, allocs);
  uel_objpool_release(&coro_stack_pool, ptr);
}
void *my_alloc(size_t size){
  allocs++;
  void *ptr = uel_objpool_acquire(&coro_stack_pool);
  ULOG_WARNING("Allocated pointer %p, size: %ld, allocs: %d\n", ptr, size, allocs);

  return ptr;
}


const char *mco_state_str[] = {[MCO_DEAD] = "MCO_DEAD",
                               [MCO_NORMAL] = "MCO_NORMAL",
                               [MCO_RUNNING] = "MCO_RUNNING",
                               [MCO_SUSPENDED] = "MCO_SUSPENDED"};

FILE* fp_log_file = NULL;
void my_file_logger(ulog_level_t severity, char *msg) {
  if(fp_log_file == NULL) {
    fprintf(stderr,"log file must be opened before logging to it!\n");
    exit(1);
  }

  fseek(fp_log_file,0, SEEK_END);
  fprintf(fp_log_file, "%s [%s]: %s\n", get_timestamp(), ulog_level_name(severity), msg);
  fflush(fp_log_file);
}
void my_console_logger(ulog_level_t severity, char *msg) {
  printf("%s %s [%s]: %s\n", ulog_level_color(severity), get_timestamp(), ulog_level_name(severity), msg);
}

bool keep_running = true;
static volatile uint32_t counter = 0;
uel_event_t *timer_handle;
uel_application_t eyra_app;

typedef void (*coroutine_func)(mco_coro *);

// Closures can be reused, no need to recreate it all the time
uel_closure_t easync_resume_coro_clo;
uel_closure_t easync_timeout_coro_clo;
uel_closure_t raise_signal_clo;

#define SIGNAL_QUEUE_BUFFER_SIZE_LOG2N (4)
uel_cqueue_t easync_signal_closure_queue[32];
void *_easync_signal_closure_buffer[32 * (1 << SIGNAL_QUEUE_BUFFER_SIZE_LOG2N)];






bool easync_spawn_coroutine(coroutine_func fp, void *user_data);
// Tick the timer every ms
void easync_timer_isr() { uel_app_update_timer(&eyra_app, ++counter); }

static void *print_value(void *context, void *parameters) {
  uintptr_t v = (uintptr_t)parameters;
  int enqueued = uel_sysqueues_count_enqueued_events(&eyra_app.queues);
  int scheduled = uel_sysqueues_count_scheduled_events(&eyra_app.queues);
  ULOG_INFO("Closure scheduled: %ld queued: %d scheduled: %d", v, enqueued, scheduled);

  return NULL;
}
static void *delay_print_value(void *context, void *parameters) {
  uintptr_t v = (uintptr_t)parameters;
  int enqueued = uel_sysqueues_count_enqueued_events(&eyra_app.queues);
  int scheduled = uel_sysqueues_count_scheduled_events(&eyra_app.queues);
  ULOG_INFO("delayed: %ld queued: %d scheduled: %d", v, enqueued, scheduled);
  return NULL;
}

static void *abort_loop(void *context, void *parameters) {
  uintptr_t v = (uintptr_t)parameters;
  keep_running = false;
  return NULL;
}

// 16 entries circular queue

/**
 * Schedule a uel closure that calls to mco_resume(task) in the future.
 * Then yield, returning to the closure that woke us up
 * Basically our version of "await Task.Delay()"
 */
void easync_task_delay(mco_coro *task, uel_application_t *app, int delay_ms) {
  uel_app_run_later(app, delay_ms, easync_resume_coro_clo, task);
  mco_yield(task);
}
enum EasyncSignal { SIGNAL_OK = 100, SIGNAL_TIMEOUT = 101, SIGNAL_CANCELLED = 102, SIGNAL_ABORT };
/**
 * Schedule a uel closure that calls to mco_resume(task) when a signal is received.
 * Then yield, returning to the closure that woke us up
 * Basically our version of "await signal.WaitAsync()" or something like that
 */
enum EasyncSignal easync_task_await_signal(mco_coro *task, uel_application_t *app, int signal, int timeoutMS) {
  assert(signal >= 0 && signal < 32);

  /* log_cqueue(&easync_signal_closure_queue[signal], "before push"); */
  if (!uel_cqueue_push(&easync_signal_closure_queue[signal], task)) {
    ULOG_ERROR("Failed to enqueue task %p resume for signal %d", task, signal);
    return SIGNAL_ABORT;
  }
  /* log_cqueue(&easync_signal_closure_queue[signal], "after push"); */
  ULOG_DEBUG("coroutines waiting in easync_signal_closure_queue[%d] = %ld", signal,
             uel_cqueue_count(&easync_signal_closure_queue[signal]));
  uel_event_t *timer_event = uel_app_run_later(&eyra_app, timeoutMS, easync_timeout_coro_clo, task);
  mco_yield(task);

  // Check if we got any notification (cancelled, timeout)
  uint8_t res = 0;
  mco_result r = mco_pop(task, &res, 1);
  if (MCO_NOT_ENOUGH_SPACE == r) {
    ULOG_DEBUG("easync_task_await_signal -> nothing to pop, no timeout/cancellation");
    uel_event_timer_cancel(timer_event);
    return SIGNAL_OK;
  } else if (MCO_SUCCESS != r) {
    ULOG_DEBUG("easync_task_await_signal -> pop failed! error %d", r);
    uel_event_timer_cancel(timer_event);
  }

  int count = 0;
  switch (res) {
  case SIGNAL_OK:
    ULOG_DEBUG("easync_task_await_signal -> notification OK");
    uel_event_timer_cancel(timer_event);
    break;
  case SIGNAL_TIMEOUT:
    ULOG_DEBUG("easync_task_await_signal -> notification TIMEOUT");
    // Iterate through the queue, removing any references to task
    count = uel_cqueue_count(&easync_signal_closure_queue[signal]);
    for (int i = 0; i < count; i++) {
      void *e = uel_cqueue_pop(&easync_signal_closure_queue[signal]);
      if (e != task) {
        uel_cqueue_push(&easync_signal_closure_queue[signal], e);
      }
    }
    return SIGNAL_TIMEOUT;
    break;
  case SIGNAL_CANCELLED:
    ULOG_DEBUG("easync_task_await_signal -> notification CANCELLED");
    count = uel_cqueue_count(&easync_signal_closure_queue[signal]);
    for (int i = 0; i < count; i++) {
      void *e = uel_cqueue_pop(&easync_signal_closure_queue[signal]);
      if (e != task) {
        uel_cqueue_push(&easync_signal_closure_queue[signal], e);
      }
    }
    uel_event_timer_cancel(timer_event);
    return SIGNAL_CANCELLED;
    break;
  default:
    ULOG_DEBUG("easync_task_await_signal -> notification UNKNOWN(%d)", res);
    break;
  }
  return SIGNAL_OK;
}

// MCO coroutine that can resume/yield/die
// Yields into a wakeup from signal USR1
void async_print_and_wait_signal(mco_coro *task) {
  int resumes = 0;
  int rc = 0;
  ULOG_INFO("print and wait signal started, resumes: %d. Waiting for signal USR2 (%d)\r", resumes, SIGUSR2);
  rc = easync_task_await_signal(task, &eyra_app, SIGUSR2, 5000);
  if (rc == SIGNAL_TIMEOUT) {
    ULOG_ERROR("async_print_and_wait_signal -> timeout in waiting for signal, aborting!");
    return;
  }
  if (rc == SIGNAL_CANCELLED) {
    ULOG_ERROR("async_print_and_wait_signal -> task cancelled, returning!");
    return;
  }
  while (true) {
    resumes++;
    ULOG_INFO("coroutine resumed, resumes: %d. Waiting for signal USR2 (%d)\r", resumes, SIGUSR2);
    rc = easync_task_await_signal(task, &eyra_app, SIGUSR2, 5000);
    if (rc == SIGNAL_TIMEOUT) {
      ULOG_ERROR("async_print_and_wait_signal -> timeout in waiting for signal, aborting!");
      return;
    }
    if (rc == SIGNAL_CANCELLED) {
      ULOG_ERROR("async_print_and_wait_signal -> task cancelled, returning!");
      return;
    }
  }
}

// MCO coroutine that can resume/yield/die
// Yields into a schedulued wakeup
void async_print_and_delay(mco_coro *task) {
  int resumes = 0;
  int delay = (uintptr_t)task->user_data;
  ULOG_INFO("Coroutine started, resumes: %d. Delaying %d ms\r", resumes, delay);
  easync_task_delay(task, &eyra_app, delay);
  while (true) {
    resumes++;
    ULOG_INFO("Coroutine running, resumes: %d. Delaying %d ms\r", resumes, delay);
    easync_task_delay(task, &eyra_app, delay);
  }
}

char uart_buffer[1024];
// MCO coroutine that "transmits over UART", waits on TX completion, "registers to receive on UART" and awaits RX completion
void uart_send_and_receive(mco_coro *task) {
  ULOG_INFO("started");
  // tell the evloop to send a "TX completion signal"
  uel_app_run_later(&eyra_app, 10, raise_signal_clo,(void*)(uintptr_t) SIGURG);
  ULOG_INFO("Sending message");
  snprintf(uart_buffer, 1024, "Sending some data: %p", task);
  ULOG_INFO(" Yielding until TX interrupt (SIGURG)");
  int rc = easync_task_await_signal(task, &eyra_app, SIGURG, 5000);
    if (rc == SIGNAL_TIMEOUT) {
      ULOG_ERROR("async_print_and_wait_signal -> timeout in waiting for signal, aborting!");
      return;
    }
    if (rc == SIGNAL_CANCELLED) {
      ULOG_ERROR("async_print_and_wait_signal -> task cancelled, returning!");
      return;
    }
  ULOG_INFO("TX completed, now waiting for response (SIGVTALRM)");
  // tell the evloop to send a "RX completion signal"
  uel_app_run_later(&eyra_app, 10, raise_signal_clo,(void*)(uintptr_t) SIGVTALRM);
  rc = easync_task_await_signal(task, &eyra_app, SIGVTALRM, 5000);
  ULOG_INFO("RX completed! Got %s", uart_buffer);

}
void *raise_signal_func(void *context, void *params) {
  uintptr_t signal = (uintptr_t) params;
  ULOG_INFO("sending signal");
  raise(signal);
}

void *easync_resume_coro_func(void *context, void *params) {
  mco_result res;
  mco_coro *co = (mco_coro *)params;
  if (mco_status(co) == MCO_SUSPENDED) {
    // Call `mco_resume` to start or resume the coroutine, directly entring the function
    mco_result res = mco_resume(co);
    assert(res == MCO_SUCCESS);
  }
  // The coroutine finished and should be now dead.
  if (mco_status(co) == MCO_DEAD) {
    ULOG_INFO("Coroutine %p is dead, unexpected", co);
    res = mco_destroy(co);
    assert(res == MCO_SUCCESS);
  }
  return NULL;
}
void *easync_timeout_coro_func(void *context, void *params) {
  mco_result res;
  mco_coro *co = (mco_coro *)params;
  if (mco_status(co) == MCO_SUSPENDED) {
    // Call `mco_uninit` to stop the coroutine from getting started.
    ULOG_ERROR("Coroutine %p timed out, signaling timeout", co);
    uint8_t val = SIGNAL_TIMEOUT;
    mco_push(co, &val, 1);
    mco_result res = mco_resume(co);
    assert(res == MCO_SUCCESS);
  }

  if (mco_status(co) == MCO_DEAD) {
    res = mco_destroy(co);
    assert(res == MCO_SUCCESS);
  }
  return NULL;
}

/// start timer
// Poor mans timer interrupt :(
int timerfd = 0;
void wait_timer() {
  uint64_t exp, tot_exp, max_exp;
  ssize_t s = read(timerfd, &exp, sizeof(uint64_t));
  if (s != sizeof(uint64_t))
    err(EXIT_FAILURE, "read");

  tot_exp += exp;
  easync_timer_isr();
}
void setup_timer(void) {
  struct itimerspec new_value = {{0, 1000 * 1000}, {0, 1000 * 1000}};
  timerfd = timerfd_create(CLOCK_MONOTONIC, 0);
  if (timerfd == -1)
    err(EXIT_FAILURE, "timerfd_create");

  if (timerfd_settime(timerfd, 0, &new_value, NULL) == -1)
    err(EXIT_FAILURE, "timerfd_settime");

  ULOG_INFO("timer started");
}
/// end timer

/// Signal handler / poor-mans TX/RX interruot
void interrupt_handler(int sig) {
  static int current_val = 0;
  // Check if anyone is waiting for the signal
  mco_coro *co = uel_cqueue_pop(&easync_signal_closure_queue[sig]);
  if (co == NULL) {
    ULOG_INFO("No task waiting for signal %d", sig);
    return;
  }
  ULOG_INFO("coroutine %p waiting to resume=> %s", co, mco_state_str[mco_status(co)]);
  // If the coroutine is suspended, resume it
  // Anything else here would be weird
  if (mco_status(co) == MCO_SUSPENDED) {
    uel_app_enqueue_closure(&eyra_app, &easync_resume_coro_clo, co);
  } else {
    ULOG_ERROR("Attempting to resume coroutine %p in state: %s", co, mco_state_str[mco_status(co)]);
  }
}
void usr1_handler(int sig) {
  static int current_val = 0;
  // Check if anyone is waiting for the signal
  keep_running = false;
}

void cont_handler(int sig){
  // spawn a coroutine that transmits over UART, and receives a reply
  easync_spawn_coroutine(uart_send_and_receive, (void *)(uintptr_t)1000);
}

int main(int argc, char **argv) {
  ULOG_INIT();
  fp_log_file = fopen("cor.log", "w+");
  ULOG_SUBSCRIBE(my_console_logger, ULOG_DEBUG_LEVEL);
  ULOG_SUBSCRIBE(my_file_logger, ULOG_DEBUG_LEVEL);
  // dynamically change the threshold for a specific logger
  /* ULOG_SUBSCRIBE(my_console_logger, ULOG_INFO_LEVEL); */

  ULOG_INFO("Starting.."); // logs to file and console
  // remove a logger
  /* ULOG_UNSUBSCRIBE(my_file_logger); */
  signal(SIGUSR1, usr1_handler);
  signal(SIGUSR2, interrupt_handler);
  signal(SIGURG, interrupt_handler);
  signal(SIGCONT, cont_handler);
  signal(SIGVTALRM, interrupt_handler);
  uel_app_init(&eyra_app);
  setup_timer();

  // Create 32 queues, each with 16 (2**4) slots
  // Park coroutines here while they are waiting for signals
  for (int i = 0; i < 32; i++) {
    uel_cqueue_init(&easync_signal_closure_queue[i],
                    &_easync_signal_closure_buffer[i * (1 << SIGNAL_QUEUE_BUFFER_SIZE_LOG2N)],
                    SIGNAL_QUEUE_BUFFER_SIZE_LOG2N);
  }

  // Initalize the space for the coroutine-stacks
  // Used by MCO_ALLOC / MCO_DEALLOC
  uel_objpool_init(&coro_stack_pool, CORO_STACK_POOL_SIZE_LOG2N, sizeof(coro_stack_t), UEL_OBJPOOL_BUFFERS(coro_stack_pool));

  // Initalize the closure that resumes coroutines passed as argument
  // Context == null, coroutine comes in param
  easync_resume_coro_clo = uel_closure_create(&easync_resume_coro_func, (void *)NULL);
  easync_timeout_coro_clo = uel_closure_create(&easync_timeout_coro_func, (void *)NULL);
  raise_signal_clo = uel_closure_create(&raise_signal_func, (void *)NULL);

  uintptr_t value = 0;
  uel_closure_t print_values = uel_closure_create(&print_value, (void *)2);
  timer_handle = uel_app_run_at_intervals(&eyra_app, 1000, true, print_values, (void *)6);
  easync_spawn_coroutine(async_print_and_delay, (void *)(uintptr_t)2000);
  easync_spawn_coroutine(async_print_and_delay, (void *)(uintptr_t)1000);
  easync_spawn_coroutine(async_print_and_wait_signal, NULL);

  while (keep_running) {
    wait_timer();
    uel_app_tick(&eyra_app);
  }
  ULOG_INFO("Quitting");
  if(fp_log_file)
    fclose(fp_log_file);
}
bool easync_spawn_coroutine(coroutine_func fp, void *user_data) {
  mco_desc desc = mco_desc_init(fp, 0);
  desc.user_data = user_data;
  mco_coro *co;
  mco_result res = mco_create(&co, &desc);
  assert(res == MCO_SUCCESS);
  assert(mco_status(co) == MCO_SUSPENDED);
  uel_app_enqueue_closure(&eyra_app, &easync_resume_coro_clo, co);
  return true;
}

char *get_timestamp(void) {
  struct timespec tv;
  char time_str[127];
  double fractional_seconds;
  int milliseconds;
  struct tm tm; // our "broken down time"

  if (clock_gettime(CLOCK_REALTIME, &tv) == -1) {
    perror("clock_gettime");
    exit(EXIT_FAILURE);
  }

  memset(&tm, 0, sizeof(struct tm));
  sprintf(time_str, "%ld UTC", tv.tv_sec);

  // convert our timespec into broken down time
  strptime(time_str, "%s %U", &tm);

  // do the math to convert nanoseconds to integer milliseconds
  fractional_seconds = (double)tv.tv_nsec;
  fractional_seconds /= 1e6;
  fractional_seconds = round(fractional_seconds);
  milliseconds = (int)fractional_seconds;

  // print date and time without milliseconds

  //ISO8601
  //strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%S", &tm);
  strftime(time_str, sizeof(time_str), "%H:%M:%S", &tm);

  // add on the fractional seconds and Z for the UTC Timezone
  snprintf(timestamp, sizeof(timestamp), "%s.%.5d", time_str, milliseconds);

  return timestamp;
}
