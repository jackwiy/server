#pragma once
#ifdef _WIN32
#ifndef NOMINMAX  
#define NOMINMAX
#endif
#include <windows.h>
#else
#define  PTP_CALLBACK_INSTANCE void*
#define  CALLBACK
#endif
namespace threadpool
{
  typedef void (CALLBACK *callback_func)(PTP_CALLBACK_INSTANCE,void*);

  struct task
  {
    callback_func m_func;
    void* m_arg;
  };

  class threadpool
  {
  public:
    virtual void submit(const task* tasks, int size) = 0;
    void submit(const task& t) { submit(&t,1); }
    virtual void shutdown() = 0;
    /* Following functions are there just for the Windows native threadpool.*/
    virtual bool is_native() { return false; }
    virtual void* get_native_env() { return 0; }
    virtual ~threadpool(){}
  };

  extern threadpool* create_threadpool_generic();
#ifdef _WIN32
  extern threadpool* create_threadpool_win();
#endif

  inline threadpool* create_threadpool(bool native = true)
  {
#ifdef _WIN32
    if (native)
      return create_threadpool_win();
 #endif
      
    return create_threadpool_generic();
  }
}
