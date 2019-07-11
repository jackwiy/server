#include "aio0aio.h"
#include <mutex>
#ifndef _WIN32
#include <unistd.h> /* pread(), pwrite() */
#endif
#include <string.h>
class simulated_aio;

struct simulated_iocb
{
  simulated_iocb *next;
  native_file_handle fd;
  aio_opcode opcode;
  unsigned long long offset;
  void* buffer;
  unsigned int len;
  simulated_aio *aio;
  char userdata[MAX_AIO_USERDATA_LEN];
};

struct simulated_iocb_cache
{
  simulated_iocb *m_head;
  std::mutex m_mtx;
  simulated_iocb_cache() :m_mtx(), m_head()
  {

  }
  simulated_iocb* get()
  {
   simulated_iocb *ret = 0;
   m_mtx.lock();
   if(m_head)
   {
     ret = m_head;
     m_head = m_head->next;
   }
   m_mtx.unlock();
   return ret?ret:new simulated_iocb;
  }
  void put(simulated_iocb* iocb)
  {
    m_mtx.lock();
    iocb->next = m_head;
    m_head= iocb;
    m_mtx.unlock();
  }
  ~simulated_iocb_cache()
  {
    m_mtx.lock();
    for (;;)
    {
      simulated_iocb *cur=m_head;
      if (!cur)
        break;
      m_head = cur->next;
      delete cur;
    }
    m_mtx.unlock();
  }
};


#ifdef _WIN32
struct sync_io_event
{
  HANDLE m_event;
  sync_io_event()
  {
    m_event = CreateEvent(0, FALSE, FALSE, 0);
  }
  ~sync_io_event()
  {
    CloseHandle(m_event);
  }
};
static thread_local sync_io_event sync_event;
#
static int pread(native_file_handle h, void* buf, size_t count, unsigned long long offset)
{
  OVERLAPPED ov{};
  ULARGE_INTEGER uli;
  uli.QuadPart = offset;
  ov.Offset = uli.LowPart;
  ov.OffsetHigh = uli.HighPart;
  ov.hEvent = sync_event.m_event;
  bool ok = ReadFile(h, buf, (DWORD)count, NULL, &ov);
  DWORD n_bytes;
  if (ok || (GetLastError() == ERROR_IO_PENDING))
    ok = GetOverlappedResult(h, &ov, &n_bytes, TRUE);
  if (ok)
    return n_bytes;
  return -1;
}

static int pwrite(native_file_handle h, void* buf, size_t count, unsigned long long offset)
{
  OVERLAPPED ov{};
  ULARGE_INTEGER uli;
  uli.QuadPart = offset;
  ov.Offset = uli.LowPart;
  ov.OffsetHigh = uli.HighPart;
  ov.hEvent = sync_event.m_event;
  bool ok = WriteFile(h, buf, (DWORD)count, NULL, &ov);
  DWORD n_bytes;
  if (ok || (GetLastError() == ERROR_IO_PENDING))
    ok = GetOverlappedResult(h, &ov, &n_bytes, TRUE);
  if (ok)
    return n_bytes;
  return -1;
}
#endif

class simulated_aio :public aio
{
  simulated_iocb_cache m_iocb_cache;
  threadpool::threadpool *m_tp;
public:
  simulated_aio(threadpool::threadpool* tp):m_tp(tp),m_iocb_cache()
  {
  }
  // Inherited via aio
  virtual int bind(native_file_handle fd) override
  {
    return 0;
  }
  static void CALLBACK execute_io_completion(PTP_CALLBACK_INSTANCE, void* param)
  {
    simulated_iocb iocb= *(simulated_iocb *)param;
    simulated_aio *aio = iocb.aio;
    aio->m_iocb_cache.put((simulated_iocb*)param);
    int ret_len;
    int err = 0;
    switch (iocb.opcode)
    {
      case AIO_PREAD:
        ret_len = pread(iocb.fd, iocb.buffer, iocb.len, iocb.offset);
        break;
      case AIO_PWRITE:
        ret_len = pwrite(iocb.fd, iocb.buffer, iocb.len, iocb.offset);
        break;
      default:
        abort();
    }
    if (ret_len  < 0)
    {
#ifdef _WIN32
      err = GetLastError();
#else
      err = errno;
#endif  
    }
    aio->execute_callback(iocb.fd, iocb.opcode,iocb.offset, iocb.buffer, iocb.len,ret_len, err, iocb.userdata);
  }

  virtual int submit(native_file_handle fd, aio_opcode opcode, unsigned long long offset, void* buffer, unsigned int len, void* userdata, size_t userdata_len) override
  {
    simulated_iocb *iocb= m_iocb_cache.get();
    iocb->aio = this;
    iocb->buffer = buffer;
    iocb->fd = fd;
    iocb->len = len;
    iocb->offset = offset;
    iocb->opcode = opcode;
    memcpy(iocb->userdata,userdata, userdata_len);
    threadpool::task t{execute_io_completion, iocb};
    m_tp->submit(t);
    return 0;
  }
};

aio* create_simulated_aio(threadpool::threadpool* tp)
{
  return new simulated_aio(tp);
}
