#include <aio0aio.h>
#include "aiocb_cache.h"
#include <stdlib.h>
#include <libaio.h>
#include <pthread.h>
#include <signal.h>
#include <assert.h>

struct linux_iocb:iocb
{
  void* aio;
  int ret_len;
  int err;
  char userdata[MAX_AIO_USERDATA_LEN];
};

/*
  Create an asynchronous I/O context

  Can decrease nr_events, if it fails with EAGAIN.
  (meaning, that per-user resources were exceeded
  (/proc/sys/fs/aio-max-nr /proc/sys/fs/aio-nr)

  @return actual nr_events used with io_setup(),
          0 indicates an error.
*/
static int do_io_setup(io_context_t *ctx,int nr_events)
{
  int ret;
  do
  {
    memset(ctx, 0, sizeof(*ctx));
    ret = io_setup(nr_events, ctx);
    if (ret == 0)
      return nr_events;
    nr_events /= 2;
  }
  while(nr_events >= 128 && ret == -EAGAIN);
  return 0;
}

class aio_linux :public aio
{
  int m_max_io_count;
  threadpool::threadpool* m_tp;
  io_context_t m_io_ctx;
  aio_cache<linux_iocb> m_cache;
  bool m_in_shutdown;
  pthread_t m_getevent_thread;

  static void CALLBACK execute_io_completion(PTP_CALLBACK_INSTANCE, void* param)
  {
    linux_iocb *cb=(linux_iocb *)param;
    aio_linux *aio= (aio_linux *)cb->aio;
    linux_iocb iocb=*cb;
    aio->m_cache.put(cb);

    aio->execute_callback(
      iocb.aio_fildes, 
      iocb.aio_lio_opcode == IO_CMD_PREAD ? AIO_PREAD : AIO_PWRITE, 
      iocb.u.c.offset, 
      iocb.u.c.buf,
      iocb.u.c.nbytes,
      iocb.ret_len, 
      iocb.err, 
      iocb.userdata);
  }
  static const int MAX_EVENTS=64;
  static void* getevent_thread_routine(void* ptr)
  {
    aio_linux *aio=(aio_linux *)ptr;
    for (;;)
    {
      io_event events[MAX_EVENTS];
      int ret = io_getevents(aio->m_io_ctx, 1, MAX_EVENTS, events, nullptr);

      if (aio->m_in_shutdown)
        break;

      if (ret > 0)
      {
        for (auto i = 0; i < ret; i++)
        {
          linux_iocb *iocb= (linux_iocb *)events[i].obj;
          long long res = events[i].res;
          if (res < 0)
          {
            iocb->err = -res;
            iocb->ret_len = 0;
          }
          else
          {
            iocb->ret_len = ret;
            iocb->err = 0;
          }
          aio->m_tp->submit({execute_io_completion,iocb});
        }
        continue;
      }
      switch (ret) {
        case -EAGAIN:
          usleep(1000);
          continue;
        case -EINTR:
        case 0:
          continue;
        default:
          fprintf(stderr,"io_getenv returned %d\n",ret);
          abort();
      }
    }
    return nullptr;
  }

public:
  aio_linux(io_context_t ctx, threadpool::threadpool *tp, size_t max_count):
    m_max_io_count(max_count),
    m_tp(tp), 
    m_io_ctx(ctx),
    m_cache(max_count)
  {
    if (pthread_create(&m_getevent_thread,nullptr, getevent_thread_routine,this))
     abort();
  }
  
  ~aio_linux()
  {
    m_in_shutdown = true;
    io_destroy(m_io_ctx);
    pthread_join(m_getevent_thread, 0);
  }

  static const int IO_SUBMIT_EAGAIN_RETRIES = 100;
  static const int IO_SUBMIT_EAGAIN_SLEEP_US = 1000000;

  // Inherited via aio
  virtual int submit(const native_file_handle& fd, aio_opcode opcode, unsigned long long offset, void* buffer, unsigned int len, void* userdata, size_t userdata_len) override
  {
    linux_iocb *cb = m_cache.get();
    cb->aio = this;
    if (opcode == AIO_PREAD)
      io_prep_pread(cb, fd, buffer, len, offset);
    else
      io_prep_pwrite(cb,fd, buffer, len, offset);
    memcpy(cb->userdata,userdata, userdata_len);
    int ret;
    for(auto n_retries = 0;; n_retries++)
    {
      ret = io_submit(m_io_ctx, 1, (iocb **)&cb);
      if (ret == 1)
        return 0;
      if (ret == -EAGAIN && n_retries < IO_SUBMIT_EAGAIN_RETRIES)
      {
        usleep(IO_SUBMIT_EAGAIN_SLEEP_US);
        continue;
      }
    }
    errno = -ret;
    return -1;
  }
};

aio* create_linux_aio(threadpool::threadpool* tp, size_t max_io_count)
{
  io_context_t ctx;
  int real_max_count= do_io_setup(&ctx, max_io_count);
  if (!real_max_count)
    return 0;
  return new aio_linux(ctx,tp, real_max_count);
}