#include <types.h>
#include <addrspace.h>
#include <thread.h>
#include <proc.h>
#include <current.h>
#include <syscall.h>

void sys__exit(int code){
    (void)code;

    struct addrspace *as = proc_getas();
    as_destroy(as);
    thread_exit();

    panic("sys__exit: We should not get here");
}