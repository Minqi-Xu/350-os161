#include <types.h>
#include <kern/errno.h>
#include <kern/unistd.h>
#include <kern/wait.h>
#include <kern/fcntl.h>
#include <lib.h>
#include <syscall.h>
#include <current.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <copyinout.h>
#include <clock.h>
#include <mips/trapframe.h>
#include "opt-A1.h"
#include "opt-A3.h"
#include <vfs.h>
#include <vm.h>

  /* this implementation of sys__exit does not do anything with the exit code */
  /* this needs to be fixed to get exit() and waitpid() working properly */

void sys__exit(int exitcode) {

  struct addrspace *as;
  struct proc *p = curproc;
  /* for now, just include this to keep the compiler from complaining about
     an unused variable */
  (void)exitcode;

  DEBUG(DB_SYSCALL,"Syscall: _exit(%d)\n",exitcode);

  KASSERT(curproc->p_addrspace != NULL);
  as_deactivate();
  /*
   * clear p_addrspace before calling as_destroy. Otherwise if
   * as_destroy sleeps (which is quite possible) when we
   * come back we'll be calling as_activate on a
   * half-destroyed address space. This tends to be
   * messily fatal.
   */
  as = curproc_setas(NULL);
  as_destroy(as);

#if OPT_A1
  for(int i = p->p_children->num - 1; i >= 0; i--) {
    struct proc* temp_child = kmalloc(sizeof(struct proc));
    memcpy(temp_child, array_get(p->p_children, i), sizeof(struct proc));
    array_remove(p->p_children, i);
    spinlock_acquire(&temp_child->p_lock);
    if(!temp_child->p_exitstatus) {
        spinlock_release(&temp_child->p_lock);
        proc_destroy(temp_child);
    } else {
        temp_child->p_parent = NULL;
        spinlock_release(&temp_child->p_lock);
    }
  }

#endif // OPT_A1

  /* detach this thread from its process */
  /* note: curproc cannot be used after this call */
  proc_remthread(curthread);


#if OPT_A1
    spinlock_acquire(&p->p_lock);
    if(p->p_parent == NULL || p->p_parent->p_exitstatus == 0) {
        spinlock_release(&p->p_lock);
        proc_destroy(p);
    } else {
        p->p_exitstatus = 0;
        p->p_exitcode += exitcode;
        spinlock_release(&p->p_lock);
    }

#else
  /* if this is the last user process in the system, proc_destroy()
     will wake up the kernel menu thread */
  proc_destroy(p);
#endif // OPT_A1



  thread_exit();
  /* thread_exit() does not return, so we should never get here */
  panic("return from thread_exit in sys_exit\n");
}

#if OPT_A1
int sys_getpid(pid_t *retval)
{
  *retval = curproc->p_pid;
  return(0);
}
#else

/* stub handler for getpid() system call                */
int
sys_getpid(pid_t *retval)
{
  /* for now, this is just a stub that always returns a PID of 1 */
  /* you need to fix this to make it work properly */
  *retval = 1;
  return(0);
}
#endif // OPT_A1


#if OPT_A1
int sys_fork(int *retval,struct trapframe *tf) {

    struct proc* new_proc = proc_create_runprogram("child");
    new_proc->p_parent = curproc;

    struct addrspace * current_as = curproc_getas();
    as_copy(current_as, &(new_proc->p_addrspace));

    struct trapframe *trapframe_for_child = kmalloc(sizeof(struct trapframe));
    if(trapframe_for_child == NULL) {
        return ENOMEM;
    }

    unsigned ret_index;
    array_add(curproc->p_children, new_proc, &ret_index);

    *trapframe_for_child = *tf;

    thread_fork("child_thread", new_proc, &enter_forked_process, trapframe_for_child, 0);

    *retval = new_proc->p_pid;

    clocksleep(1);
    return 0;
}
#endif  // OPT_A1


