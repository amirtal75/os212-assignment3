#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl) {
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table at boot time.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->kstack = KSTACK((int) (p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void) {
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void) {
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid() {
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    free_data(p);
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    free_data(p);
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  #ifndef NONE
  // Pages support
  
  p->swapFile = 0;
  p->numOfPages = 0;
  p->pagesOnRAM = 0;
  p->indexSCFIFO = 0;

  if(p->pid > 2){
    if(init_metadata() < 0){
      panic("failed to create page metadata");
    }
    release(&p->lock);
    acquire(&p->lock);
  }
  #endif
  

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  
  #if defined(NFUA) || defined(LAPA) || defined(SCFIFO)
  p->swapFile = 0;
  p->numOfPages = 0;
  p->indexSCFIFO = 0;
  p->pagesOnRAM = 0;
  #endif
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process,
// with no user memory, but with trampoline pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe just below TRAMPOLINE, for trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// od -t xC initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy init's instructions
  // and data into it.
  uvminit(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    free_data(np);
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;
  #ifndef NONE
  // Pages Support
  if(p->pid > 2){
    np->numOfPages = p->numOfPages;
    np->pagesOnRAM = p->pagesOnRAM;
    np->indexSCFIFO = p->indexSCFIFO;
    copy_metadata(p,np);
    copy_file(p,np);
  }
  #endif
  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");
  #if defined(NFUA) || defined(LAPA) || defined(SCFIFO)
  //Pages support
  free_data(p);
  #endif
  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(np = proc; np < &proc[NPROC]; np++){
      if(np->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if(np->state == ZOMBIE){
          // Found one.
          pid = np->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          free_data(np);
          freeproc(np);
          release(&np->lock);
          release(&wait_lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || p->killed){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  
  c->proc = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        #if defined(NFUA) || defined(LAPA) || defined(SCFIFO)
        sfence_vma();
        update_pages();
        #endif
        c->proc = p;
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&p->lock);
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
// void
// procdump(void)
// {
//   static char *states[] = {
//   [UNUSED]    "unused",
//   [SLEEPING]  "sleep ",
//   [RUNNABLE]  "runble",
//   [RUNNING]   "run   ",
//   [ZOMBIE]    "zombie"
//   };
//   struct proc *p;
//   char *state;

//   printf("\n");
//   for(p = proc; p < &proc[NPROC]; p++){
//     if(p->state == UNUSED)
//       continue;
//     if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
//       state = states[p->state];
//     else
//       state = "???";
//     printf("%d %s %s %d", p->pid, state, p->name);
//     printf("\n");
//   }
// }


// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(int pid, int pid2)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  char* parent_buffer = kalloc();
  char* child_buffer = kalloc();
  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if (p->pid == pid)
    {
      if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
        state = states[p->state];
      else
        state = "???";
      printf("%d %s %s %d", p->pid, state, p->name);
      printf("\n");
        printf("numOfPages: %d, pagesOnRAM: %d, indexSCFIFO: %d\n", 
        p->numOfPages, p->pagesOnRAM, p->indexSCFIFO);

        printf("metadata RAM\n", 
        for (int i = 0; i < MAX_PAGES; i++)
        {
          printf("index[%d]: va: %x, offest: %d, age_counter: %d , scfifo_accessed: %d\n",
          i, p->ram_pages[i].va, p->ram_pages[i].offest, p->ram_pages[i].age_counter, 
          p->ram_pages[i].scfifo_accessed);
        }
        printf("metadata DISK\n", 
        for (int i = 0; i < MAX_PAGES; i++)
        {
          printf("index[%d]: va: %x, offest: %d, age_counter: %d , scfifo_accessed: %d\n",
          i, p->disk_pages[i].va, p->disk_pages[i].offest, p->disk_pages[i].age_counter, 
          p->disk_pages[i].scfifo_accessed);
        }
    }
    if (p->pid == pid2)
    {
      if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
        state = states[p->state];
      else
        state = "???";
      printf("%d %s %s %d", p->pid, state, p->name);
      printf("\n");
        printf("numOfPages: %d, pagesOnRAM: %d, indexSCFIFO: %d\n", 
        p->numOfPages, p->pagesOnRAM, p->indexSCFIFO);

        printf("metadata RAM\n", 
        for (int i = 0; i < MAX_PAGES; i++)
        {
          printf("index[%d]: va: %x, offest: %d, age_counter: %d , scfifo_accessed: %d\n",
          i, p->ram_pages[i].va, p->ram_pages[i].offest, p->ram_pages[i].age_counter, 
          p->ram_pages[i].scfifo_accessed);
        }
        printf("metadata DISK\n", 
        for (int i = 0; i < MAX_PAGES; i++)
        {
          printf("index[%d]: va: %x, offest: %d, age_counter: %d , scfifo_accessed: %d\n",
          i, p->disk_pages[i].va, p->disk_pages[i].offest, p->disk_pages[i].age_counter, 
          p->disk_pages[i].scfifo_accessed);
        }
    }
  }

  //compare swap files
  int swap_file_equality = 1;
  for (int i = 0; i < MAX_PAGES; i++)
  {
    if(readFromSwapFile(p, parent_buffer, i*PGSIZE, PGSIZE) == -1){
      panic("fail to read from the file");
    }
    if(readFromSwapFile(p, child_buffer, i*PGSIZE, PGSIZE) == -1){
      panic("fail to read from the file");
    }

    for (int j = 0; j < PGSIZE; j++)
    {
      if (child_buffer[j] != parent_buffer[j])
      {
        swap_file_equality = 0;
        break;
      }
    }
  }

  kfree(child_buffer);
  kfree(parent_buffer);

  if (swap_file_equality)
  {
    printf("swap files are equal\n");
  }
  else printf("swap files are not equal!!!!!!!!!!!!!!!!\n");
}

#ifndef NONE
void copy_metadata(struct proc *p,struct proc *np){
  if(p->pid >2){
    for (int i = 0; i < MAX_PAGES; i++)
    {
      np->disk_pages[i].va = p->disk_pages[i].va;    
      np->disk_pages[i].offset = p->disk_pages[i].offset;
      np->disk_pages[i].age_counter = p->disk_pages[i].age_counter;
      np->disk_pages[i].scfifo_accessed = p->disk_pages[i].scfifo_accessed;

      np->ram_pages[i].va = p->ram_pages[i].va;    
      np->ram_pages[i].offset = p->ram_pages[i].offset;
      np->ram_pages[i].age_counter = p->ram_pages[i].age_counter;
      np->ram_pages[i].scfifo_accessed = p->ram_pages[i].scfifo_accessed;
    }
  }
}

void copy_file(struct proc *p,struct proc *np){
  if(p->pid >2){
    if (!p->swapFile || !np->swapFile)
    {
      panic("fork:copy_file: swap file does not exist");
    }
    
    void *buffer = kalloc();
    for (int i = 0; i < MAX_PAGES; i++)
    {
      readFromSwapFile(p->swapFile, buffer, i*PGSIZE, PGSIZE);
      writeToSwapFile(np->swapFile, buffer, i*PGSIZE, PGSIZE);
    }
    kfree(buffer);
  }
}
// Pages support
int init_metadata(){
  struct proc *p = myproc();
  if (createSwapFile(p) < 0)
  {
    return -1;
  }
  
  for (int i = 0; i < MAX_PAGES; i++)
  {
    restart_page(p->ram_pages[i]);
    restart_page(p->disk_pages[i]);
  }
  char* buffer = (char*)kalloc();
  memmove(buffer,48,PGSIZE);
  for (int i = 0; i < MAX_PAGES; i++)
  {
    if (writeToSwapFile(p,buffer,i*PGSIZE,PGSIZE) < 0)
    {
      return -1;
    }
  }
  kfree(buffer);
  
  return 1;
}


void restart_page(struct metadata m){
  if(p->pid >2){
    m.offset = -1;
    m.va = 0;
    m.scfifo_accessed  = 0;
    #ifndef LAPA
    m.age_counter = 0;
    #else
    m.age_counter = 0xffffffff;
    #endif

  }
}

/*
  Search for a given page
*/
int find_existing_page(int isRam,uint64 va){
  struct proc *p = myproc();
  
  for(int i = 0; i < MAX_PAGES; i++){
    if (isRam)
    {
      if (p->ram_pages[i].va == va && 
        p->ram_pages[i].offset != -1)
      {
        return i;
      } 
    }
    else
    {
      if (p->disk_pages[i].va == va && 
        p->disk_pages[i].offset != -1)
      {
        return i;
      }  
    }   
  }
  return -1;
  //  panic("Failed to find the page/PTE in the pages array");
}

/*
  finds empty page
*/
int find_free_page(int isRam){
  struct proc *p = myproc();
  #ifdef SCFIFO
    if(isRam){
      return (p->indexSCFIFO -1) % 16 ;
    }
  #endif
  for(int i = 0; i < MAX_PAGES; i++){
    if (isRam)
    {
      if ( p->ram_pages[i].offset == -1)
      {
        restart_page(p->ram_pages[i]); //cleaning the page
        return i;
      }  
    }  
    else 
    {
      if ( p->disk_pages[i].offset == -1)
      {
        restart_page(p->disk_pages[i]); //cleaning the page
        return i;
      } 
    }    
  }
  panic("Failed to find a free page");
}

void free_data(struct proc * p){
  if(p->pid >2){
    if (removeSwapFile(p) < 0)
    {
      panic("failed to delete file");
    }

    for (int i = 0; i < MAX_PAGES; i++)
    {
      restart_page(p->ram_pages[i]);
      restart_page(p->disk_pages[i]);
    }  
  }
}
/*
  import a page from the swap file to the memory
  The function accepts a virtual address that represent 
  a position oa a "leaf" page to insert
*/
void
swapin(uint64 va)
{
  if(p->pid >2){
    struct proc *p = myproc();
    
    int disk_index = find_existing_page(0,va);
    if (disk_index == -1)
    {
      panic("Failed to find the page/PTE in the disk_pages array");
    }
    
    int unlocked = 0;

    // extract offset and re-init to -1
    int offset = disk_index*PGSIZE;
    restart_page(p->disk_pages[disk_index]);

    if (p->lock.locked)
    {
      unlocked = 1;
      release(&p->lock);
    }
    char* buffer = kalloc();
    if(readFromSwapFile(p, buffer, offset, PGSIZE) == -1){
      panic("fail to read from the file");
    }
    if (unlocked)
    {
      acquire(&p->lock);
    }

    if ((p->numOfPages - p->pagesOnRAM) == 16)
    {
      free_page(p);
    }

    int ram_index = find_free_page(1);
    
    va |= PTE_V;
    va &= ~PTE_PG;

    mappages(p->pagetable, va, PGSIZE, (uint64)buffer, PTE_W|PTE_R|PTE_X|PTE_U);
    
    p->ram_pages[ram_index].offset = ram_index;
    p->ram_pages[ram_index].va = va;

    #ifdef NFUA
      p->pagesOnRAM[index].counter = 1<<31;
    #endif

    p->pagesOnRAM++;
    sfence_vma();
  }
}

/*
  export a page from the memory to the swap file
  The function accepts a virtual address that represent 
  a position oa a "leaf" page to insert
*/
void 
swapout(uint64 va)
{
  if(p->pid >2){
    struct proc *p = myproc();
    int disk_index = find_free_page(0);
    int unlocked = 0;

    if (!(va & PTE_V) || (va & PTE_PG)) {
      panic("swapout: page isn't on ram");
    }

    if (p->lock.locked)
    {
      unlocked = 1;
      release(&p->lock);
    }
    uint64 pa = PTE2PA(va);
    if(writeToSwapFile(p, (char*)pa, disk_index*PGSIZE, PGSIZE) == -1){
        panic("swapout: fail to write to the file");
    }
    uvmunmap(p->pagetable, va, 1, 1);

    if (unlocked)
    {
      acquire(&p->lock);
    }

    int ram_index = find_existing_page(1,va);
    va &= ~PTE_V;
    va |= PTE_PG;

    p->disk_pages[disk_index].offset = disk_index;
    p->disk_pages[disk_index].va = va;

    restart_page(p->ram_pages[ram_index]);

    p->pagesOnRAM--;
    sfence_vma();
  }
}

void update_pages(){
  if(p->pid >2){
    struct proc *p = myproc();
    for (int i = 0; i < MAX_PAGES; i++)
    {
    #ifndef SCFIFO
      uint c = p->ram_pages[i].age_counter >> 1;
      c &= ~(1L << 32);
      if((p->ram_pages[i].va & PTE_A) == 1)
      {
        c |= (1L << 32);
      }
      p->ram_pages[i].age_counter = c;
      p->ram_pages[i].va &= ~PTE_A;
    }
    #else
      {
        if(p->ram_pages[i].va &= PTE_A){
          p->ram_pages[i].scfifo_accessed =1;
        }
      }  
    #endif
  }
}

void add_page(uint64 va){
  if(p->pid >2){ 
    struct proc *p = myproc();
    if (p->pid > 2)
    {
      p->numOfPages++;
      p->pagesOnRAM++;
      
      int index = find_free_page(1);
      p->ram_pages[index].va = va;
      p->ram_pages[index].offset = index;

      #ifdef NFUA
        p->ram_pages[index].age_counter = 0;
      #endif
      #ifdef LAPA
        p->ram_pages[index].age_counter = 0xffffffff;
      #endif
      #ifdef SCFIFO
         p->ram_pages[i].scfifo_accessed = 0;
      #endif
    }
  }
}

void remove_page(uint64 va){
  if(p->pid >2){
    struct proc *p = myproc();
      
    int ram_index = -1;
    int disk_index = find_existing_page(0,va);
    if (disk_index != -1)
    {
      ram_index = find_existing_page(1,va);
    }

    if (ram_index != -1)
    {
      p->pagesOnRAM--;
    }
    
    if (ram_index == -1 && disk_index == -1)
    {
      panic("Failed to find the page/PTE in the pages array");
    }

    if (ram_index > -1 && disk_index > -1)
    {
      panic("Found page/PTE in both pages array");
    }

    p->numOfPages--;

    if (ram_index != -1)
    {
      restart_page(p->ram_pages[ram_index]);
    }
    else
    {
      restart_page(p->disk_pages[disk_index]);
    } 
  }
}

// which page to be swapped out according to the chosen algorithm

int index_to_be_swaped()
{
  int out = -1;
  
  struct proc* p = myproc();

  #ifdef NFUA
    uint min, current = 0xFFFFFFFF;
    for(int i= 0; i < MAX_PAGES; i++){
      current = p->ram_pages[i].age_counter;
      if (current < min)
      {
        min = current;
        out = i;
      }
    }
  #endif
  #ifdef LAPA
  for (int i = 0; i < MAX_PAGES; i++)
    {
      uint current = 0;
      uint min = 33;
      for (int j = 0; i < 32; j++)
      {
        if (p->ram_pages[i].age_counter & (1L << j))
        {
          current++;
        }
      }
      if (current < min)
      {
        min = current;
        out = i;
      }
    }
  #endif
  #ifdef SCFIFO
    for (int i = 0; i < MAX_PAGES; i++){
      if(p->ram_pages[p->indexSCFIFO].scfifo_accessed == 0)
      {
        out = p->indexSCFIFO;
        p->indexSCFIFO = (p->indexSCFIFO +1) % 16;
        return  out;
      }
      else{
        p->ram_pages[p->indexSCFIFO].scfifo_accessed = 0;
        p->indexSCFIFO ++;
        p->indexSCFIFO = (p->indexSCFIFO +1 ) % 16;
      }
    }
  #endif
  return out;
}

void free_page(struct proc* p)
{
  if(p->pid >2){

    int lowest_age = index_to_be_swaped();
    if (lowest_age == -1)
    {
      panic("no pages on ram to move to file");
    }
    swapout(p->ram_pages[lowest_age]);
    vmunmap(p->pagetable, p->ram_pages[lowest_age].va, 1, 1);
  }
}
//$$$$ index scfifo when do we need to inc and dec && tests && gdb
#endif