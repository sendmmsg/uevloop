#include "uevloop/utils/linked-list.h"
#include "uevloop/utils/module.h"
#define MINICORO_IMPL
#include "minicoro.h"
#include "uevloop/system/containers/application.h"
#include "uevloop/system/containers/system-pools.h"
#include "uevloop/system/containers/system-queues.h"
#include "uevloop/system/event-loop.h"
#include "uevloop/system/scheduler.h"
#include "uevloop/utils/closure.h"
#include "uevloop/utils/circular-queue.h"
#include <bits/types/timer_t.h>
#include <stdint.h>
#include <stdio.h>

#include <err.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

bool keep_running = true;
// Create system containers
/* uel_syspools_t pools; */
/* uel_sysqueues_t queues; */
// uel_evloop_t loop;
//struct uel_evloop loop;
static volatile uint32_t counter = 0;
/* uel_scheduer_t scheduler; */
uel_event_t *timer_handle;
/* static uel_application_t eyra_app; */
uel_application_t my_app;

typedef void (*coroutine_func)(mco_coro*);
bool create_and_enqueue_coroutine(coroutine_func fp);
// Tick the timer every ms
void eyra_timer_isr() { uel_app_update_timer(&my_app, ++counter); }

static void *print_value(void *context, void *parameters) {
  uintptr_t v = (uintptr_t)parameters;
  int enqueued = uel_sysqueues_count_enqueued_events(&my_app.queues);
  int scheduled = uel_sysqueues_count_scheduled_events(&my_app.queues);
  printf("value: %ld queued: %d scheduled: %d\n", v, enqueued, scheduled);

  return NULL;
}
static void *delay_print_value(void *context, void *parameters) {
  uintptr_t v = (uintptr_t)parameters;
  int enqueued = uel_sysqueues_count_enqueued_events(&my_app.queues);
  int scheduled = uel_sysqueues_count_scheduled_events(&my_app.queues);
  printf("delayed: %ld queued: %d scheduled: %d\n", v, enqueued, scheduled);
  return NULL;
}

static void *abort_loop(void *context, void *parameters) {
  uintptr_t v = (uintptr_t)parameters;
  keep_running = false;
  return NULL;
}
/**
 * Schedule a uel closure that calls to mco_resume(task) in the future.
 * Then yield, returning to the closure that woke us up
 * Basically our version of "await Task.Delay()"
 */
void easync_task_delay(mco_coro *task, uel_application_t* app, int delay_ms){
  uel_closure_t *resume_coroutine_c = mco_get_user_data(task);
  uel_app_run_later(app, delay_ms, *resume_coroutine_c, task);
  mco_yield(task);
}

// 16 entries circular queue
#define SIGNAL_QUEUE_BUFFER_SIZE_LOG2N   (4)
uel_cqueue_t signal_closure_queue[32];
void *signal_closure_buffer[32*(1<<SIGNAL_QUEUE_BUFFER_SIZE_LOG2N)];
void easync_task_await_signal(mco_coro *task, uel_application_t* app, int signal){
  assert(signal >= 0 &&  signal < 32);
  if(!uel_cqueue_push(&signal_closure_queue[signal], task)){
    fprintf(stderr, "Failed to enqueue task %p resume for signal %d\n", task, signal);
    return;
  }
  mco_yield(task);
}


// MCO coroutine that can resume/yield/die
// Yields into a wakeup from signal USR1
void async_print_and_wait_signal(mco_coro *co){
    int resumes = 0;
    int delay = 1000;
    printf("Coroutine started, resumes: %d. Waiting for signal USR2 (%d)\r\n", resumes, SIGUSR2);
    easync_task_await_signal(co, &my_app, SIGUSR2);
    while(true){
        resumes++;
        printf("Coroutine started, resumes: %d. Waiting for signal USR2 (%d)\r\n", resumes, SIGUSR2);
        easync_task_delay(co, &my_app, SIGUSR2);
    }
}

// MCO coroutine that can resume/yield/die
// Yields into a schedulued wakeup
void async_print_and_delay(mco_coro *co){
    int resumes = 0;
    int delay = 1000;
    printf("Coroutine started, resumes: %d. Delaying %d ms\r\n", resumes,delay);
    easync_task_delay(co, &my_app, 1000);
    while(true){
        resumes++;
        printf("Coroutine running, resumes: %d. Delaying %d ms\r\n", resumes,delay);
        easync_task_delay(co, &my_app, 1000);
    }
}

