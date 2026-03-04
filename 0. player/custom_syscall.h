#ifndef CUSTOM_SYSCALL_H
#define CUSTOM_SYSCALL_H

#include <poll.h>
#include <sys/syscall.h>

// Optimal poll syscall for Zen 4
static __attribute__((always_inline)) inline long my_poll_x86(struct pollfd *fds, unsigned long nfds, int timeout) {
    register  long rax __asm__("rax") = (long)__NR_poll;
    register  long rdi __asm__("rdi") = (long)fds;
    register  long rsi __asm__("rsi") = nfds;
    register  long rdx __asm__("rdx") = (long)timeout;
    register long ret __asm__("rax");  // No const - this is output
    
    __asm__ __volatile__ (
        "syscall"
        : "=r"(ret)
        : "r"(rax), "r"(rdi), "r"(rsi), "r"(rdx)
        : "rcx", "r11", "memory"
    );
    
    return ret;
}

// Direct ioctl - no errno, no branch, no TLS write
static __attribute__((always_inline)) inline void my_ioctl_x86(int fd, unsigned long request, void *arg) {
    register long rax __asm__("rax") = (long)__NR_ioctl;
    register long rdi __asm__("rdi") = (long)fd;
    register long rsi __asm__("rsi") = request;
    register long rdx __asm__("rdx") = (long)arg;
    long dummy_ret;

    __asm__ __volatile__ (
        "syscall"
        : "=a"(dummy_ret)
        : "r"(rax), "r"(rdi), "r"(rsi), "r"(rdx)
        : "rcx", "r11", "memory"
    );
}

#endif // CUSTOM_SYSCALL_H
