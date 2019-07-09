#include <windows.h>
#include <malloc.h>
#include <stdlib.h>

#include "aio0aio.h"
#include "tp0tp.h"

class win_aio;

struct OVERLAPPED_EXTENDED: OVERLAPPED
{
  HANDLE fd;
  void* userdata;
  void* buffer;
  win_aio* aio;
  unsigned int len;
  unsigned int bytes_transferred;
  int err;
  enum aio_opcode opcode;
};

struct OVERLAPPED_SLIST_ENTRY: SLIST_ENTRY
{
  OVERLAPPED_EXTENDED ov;
};

struct overlapped_cache
{
  SLIST_HEADER* list_head;
  overlapped_cache()
  {
    list_head= (SLIST_HEADER*)_aligned_malloc(sizeof(OVERLAPPED_SLIST_ENTRY),MEMORY_ALLOCATION_ALIGNMENT);
    if(list_head)
    {
      InitializeSListHead(list_head);
      return;
    }
    abort();
  }
  ~overlapped_cache()
  {
    for(;;)
    {
      SLIST_ENTRY* entry=InterlockedPopEntrySList(list_head);
      if(!entry)
        break;
    }
    _aligned_free(list_head);
  }
  OVERLAPPED_EXTENDED* get()
  {
    OVERLAPPED_SLIST_ENTRY* entry = (OVERLAPPED_SLIST_ENTRY *)InterlockedPopEntrySList(list_head);
    if (entry)
      return &entry->ov;
    entry= (OVERLAPPED_SLIST_ENTRY*)_aligned_malloc(sizeof(OVERLAPPED_SLIST_ENTRY), MEMORY_ALLOCATION_ALIGNMENT);
    if (entry)
    {
      memset(entry, 0, sizeof(*entry));
      return &entry->ov;
    }
    abort();
  }
  void put(OVERLAPPED_EXTENDED* ov)
  {
    OVERLAPPED_SLIST_ENTRY *entry = (OVERLAPPED_SLIST_ENTRY *)((char *)ov - offsetof(OVERLAPPED_SLIST_ENTRY, ov));
    InterlockedPushEntrySList(list_head, entry);
  }
};


class win_aio:public aio
{
  HANDLE m_completion_port;
  overlapped_cache m_overlapped_cache;
  HANDLE m_io_completion_thread;
  threadpool::threadpool *m_pool;

public:
  win_aio(threadpool::threadpool *pool):m_pool(pool),m_completion_port(),m_overlapped_cache()
  {
    m_completion_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE,0,0,0);
    DWORD tid;
    m_io_completion_thread = CreateThread(0, 0, io_completion_routine, this, 0,  &tid);
  }
  static void CALLBACK execute_io_completion(PTP_CALLBACK_INSTANCE, void* param)
  {
    OVERLAPPED_EXTENDED ov = *(OVERLAPPED_EXTENDED*)param;
    ov.aio->m_overlapped_cache.put((OVERLAPPED_EXTENDED*)param);
    int err = 0;
    if (ov.len != ov.bytes_transferred)
    {
      if (!GetOverlappedResult(ov.fd, &ov, (DWORD*) & (ov.bytes_transferred), FALSE))
        err = GetLastError();
    }
    ov.aio->m_callback(ov.fd, ov.opcode, (((unsigned long long)ov.OffsetHigh) << 32) + ov.Offset , ov.buffer, ov.len, ov.bytes_transferred, err, ov.userdata);
  }
  static DWORD WINAPI io_completion_routine(void* par)
  {
    OVERLAPPED_ENTRY arr[64];
    ULONG count;
    win_aio* aio = (win_aio*)par;

    while (GetQueuedCompletionStatusEx(aio->m_completion_port, arr, 64, &count, INFINITE, FALSE))
    {
      for (ULONG i = 0; i < count; i++)
      {
        OVERLAPPED_EXTENDED* ov = (OVERLAPPED_EXTENDED*)arr[i].lpOverlapped;
        ov->bytes_transferred = arr[i].dwNumberOfBytesTransferred;

        threadpool::task t;
        t.m_func = execute_io_completion;
        t.m_arg = ov;
        aio->m_pool->submit(t);
      }
    }
    return 0;
  }
  // Inherited via aio
  virtual int bind(native_file_handle fd) override
  {
    HANDLE h = CreateIoCompletionPort(fd, m_completion_port, 0, 0);
    if (h == INVALID_HANDLE_VALUE || !h)
      return -1;
    return 0;
  }
  virtual int submit(native_file_handle fd, aio_opcode opcode, unsigned long long offset, void* buffer, unsigned int len, void* userdata) override
  {
    OVERLAPPED_EXTENDED* ov = m_overlapped_cache.get();
    ov->fd = fd;
    ov->userdata = userdata;
    ov->Offset = (DWORD)offset;
    ov->OffsetHigh = (DWORD)(offset >> 32);
    ov->len = len;
    ov->buffer = buffer;
    ov->opcode = opcode;
    ov->aio= this;
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

  ~win_aio()
  {
    CloseHandle(m_completion_port);
    WaitForSingleObject(m_io_completion_thread, INFINITE);
  }
};

aio* create_win_aio(threadpool::threadpool* tp)
{
  return new win_aio(tp);
}