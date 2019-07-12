#include <windows.h>
#include "tp0tp.h"
#include <atomic>

namespace threadpool
{

  class threadpool_win :public threadpool
  {
    PTP_CALLBACK_ENVIRON m_env;

public:

    threadpool_win(PTP_CALLBACK_ENVIRON env):m_env(env){};

    virtual void submit(const task* tasks, int size) override
    {
      for (auto i = 0; i < size; i++)
      {
        if (!TrySubmitThreadpoolCallback(tasks[i].m_func, tasks[i].m_arg, m_env))
          abort();
      }
    }
    virtual void shutdown() override
    {
    }
    bool is_native() override
    {
      return true;
    }
    void* get_native_env()
    {
      return m_env;
    }
  };

  
  threadpool* create_threadpool_win()
  {
    return new threadpool_win(nullptr);
  }
}
