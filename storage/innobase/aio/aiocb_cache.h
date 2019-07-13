#pragma once
#include <vector>
#include <stack>
#include <mutex>
#include <condition_variable>

enum aio_cache_notification_mode
{
  NOTIFY_ONE,
  NOTIFY_ALL
};

template<typename T> class aio_cache
{
  std::mutex m_mtx;
  std::condition_variable m_cv;
  std::vector<T>  m_base;
  std::vector<T*> m_cache;
  aio_cache_notification_mode m_notifcation_mode;
public:
  aio_cache(size_t count, aio_cache_notification_mode mode= NOTIFY_ALL):
  m_mtx(), m_cv(), m_base(count),m_cache(count), m_notifcation_mode(mode)
  {
    for(size_t i = 0 ; i < count; i++)
      m_cache[i]=&m_base[i];
  }

  T* get()
  {
    std::unique_lock<std::mutex> lk(m_mtx);
    while(m_cache.empty())
     m_cv.wait(lk);
    T* ret = m_cache.back();
    m_cache.pop_back();
    return ret;
  }
  
  void put(T *ele)
  {
    std::unique_lock<std::mutex> lk(m_mtx);
    m_cache.push_back(ele);
    if (m_notifcation_mode == NOTIFY_ONE)
      m_cv.notify_one();
    else if(m_cache.size() == 1)
      m_cv.notify_all();
  }
};