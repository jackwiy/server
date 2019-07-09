// threadpool.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <vector>
#include <atomic>
#include <queue>
#include <stack>

#include "tp0tp.h"

namespace threadpool
{

class threadpool_generic:public threadpool
{
  std::queue<task> m_tasks;
  std::stack<std::condition_variable *> m_standby_threads;
  std::mutex m_mtx;
  std::chrono::milliseconds m_thread_timeout;
  std::chrono::milliseconds m_timer_interval;
  std::condition_variable m_cv_no_active_threads;
  std::condition_variable m_cv_no_threads;
  std::condition_variable m_cv_shutdown;
  std::thread m_timer_thread;
  int m_threads;
  int m_active_threads;
  int m_tasks_dequeued;
  int m_concurrency;
  bool m_in_shutdown;
  bool m_stopped;

  void worker_main();
  void timer_main();
  bool add_thread();
  bool wake();

public:
  threadpool_generic();
  ~threadpool_generic() {shutdown();}
  void submit(const task* tasks, int size);
  void shutdown();

};

void threadpool_generic::worker_main()
{
  std::condition_variable cv;
  std::unique_lock<std::mutex> lk(m_mtx);

  for (;;)
  {
    task t;

    if (m_tasks.empty())
    {
      m_standby_threads.push(&cv);

      m_active_threads--;

      while(m_tasks.empty() && !m_in_shutdown)
        cv.wait_for(lk, m_thread_timeout);

      m_active_threads++;

      if (m_tasks.empty())
        goto end;
    }

    t = m_tasks.front();
    m_tasks.pop();
    m_tasks_dequeued++;
    lk.unlock();

    t.m_func(0, t.m_arg);

    lk.lock();
  }
end:
  m_threads--;
  m_active_threads--;

  if (!m_threads && m_in_shutdown)
    m_cv_no_threads.notify_all();
}

void threadpool_generic::timer_main()
{
  int last_tasks_dequeued = 0;
  int last_threads = 0;
  for (;;)
  {
    std::unique_lock<std::mutex> lk(m_mtx);
    m_cv_shutdown.wait_for(lk, m_timer_interval);
    if (m_in_shutdown && m_tasks.empty())
      return;
    if (!m_tasks.empty()
      && last_tasks_dequeued == m_tasks_dequeued
      && last_threads <= m_threads
      && m_active_threads == m_threads)
    {
      // no progress made since last iteration. create new thread
      add_thread();
    }
    lk.unlock();
    last_tasks_dequeued = m_tasks_dequeued;
    last_threads = m_threads;
  }
}
bool threadpool_generic::add_thread()
{
  m_threads++;
  m_active_threads++;
  std::thread thread(&threadpool_generic::worker_main, this);
  thread.detach();
  return true;
}
bool threadpool_generic::wake()
{
  if (m_standby_threads.empty())
    return false;
  auto cv = m_standby_threads.top();
  m_standby_threads.pop();
  cv->notify_one();
  return true;
}
threadpool_generic::threadpool_generic() :
  m_concurrency(std::thread::hardware_concurrency()),
  m_tasks(),
  m_mtx(),
  m_standby_threads(),
  m_cv_no_threads(),
  m_cv_shutdown(),
  m_threads(),
  m_active_threads(),
  m_tasks_dequeued(),
  m_timer_interval(std::chrono::milliseconds(10)),
  m_thread_timeout(std::chrono::milliseconds(60000)),
  m_in_shutdown(),
  m_stopped()
{
  m_timer_thread = std::thread(&threadpool_generic::timer_main, this);
}

void threadpool_generic::submit(const task* tasks, int size)
{
  auto now = std::chrono::high_resolution_clock::now();
  std::unique_lock<std::mutex> lk(m_mtx);
  for (auto i = 0; i < size; i++)
  {
    task t(tasks[i]);
    m_tasks.push(tasks[i]);
  }
  for (auto i = 0; i < std::min(m_concurrency - m_active_threads, size); i++)
  {
    if (!wake() && !m_in_shutdown)
      add_thread();
  }
}

void threadpool_generic::shutdown()
{
  std::unique_lock<std::mutex> lk(m_mtx);
  if (m_stopped)
    return;
  m_in_shutdown = true;

  /* Wake up idle threads. */
  while (!m_standby_threads.empty())
  {
    auto cv = m_standby_threads.top();
    m_standby_threads.pop();
    cv->notify_one();
  }

  while (m_threads)
    m_cv_no_threads.wait(lk);

  /* notify timer, too.*/
  m_cv_shutdown.notify_all();

  lk.unlock();
  m_timer_thread.join();
  m_stopped = true;
}

threadpool* create_threadpool_generic()
{
  return new threadpool_generic();
}
}

