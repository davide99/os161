#include <types.h>
#include <addrspace.h>
#include <thread.h>
#include <proc.h>
#include <current.h>
#include <syscall.h>
#include <synch.h>

#define OPT_WAITPID 1
#define USE_SEMAPHORE_FOR_WAITPID 1

void sys__exit(int status){
#if OPT_WAITPID
  struct proc *p = curproc;
  p->p_status = status & 0xff; /* just lower 8 bits returned */
  V(p->p_sem);
#else
  /* get address space of current process and destroy */
  struct addrspace *as = proc_getas();
  as_destroy(as);
#endif

  thread_exit();

  panic("thread_exit returned (should not happen)\n");
  (void) status; // TODO: status handling
}