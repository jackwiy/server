#pragma once
#ifdef _WIN32
#include <windows.h>
struct native_file_handle
{
public:
  HANDLE  m_handle;
  PTP_IO  m_ptp_io;
  native_file_handle(){};
  native_file_handle(HANDLE h):m_handle(h),m_ptp_io(){}
  operator HANDLE() const {return m_handle;}
};
#else
#include <unistd.h>
typedef int native_file_handle;
#endif

#include <atomic>
#include <stdlib.h>
#include <stddef.h>

enum aio_opcode
{
  AIO_PREAD,
  AIO_PWRITE
};

typedef  void (* aio_callback_func)
(
  native_file_handle fh,
  aio_opcode opcode,
  unsigned long long offset,
  void *buffer,
  unsigned int len,
  int ret_len,
  int err,
  void *userdata
);

const size_t MAX_AIO_USERDATA_LEN = 40;

class aio
{
  aio_callback_func m_callback;
  std::atomic<int> m_pending_reads;
  std::atomic<int> m_pending_writes;
  void add_pending_counter(aio_opcode opcode, int count)
  {
    if (opcode == AIO_PREAD)
      m_pending_reads+=count;
    else
      m_pending_writes+=count;
  }

protected:
  aio() :m_callback(), m_pending_reads(), m_pending_writes()
  {
  }
  void execute_callback(const native_file_handle& fh, aio_opcode opcode,  unsigned long long offset, void* buffer, unsigned int len,
        int ret_len,  int err,  void* userdata )
  {
    m_callback(fh,opcode,offset,buffer, len, ret_len, err, userdata);
    add_pending_counter(opcode, -1);
  }
  virtual int submit(const native_file_handle& fd, aio_opcode opcode, unsigned long long offset, void* buffer, unsigned int len, void* userdata, size_t userdata_len) = 0;
public:
  void set_callback(aio_callback_func callback) { m_callback = callback;}
  virtual int bind(native_file_handle& fd) { return 0;};
  virtual void unbind(const native_file_handle &fd) {}
  int submit_io(const native_file_handle& fd, aio_opcode opcode, unsigned long long offset, void* buffer, unsigned int len, void* userdata, size_t userdata_len)
  {
    add_pending_counter(opcode, 1);
    int ret = submit(fd,opcode, offset, buffer, len, userdata, userdata_len);
    if (ret)
      add_pending_counter(opcode, -1);
    return ret;
  }
  void wait_for_pending_writes()
  {
    while(m_pending_writes)
    {
#ifdef _WIN32
      Sleep(1);
#else
      usleep(100000);
#endif
    }
  }
  virtual ~aio()
  {
    while(m_pending_reads || m_pending_writes)
    {
#ifdef _WIN32
      Sleep(1);
#else
      usleep(100000);
#endif
    }
  }
};

#include "tp0tp.h"
#ifdef _WIN32
extern aio* create_win_aio(threadpool::threadpool *tp);
#endif
extern aio* create_simulated_aio(threadpool::threadpool* tp);
