#ifndef CUSTOM_SYSCALL_H
#define CUSTOM_SYSCALL_H

#include <poll.h>
#include <sys/syscall.h>

// Optimal poll syscall for Zen 4
static __attribute__((always_inline)) inline long my_poll(struct pollfd *fds, unsigned long nfds, int timeout) {
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

#endif // CUSTOM_SYSCALL_H