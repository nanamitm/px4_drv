"""Compile the actual read function with injected copy_to_user faults (no hardware)."""
import os
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "ringbuffer.c").read_text(encoding="utf-8")
function = source[source.index("int ringbuffer_read_user("):source.index("int ringbuffer_write_atomic(")]
prefix = r"""
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef unsigned char u8;
#define __user
#define likely(x) (x)
#define unlikely(x) (x)
#define atomic_add_return_acquire(n,p) (*(p) += (n))
#define atomic_read_acquire(p) (*(p))
#define atomic_read(p) (*(p))
#define atomic_xchg(p,n) (*(p) = (n))
#define atomic_sub_return_release(n,p) (*(p) -= (n))
#define atomic_sub_return(n,p) (*(p) -= (n))
#define wake_up(p) ((void)(p))
struct ringbuffer { u8 *buf; size_t size; int actual_size, head, rw_count, wait_count, wait; };
static size_t limits[2];
static unsigned int calls;
static unsigned long copy_to_user(void *to, const void *from, size_t size)
{
 size_t copied = size < limits[calls] ? size : limits[calls];
 ++calls;
 memcpy(to, from, copied);
 return size - copied;
}
"""
suffix = r"""
int main(void)
{
 unsigned int cases = 0;
 for (int head=0; head<16; ++head)
 for (int actual=0; actual<=16; ++actual)
 for (int request=0; request<=24; ++request)
 for (size_t first=0; first<=16; ++first)
 for (size_t second=0; second<=16; ++second) {
  u8 data[16], output[24];
  for (int i=0; i<16; ++i) data[i] = (u8)i;
  memset(output, 255, sizeof(output));
  struct ringbuffer rb = {data, 16, actual, head, 0, 0, 0};
  limits[0]=first; limits[1]=second; calls=0;
  size_t size=(size_t)request;
  int ret=ringbuffer_read_user(&rb, output, &size);
  size_t wanted=request < actual ? (size_t)request : (size_t)actual;
  size_t front=wanted < (size_t)(16-head) ? wanted : (size_t)(16-head);
  size_t copied=front < first ? front : first;
  if (copied==front && wanted>front)
   copied += wanted-front < second ? wanted-front : second;
  if (size!=copied || rb.head!=(head+(int)copied)%16 || rb.actual_size!=actual-(int)copied ||
      ret!=(copied<wanted ? -EFAULT : 0) || rb.rw_count!=0) return 1;
  for (size_t i=0; i<copied; ++i) if (output[i]!=data[(head+i)%16]) return 2;
  for (size_t i=copied; i<sizeof(output); ++i) if (output[i]!=255) return 3;
  ++cases;
 }
 printf("ringbuffer_read_test: passed (%u partial-copy cases)\n", cases);
}
"""
with tempfile.TemporaryDirectory(prefix="px4-ringbuffer-") as temp:
    test = Path(temp) / "read_test.c"
    binary = Path(temp) / "read_test"
    test.write_text(prefix + function + suffix, encoding="utf-8")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(test), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=30)
