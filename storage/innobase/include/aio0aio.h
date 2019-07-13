#pragma once
#ifdef _WIN32
#include <windows.h>
struct native_file_handle
{
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
  void add_pending_counter(aio_opcode opcode, int count);
protected:
  aio() :m_callback(), m_pending_reads(), m_pending_writes(){}
  void execute_callback(const native_file_handle& fh, aio_opcode opcode,  unsigned long long offset, void* buffer, unsigned int len,
        int ret_len,  int err,  void* userdata );
  virtual int submit(const native_file_handle& fd, aio_opcode opcode, unsigned long long offset, void* buffer, unsigned int len, void* userdata, size_t userdata_len) = 0;
public:
  void set_callback(aio_callback_func callback) { m_callback = callback;}
  virtual int bind(native_file_handle& fd) { return 0;};
  virtual void unbind(const native_file_handle &fd) {}
  int submit_io(const native_file_handle& fd, aio_opcode opcode, unsigned long long offset, void* buffer, unsigned int len, void* userdata, size_t userdata_len);
  void wait_for_pending_writes();
  virtual ~aio();
};

#include "tp0tp.h"
#ifdef _WIN32
extern aio* create_win_aio(threadpool::threadpool *tp, size_t max_io_count);
#endif
#ifdef LINUX_NATIVE_AIO
extern aio* create_linux_aio(threadpool::threadpool* tp, size_t max_io_count);
#endif
extern aio* create_simulated_aio(threadpool::threadpool* tp, size_t read_threads, size_t write_threads);
