#include <aio0aio.h>

void aio::add_pending_counter(aio_opcode opcode, int count)
{
 if (opcode == AIO_PREAD)
   m_pending_reads += count;
 else
   m_pending_writes += count;
}

void aio::execute_callback(const native_file_handle& fh, aio_opcode opcode, unsigned long long offset, void* buffer, unsigned int len,
  int ret_len, int err, void* userdata)
{
  m_callback(fh, opcode, offset, buffer, len, ret_len, err, userdata);
  add_pending_counter(opcode, -1);
}

int aio::submit_io(const native_file_handle& fd, aio_opcode opcode, unsigned long long offset, void* buffer, unsigned int len, void* userdata, size_t userdata_len)
{
  add_pending_counter(opcode, 1);
  int ret = submit(fd, opcode, offset, buffer, len, userdata, userdata_len);
  if (ret)
    add_pending_counter(opcode, -1);
  return ret;
}

static void microsleep()
{
#ifdef _WIN32
  Sleep(1);
#else
  usleep(100000);
#endif
}
void aio::wait_for_pending_writes()
{
  while (m_pending_writes)
   microsleep();
}

aio::~aio()
{
  while (m_pending_reads || m_pending_writes)
    microsleep();
}