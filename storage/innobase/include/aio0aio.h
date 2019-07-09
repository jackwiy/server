#pragma once
#ifdef _WIN32
#include <windows.h>
typedef HANDLE native_file_handle;
#else
typedef int native_file_handle;
#endif

enum aio_opcode
{
  AIO_PREAD,
  AIO_PWRITE
};

typedef void (* aio_callback_func)
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

class aio
{
public:
  aio_callback_func m_callback;
  void set_callback(aio_callback_func callback) { m_callback = callback;}
  virtual int bind(native_file_handle fd) = 0;
  virtual int submit(native_file_handle fd, aio_opcode opcode, unsigned long long offset, void *buffer, unsigned int len, void *userdata) = 0;
  virtual ~aio(){}
};

#include "tp0tp.h"
#ifdef _WIN32
extern aio* create_win_aio(threadpool::threadpool *tp);
extern aio* create_simulated_aio(threadpool::threadpool* tp);
#endif