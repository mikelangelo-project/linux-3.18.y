#include <linux/syscalls.h>
#include <linux/kernel.h>

asmlinkage long sys_copy_to_user(void __user *to, void *addr, size_t len){
  if (len > PAGE_SIZE || len < 0){
    return -EINVAL;
  }

  if (copy_to_user(to, addr, len)) {
    return -EFAULT;
  }
  return 0;
}