void *spawn_and_resume_coro(void *context, void *params) {
  mco_result res;
  mco_coro *co = (mco_coro *)params;
  if (mco_status(co) == MCO_SUSPENDED) {
    // Call `mco_resume` to start for the first time, switching to its context.
    mco_result res = mco_resume(co); // Should print "coroutine 1".
    assert(res == MCO_SUCCESS);
  }
  // Call `mco_resume` to resume for a second time.
  // The coroutine finished and should be now dead.
  if (mco_status(co) == MCO_DEAD) {
    res = mco_destroy(co);
    assert(res == MCO_SUCCESS);
  }
  return NULL;
}

int timerfd = 0;
void wait_timer() {
  uint64_t exp, tot_exp, max_exp;
  ssize_t s = read(timerfd, &exp, sizeof(uint64_t));
  if (s != sizeof(uint64_t))
    err(EXIT_FAILURE, "read");

  tot_exp += exp;
  eyra_timer_isr();
}
void setup_timer(void) {
  struct itimerspec new_value = {{0, 1000 * 1000}, {0, 1000 * 1000}};
  timerfd = timerfd_create(CLOCK_MONOTONIC, 0);
  if (timerfd == -1)
    err(EXIT_FAILURE, "timerfd_create");

  if (timerfd_settime(timerfd, 0, &new_value, NULL) == -1)
    err(EXIT_FAILURE, "timerfd_settime");

  printf("timer started\n");
}
/* void usr1_handler(int sig) { */
/*   static int current_val = 0; */
/*   printf("Signal handler usr1 called!\n"); */

/*   uel_closure_t run_coro_clo = uel_closure_create(&spawn_and_resume_coro, (void *)NULL); */
/*   mco_desc desc = mco_desc_init(send_and_response, 0); */
/*   desc.user_data = &run_coro_clo; */
/*   mco_coro *co; */
/*   mco_result res = mco_create(&co, &desc); */
/*   assert(res == MCO_SUCCESS); */
/*   assert(mco_status(co) == MCO_SUSPENDED); */

/*   uel_app_enqueue_closure(&my_app, &run_coro_clo, co); */

/* } */
void usr2_handler(int sig) {
  static int current_val = 0;
  mco_coro *co = uel_cqueue_pop(&signal_closure_queue[SIGUSR2]);
  if(co == NULL){
    printf("No task waiting for signal %d\n", SIGUSR2);
    return;
  }
  printf("mco_status => %d\n", mco_status(co));
  assert(mco_status(co) == MCO_SUSPENDED);
  uel_closure_t run_coro_clo = uel_closure_create(&spawn_and_resume_coro, (void *)NULL);
  uel_app_enqueue_closure(&my_app, &run_coro_clo, co);
}



uel_closure_t run_coro_clo;
int main(int argc, char **argv) {
  /* signal(SIGUSR1, usr1_handler); */
  signal(SIGUSR2, usr2_handler);
  uel_app_init(&my_app);
  setup_timer();

  // Create 32 queues, each with 16 (2**4) slots
  // Park coroutines here while they are waiting for signals
  for(int i = 0; i < 32 ; i++){
    uel_cqueue_init(&signal_closure_queue[i], &signal_closure_buffer[i*(1<<SIGNAL_QUEUE_BUFFER_SIZE_LOG2N)], SIGNAL_QUEUE_BUFFER_SIZE_LOG2N);
  }
  run_coro_clo = uel_closure_create(&spawn_and_resume_coro, (void *)NULL);

  uintptr_t value = 0;
  uel_closure_t print_values = uel_closure_create(&print_value, (void *)2);
  timer_handle = uel_app_run_at_intervals(&my_app, 1000, true, print_values, (void *)6);


  /* uel_closure_t run_coro_clo = uel_closure_create(&spawn_and_resume_coro, (void *)NULL); */
  /* mco_desc desc = mco_desc_init(async_print_and_delay, 0); */
  /* desc.user_data = &run_coro_clo; */
  /* mco_coro *co; */
  /* mco_result res = mco_create(&co, &desc); */
  /* assert(res == MCO_SUCCESS); */
  /* assert(mco_status(co) == MCO_SUSPENDED); */
  /*uel_app_enqueue_closure(&my_app, &run_coro_clo, co); */
  create_and_enqueue_coroutine(async_print_and_delay);

  while (keep_running) {
    wait_timer();
    uel_app_tick(&my_app);
  }
}
bool create_and_enqueue_coroutine(coroutine_func fp){
  mco_desc desc = mco_desc_init(fp, 0);
  desc.user_data = &run_coro_clo;
  mco_coro *co;
  mco_result res = mco_create(&co, &desc);
  assert(res == MCO_SUCCESS);
  assert(mco_status(co) == MCO_SUSPENDED);
  uel_app_enqueue_closure(&my_app, &run_coro_clo, co);
  return true;
}