#if OPT_A3
static vaddr_t* args_alloc() {
    vaddr_t* argv = kmalloc(17 * sizeof(vaddr_t));
    for(int i = 0; i < 16; i++) {
        argv[i] = (vaddr_t)kmalloc(128);
    }
    argv[16] = 0;
    return argv;
}

static void args_free(vaddr_t* argv) {
    for(int i = 0; i < 16; i++)
        kfree((void *)argv[i]);
    kfree(argv);

}

static int argcopy_in(vaddr_t* argv, char** args) {
    int argc;
    size_t got;
    for(argc = 0; ((char**)args)[argc] != NULL; argc++) {}
    argv[argc] = (vaddr_t)NULL;
    for(int i = 0; i < argc; i++) {
        copyinstr((userptr_t) args[i], (char*)argv[i], strlen(args[i]), &got);
    }
    return argc;

}

int sys_execv(char *progname, char **argv) {
    struct addrspace *as;
    struct addrspace *old;
	struct vnode *v;
	vaddr_t entrypoint, stackptr;
	int result;

	/* Open the file. */
	result = vfs_open(progname, O_RDONLY, 0, &v);
	if (result) {
		return result;
	}

	/* We should be a new process. */
	// KASSERT(curproc_getas() == NULL);

	/* Create a new address space. */
	as = as_create();
	if (as ==NULL) {
		vfs_close(v);
		return ENOMEM;
	}

	/* Switch to it and activate it. */
	old = curproc_setas(as);
	as_activate();
	as_destroy(old);

	/* Load the executable. */
	result = load_elf(v, &entrypoint);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		vfs_close(v);
		return result;
	}

	/* Done with the file now. */
	vfs_close(v);

	/* Define the user stack in the address space */
	result = as_define_stack(as, &stackptr);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		return result;
	}
    vaddr_t* args = args_alloc();
	int argcc = argcopy_in(args, argv);
	enter_new_process(argcc, (userptr_t)args, stackptr, entrypoint);


	/* Warp to user mode. */


	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	args_free(args);
	return EINVAL;

}
#endif // OPT_A3


/* stub handler for waitpid() system call                */

int
sys_waitpid(pid_t pid,
	    userptr_t status,
	    int options,
	    pid_t *retval)
{
#if OPT_A1
  bool isFind = false;
  int result;
  int exitstatus;
  struct proc *temp_child/* = kmalloc(sizeof(struct proc))*/;

  for (unsigned i = 0; i < curproc->p_children->num; i++) {

//        memcpy(temp_child, array_get(curproc->p_children, i), sizeof(struct proc));
        temp_child = (struct proc*)array_get(curproc->p_children, i);
        if(temp_child->p_pid == pid) {
            isFind = true;
            array_remove(curproc->p_children, i);
            break;
        }
  }
  if(isFind == false){
        return EINVAL;
  }
  spinlock_acquire(&temp_child->p_lock);
  while(temp_child->p_exitstatus) {
        spinlock_release(&temp_child->p_lock);
        clocksleep(1);
        spinlock_acquire(&temp_child->p_lock);
  }
  spinlock_release(&temp_child->p_lock);
  if (options != 0) {
    return(EINVAL);
  }
  exitstatus = _MKWAIT_EXIT(temp_child->p_exitcode);
  result = copyout((void *)&exitstatus, status, sizeof(int));
  proc_destroy(temp_child);
  if(result) {
        return result;
  }
  *retval = pid;
  return 0;


#else
  int exitstatus;
  int result;

  /* this is just a stub implementation that always reports an
     exit status of 0, regardless of the actual exit status of
     the specified process.
     In fact, this will return 0 even if the specified process
     is still running, and even if it never existed in the first place.

     Fix this!
  */

  if (options != 0) {
    return(EINVAL);
  }
  /* for now, just pretend the exitstatus is 0 */
  exitstatus = 0;
  result = copyout((void *)&exitstatus,status,sizeof(int));
  if (result) {
    return(result);
  }
  *retval = pid;
  return(0);
#endif // OPT_A1

}

