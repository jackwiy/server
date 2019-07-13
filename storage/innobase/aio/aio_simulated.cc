#include "aio0aio.h"
#include <mutex>
#ifndef _WIN32
#include <unistd.h> /* pread(), pwrite() */
#endif
#include <string.h>
#include "aiocb_cache.h"

class simulated_aio;

struct simulated_iocb
{
  native_file_handle fd;
  aio_opcode opcode;
  unsigned long long offset;
  void* buffer;
  unsigned int len;
  simulated_aio *aio;
  char userdata[MAX_AIO_USERDATA_LEN];
};


#ifdef _WIN32
struct sync_io_event
{
  HANDLE m_event;
  sync_io_event()
  {
    m_event = CreateEvent(0, FALSE, FALSE, 0);
    m_event = (HANDLE)(((uintptr_t)m_event)|1);
  }
  ~sync_io_event()
  {
    m_event = (HANDLE)(((uintptr_t)m_event) & ~1);
    CloseHandle(m_event);
  }
};
static thread_local sync_io_event sync_event;

static int pread(const native_file_handle& h, void* buf, size_t count, unsigned long long offset)
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

static int pwrite(const native_file_handle& h, void* buf, size_t count, unsigned long long offset)
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
  threadpool::threadpool* m_tp; 
  aio_cache<simulated_iocb> m_read_cache;
  aio_cache<simulated_iocb> m_write_cache;

public:
  simulated_aio(threadpool::threadpool* tp, size_t read_threads, size_t write_threads):
    m_tp(tp), m_read_cache(read_threads,NOTIFY_ONE),m_write_cache(write_threads, NOTIFY_ONE)
  {
  }
 
  static void CALLBACK execute_io_completion(PTP_CALLBACK_INSTANCE, void* param)
  {
    simulated_iocb iocb= *(simulated_iocb *)param;
    simulated_aio *aio = iocb.aio;
    int ret_len;
    int err = 0;
    switch (iocb.opcode)
    {
      case AIO_PREAD:
        ret_len = pread(iocb.fd, iocb.buffer, iocb.len, iocb.offset);
        aio->m_read_cache.put((simulated_iocb*)param);
        break;
      case AIO_PWRITE:
        ret_len = pwrite(iocb.fd, iocb.buffer, iocb.len, iocb.offset);
        aio->m_write_cache.put((simulated_iocb*)param);
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

  virtual int submit(const native_file_handle& fd, aio_opcode opcode, unsigned long long offset, void* buffer, unsigned int len, void* userdata, size_t userdata_len) override
  {
    simulated_iocb *iocb= opcode == AIO_PREAD? m_read_cache.get(): m_write_cache.get();
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

aio* create_simulated_aio(threadpool::threadpool* tp, size_t read_threads, size_t write_threads)
{
  return new simulated_aio(tp, read_threads, write_threads);
}
