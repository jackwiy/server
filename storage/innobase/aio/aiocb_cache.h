#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>
#include "aio0aio.h"

class aiocb_cache
{
  std::mutex mtx;
  std::condition_variable cv_not_empty;
  std::queue<aiocb *> cache;
  size_t pos;
public:
  aiocb_cache(size_t count):mtx(), cv_not_empty(), cache()
  {
    for(auto i = 0 ; i < count; i++)
      cache.push(new aiocb);
  }

  aiocb* acquire()
  {
    std::unique_lock<std::mutex> lk(mtx);
    while(cache.empty())
      cv_not_empty.wait(lk);
    aiocb *cb = cache.front();
    cache.pop();
    return cb;
  }
  
  void release(aiocb *cb)
  {
    std::unique_lock<std::mutex> lk(mtx);
    cache.push(cb);
  }
};