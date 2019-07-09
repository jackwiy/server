#include <windows.h>
#include "tp0tp.h"
#include <atomic>

namespace threadpool
{

  class threadpool_win :public threadpool
  {
    std::atomic<int> counter;
public:

    virtual void submit(const task* tasks, int size) override
    {
      for (auto i = 0; i < size; i++)
      {
        if (!TrySubmitThreadpoolCallback(tasks[i].m_func, tasks[i].m_arg, 0))
          abort();
      }
    }
    virtual void shutdown() override
    {
    }
  };
  threadpool* create_threadpool_win()
  {
    return new threadpool_win();
  }
}
