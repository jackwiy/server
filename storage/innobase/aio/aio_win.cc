#include <windows.h>
#include <malloc.h>
#include <stdlib.h>

#include "aio0aio.h"
#include "aiocb_cache.h"
#include "tp0tp.h"

class win_aio;
class win_aio_generic;

struct OVERLAPPED_EXTENDED: OVERLAPPED
{
  native_file_handle fd;
  void* buffer;
  void* aio;
  unsigned int len;
  unsigned int bytes_transferred;
  int err;
  enum aio_opcode opcode;
  char userdata[MAX_AIO_USERDATA_LEN];
};

static int do_io(aio_opcode opcode, 
  OVERLAPPED *ov, 
  const native_file_handle& fd,
  unsigned long long offset, 
  unsigned int len,
  void *buffer
)
{
  ov->Offset = (DWORD)offset;
  ov->OffsetHigh = (DWORD)(offset >> 32);
  BOOL ret;
  switch (opcode)
  {
  case AIO_PREAD:
    ret = ReadFile(fd, buffer, len, nullptr, ov);
    break;
  case AIO_PWRITE:
    ret = WriteFile(fd, buffer, len, nullptr, ov);
    break;
  default:
    abort();
  }
  if (ret || GetLastError() == ERROR_IO_PENDING)
    return 0;
  return -1;
}

class win_aio_generic:public aio
{
  HANDLE m_completion_port;
  aio_cache<OVERLAPPED_EXTENDED> m_overlapped_cache;
  HANDLE m_io_completion_thread;
  threadpool::threadpool *m_pool;
public:
  win_aio_generic(threadpool::threadpool *pool,size_t max_io_count): m_pool(pool),m_completion_port(),m_overlapped_cache(max_io_count)
  {
    m_completion_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE,0,0,0);
    DWORD tid;
    m_io_completion_thread = CreateThread(0, 0, io_completion_routine, this, 0,  &tid);
  }
  static void CALLBACK execute_io_completion(PTP_CALLBACK_INSTANCE, void* param)
  {
    OVERLAPPED_EXTENDED ov = *(OVERLAPPED_EXTENDED*)param;
    win_aio_generic *aio=(win_aio_generic*)ov.aio;
    aio->m_overlapped_cache.put((OVERLAPPED_EXTENDED*)param);
    int err = 0;
    if (ov.len != ov.bytes_transferred)
    {
      if (!GetOverlappedResult(ov.fd, &ov, (DWORD*) & (ov.bytes_transferred), FALSE))
        err = GetLastError();
    }
    unsigned long long offset = (((unsigned long long)ov.OffsetHigh) << 32) + ov.Offset;
    aio->execute_callback(ov.fd, ov.opcode, offset  , ov.buffer, ov.len, ov.bytes_transferred, err, ov.userdata);
  }
 
  static const int MAX_EVENT_COUNT=64;

  static DWORD WINAPI io_completion_routine(void* par)
  {
    OVERLAPPED_ENTRY arr[MAX_EVENT_COUNT];
    threadpool::task tasks[MAX_EVENT_COUNT];
    ULONG count;
    win_aio_generic* aio = (win_aio_generic*)par;

    while (GetQueuedCompletionStatusEx(aio->m_completion_port, arr, 64, &count, INFINITE, FALSE))
    {
      for (ULONG i = 0; i < count; i++)
      {
        OVERLAPPED_EXTENDED* ov = (OVERLAPPED_EXTENDED*)arr[i].lpOverlapped;
        ov->bytes_transferred = arr[i].dwNumberOfBytesTransferred;
        tasks[i].m_arg = ov;
        tasks[i].m_func = execute_io_completion;
      }
      if (count)
      {
        aio->m_pool->submit(tasks,count);
      }
    }
    return 0;
  }
  // Inherited via aio
  virtual int bind(native_file_handle& fd) override
  {
    HANDLE h = CreateIoCompletionPort(fd, m_completion_port, 0, 0);
    if (h == INVALID_HANDLE_VALUE || !h)
      return -1;
    return 0;
  }
  
  virtual int submit(const native_file_handle& fd, aio_opcode opcode, unsigned long long offset, void* buffer, unsigned int len, void* userdata, size_t userdata_len) override
  {
    OVERLAPPED_EXTENDED* ov = m_overlapped_cache.get();
    ov->fd = fd;
    ov->aio = this;
    ov->len = len;
    ov->buffer = buffer;
    ov->opcode = opcode;
    memcpy(ov->userdata,userdata, userdata_len);
    return do_io(opcode, ov, fd,offset, len, buffer);
  }
  ~win_aio_generic()
  {
    CloseHandle(m_completion_port);
    WaitForSingleObject(m_io_completion_thread, INFINITE);
  }
};

class win_aio_native_tp :public aio
{
  aio_cache<OVERLAPPED_EXTENDED> m_overlapped_cache;
  PTP_CALLBACK_ENVIRON m_env;
public:
  win_aio_native_tp(threadpool::threadpool* pool, size_t max_io_count) : m_overlapped_cache(max_io_count)
  {
    m_env = (PTP_CALLBACK_ENVIRON) pool->get_native_env();
  }
 
  static void CALLBACK io_completion_callback(PTP_CALLBACK_INSTANCE Instance, PVOID Context, PVOID Overlapped, ULONG  IoResult, ULONG_PTR  NumberOfBytesTransferred, PTP_IO  io)
  {
    OVERLAPPED_EXTENDED ov = *(OVERLAPPED_EXTENDED*)Overlapped;
    win_aio_native_tp *aio = (win_aio_native_tp * )ov.aio;
    aio->m_overlapped_cache.put((OVERLAPPED_EXTENDED*)Overlapped);
    unsigned long long offset = (((unsigned long long)ov.OffsetHigh) << 32) + ov.Offset;
    aio->execute_callback(ov.fd, ov.opcode, offset, ov.buffer, ov.len, (int)NumberOfBytesTransferred, (int)IoResult, ov.userdata);
  }
  
  virtual int bind(native_file_handle& fd) override
  {
    fd.m_ptp_io = CreateThreadpoolIo(fd.m_handle, io_completion_callback, 0, m_env);
    return fd.m_ptp_io ? 0 : -1;
  }

  virtual void unbind(const native_file_handle& fd) override
  {
    if (fd.m_ptp_io)
    {
      CloseThreadpoolIo(fd.m_ptp_io);
    }
  }

  virtual int submit(const native_file_handle& fd, aio_opcode opcode, unsigned long long offset, void* buffer, unsigned int len, void* userdata, size_t userdata_len) override
  {
    OVERLAPPED_EXTENDED* ov = m_overlapped_cache.get();
    ov->fd = fd;
    memcpy(ov->userdata, userdata, userdata_len);
    ov->aio = this;
    ov->len = len;
    ov->buffer = buffer;
    ov->opcode = opcode;
    
    StartThreadpoolIo(fd.m_ptp_io);
    if (do_io(opcode,ov, fd, offset, len,buffer))
    {
      CancelThreadpoolIo(fd.m_ptp_io);
      return -1;
    }
    return 0;
  }

  ~win_aio_native_tp()
  {
  }
};


aio* create_win_aio(threadpool::threadpool* tp, size_t max_io_count)
{
  if (tp->is_native())
    return new win_aio_native_tp(tp, max_io_count);
  return new win_aio_generic(tp, max_io_count);
}