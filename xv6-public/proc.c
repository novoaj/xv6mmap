#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"
#include "wmap.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;
  
  // p->numMappings = 0; // init field in proc
  struct wmapinfo* wmap = (struct wmapinfo*)kalloc();
  if (wmap == 0){
    panic("kalloc()");
  }
  wmap->total_mmaps = 0;
  // init wmap info arrays to hold 0 values at each idx so no garbage values
  // TODO: also want to initialize mem_block arrays to hold pointers - maybe we have valid bit in mem_block struct. 
    // instead of checking whether its null and kallocing in sysfile insert method, we can check valid bit.
    // running into issues with ref currrently because it holds a garbage value so maybe best for us to init here with 0s or something
  for (int i = 0; i < MAX_WMMAP_INFO; i++){
    wmap->addr[i] = 0;
    wmap->length[i] = 0;
    wmap->n_loaded_pages[i] = 0;
    wmap->leftmostLoadedAddr[i] = MAX_ADDR + 1;
  }
  p->wmapinfo = wmap; // init wmap pointer in process to point to this struct
  // allocproc is called in fork method, rn reinitializing everything, ultimately that will depend on flags
  // init arr to all null pointers
  for (int i = 0; i < MAX_WMMAP_INFO; i++){
    mem_block* new_mapping = (mem_block*)kalloc();
    if (new_mapping == 0){
      panic("kalloc()");
      // return FAILED;
    }
    new_mapping->end = 0;
    new_mapping->fd = 0;
    new_mapping->flags = 0;
    new_mapping->length = 0;
    new_mapping->ref = 0;
    new_mapping->start = 0;
    new_mapping->valid = 0;
    // INIT fields for new_block to default vals

    p->arr[i] = new_mapping;
  }
  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Helper functions for mapping in fork
extern char data[];
extern pde_t *kpgdir; 

void increment_mapping_ref_count(struct mem_block *mapping) {
    mapping->ref++;
}

void duplicate_private_mapping(struct proc *child, struct mem_block *mapping) {
    uint va, pa;
    pte_t *pte;
    char *mem;

    for (va = mapping->start; va < mapping->end; va += PGSIZE) {
        // Walk the parent's page directory to find the PTE for the current VA
        
        pte = walkpgdir(child->parent->pgdir, (void *)va, 0);
        cprintf("child page directory: %p\n", (void*)child->pgdir);
        cprintf("child->parent page directory: %p\n", (void*)child->parent->pgdir);
        cprintf("pte in duplicate_private_mapping: %p\n", (void*)pte);

        //     panic("duplicate private mapping: parent page not present");
        // }

        // Allocate a new physical page for the child
        // TODO I think the addressing is okay but maybe a problem with the copying of page tabels
        // Could also be incorectly error checking on my part
        mem = kalloc();
        // if (!mem) {
        //     panic("duplicate private mapping: kalloc failed");
        // }

        // Copy the content from the parent's page to the newly allocated page
        pa = PTE_ADDR(*pte);
        memmove(mem, (char*)P2V(pa), PGSIZE);
        //cprintf("mem value: %d\n", mem);
        //cprintf("pte in duplicate_private_mapping after memmove: %d\n", pte);

        // Map the new page into the child's page table
        mappages(child->pgdir, (void*)va, PGSIZE, V2P(mem), PTE_FLAGS(*pte));
    }
}


void add_shared_mapping(struct proc *child, struct mem_block *parent_mapping) {
  uint va;
  pte_t *pte_parent;
  //char *mem;

    // Iterate through each page in the mapping
  for (va = parent_mapping->start; va < parent_mapping->end; va += PGSIZE) {
    // Find the parent's page table entry for this virtual address
    pte_parent = walkpgdir(child->parent->pgdir, (void *)va, 0);
    
    // Ensure the page is present in the parent's page table
    // if (!pte_parent || !(*pte_parent & PTE_P)) {
    //     panic("add_shared_mapping: parent page not present");
    // }

    
    // No need to allocate a new physical page for the child, use the parent's
    uint pa = PTE_ADDR(*pte_parent);
    uint flags = PTE_FLAGS(*pte_parent);

    // Map the physical page into the child's page table with the same permissions
    mappages(child->pgdir, (void *)va, PGSIZE, pa, flags);

  }
}


// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{ 
  cprintf("enter fork...\n\n");
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // TODO: depending on curproc's mmap and the flags associated w each mapping, copy or don't copy over memmappings to np (new process)

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  // Allows inheritance of maps depending on MAP_PRIVATE or MAP_SHARED flags
  cprintf("Parent PID: %d\n", curproc->pid);
  cprintf("Child PID: %d\n", np->pid);
  for (int i = 0; i < MAX_WMMAP_INFO; i++) {
    if (curproc->arr[i]->valid == 0){
      continue;
    }
    struct mem_block *cur_mapping = curproc->arr[i];
      
    if (cur_mapping->flags & MAP_PRIVATE) {
      // copy the ith mapping from parent to child
        cprintf("copying mapping from parent: %d - %p, to child: %d - %p\n", i, curproc->arr[i], i, np->arr[i]);
        cprintf("new process refs: %d parent process ref: %d\n",np->arr[i]->ref, curproc->arr[i]->ref + 1);
       // np->arr[i] = curproc->arr[i];
        np->arr[i]->end = curproc->arr[i]->end;
        np->arr[i]->fd = curproc->arr[i]->fd;
        np->arr[i]->flags = curproc->arr[i]->flags;
        np->arr[i]->length = curproc->arr[i]->length;
        np->arr[i]->ref = curproc->arr[i]->ref;
        np->arr[i]->start = curproc->arr[i]->start;
        np->arr[i]->valid = curproc->arr[i]->valid;

        np->wmapinfo->addr[i] = curproc->wmapinfo->addr[i];
        np->wmapinfo->leftmostLoadedAddr[i] = curproc->wmapinfo->leftmostLoadedAddr[i];
        np->wmapinfo->length[i] = curproc->wmapinfo->length[i];
        np->wmapinfo->n_loaded_pages[i] = curproc->wmapinfo->n_loaded_pages[i];
        np->wmapinfo->total_mmaps += 1;
      for (uint va = cur_mapping->start; va < cur_mapping->end; va += PGSIZE) {
        // Walk the parent's page directory to find the PTE for the current VA
        char *mem = kalloc();
        if (mem == 0){
          panic("kalloc\n");
        }
        pte_t *pte = walkpgdir(curproc->pgdir, (void *)va, 0);
        // cprintf("child page directory: %p\n", (void*)np->pgdir);
        // cprintf("child->parent page directory: %p\n", (void*)np->parent->pgdir);
        // cprintf("pte in duplicate_private_mapping: %p\n", (void*)pte);
        cprintf("memmove in MAP_PRIVATE case\n");
        memmove(mem, P2V(PTE_ADDR(*pte)), PGSIZE);

        if(mappages(np->pgdir, (void*)va, PGSIZE, (uint) V2P(mem), PTE_W | PTE_U) != 0){
          cprintf("kfree in fork for MAP_PRIVATE\n\n");
          kfree(mem);
          // np->killed=1;
          cprintf("fork complete\n\n");
        }
      }
    } else if (cur_mapping->flags & MAP_SHARED) {
      cprintf("map shared case in fork\n\n");
      // np->arr[i] = curproc->arr[i];
      // np->arr[i]->ref++;
      // np->wmapinfo = curproc->wmapinfo;
      np->arr[i]->end = curproc->arr[i]->end;
      np->arr[i]->fd = curproc->arr[i]->fd;
      np->arr[i]->flags = curproc->arr[i]->flags;
      np->arr[i]->length = curproc->arr[i]->length;
      np->arr[i]->ref = curproc->arr[i]->ref;
      np->arr[i]->start = curproc->arr[i]->start;
      np->arr[i]->valid = curproc->arr[i]->valid;

      np->wmapinfo->addr[i] = curproc->wmapinfo->addr[i];
      np->wmapinfo->leftmostLoadedAddr[i] = curproc->wmapinfo->leftmostLoadedAddr[i];
      np->wmapinfo->length[i] = curproc->wmapinfo->length[i];
      np->wmapinfo->n_loaded_pages[i] = curproc->wmapinfo->n_loaded_pages[i];
      np->wmapinfo->total_mmaps += 1;
      cprintf("entering for loop\n");
      // is forked so that exit can properly close mappings
      for (uint va = cur_mapping->start; va < cur_mapping->end; va += PGSIZE) {
        // Find the parent's page table entry for this virtual address
        // how can we find physical address from virtual?
        // cprintf("walking pgdir\n\n");
        pte_t *pte_parent = walkpgdir(curproc->pgdir, (void *)va, 0);
        // cprintf("after walk pgdir\n");
        uint pa = (uint) (PTE_ADDR(*pte_parent));
        // uint flags = PTE_FLAGS(*pte_parent);
        // Map the physical page into the child's page table with the same permissions
        if (mappages(np->pgdir, (void *)va, PGSIZE, (pa), PTE_W | PTE_U) != 0){
          cprintf("mappages failed in fork\n\n");
          kfree((char*) pa);
          // np->killed = 1;
        }
      }
    }
  }
  for (int i = 0; i < MAX_WMMAP_INFO; i++){
    cprintf("curproc: \n");
    cprintf("valid: %d, pointer: %p, startaddr: %x, refcount: %d, length %d\n",curproc->arr[i]->valid, curproc->arr[i], curproc->arr[i]->start, curproc->arr[i]->ref, curproc->arr[i]->length);
  }
  for (int i = 0; i < MAX_WMMAP_INFO; i++){
    cprintf("np: \n");
    cprintf("valid: %d, pointer: %p, startaddr: %x, refcount: %d length: %d\n",np->arr[i]->valid, np->arr[i], np->arr[i]->start, np->arr[i]->ref, curproc->arr[i]->length);
  }

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  cprintf("fork returns %d\n", pid);
  return pid;
}

// Helper method for exit 
void cleanup_wmapinfo(struct proc *p) {
  for (int i = 0; i < p->wmapinfo->total_mmaps; i++) {
    struct mem_block *mapping = p->arr[i];
    if (!mapping) {
      continue;
    }

    if (mapping->flags & MAP_SHARED) {
      // TODO free the shared mapping, will be tricky to free shared allocations
      // Tried and failed at solving this for a while
      // This helper method could probabaly use more helper methods itsefl
      // This should mean that this function is called in exit to clean up 
      cleanup_shared_mapping(p, mapping);
    }
    else if (mapping->flags & MAP_PRIVATE) {
      // TODO free the individual mapping, realatively simple to free this mapping
      free_physical_pages(p, mapping);
    }

    kfree((char*)mapping);
    p->arr[i] = 0;
  }
  p->wmapinfo->total_mmaps = 0;
}
// Helper method for helper lol
void free_physical_pages(struct proc *p, struct mem_block *mapping) {
  for (uint va = mapping->start; va < mapping->end; va += PGSIZE) {
    pte_t *pte = walkpgdir(p->pgdir, (void *)va, 0);
    if (!pte || !(*pte & PTE_P)) {
      // No mapping present
      continue;
    }
    uint pa = PTE_ADDR(*pte);
    // Physical address to kernel va
    char* v = P2V(pa);
    // free the physcial pages
    kfree(v);

    // Clear page table entry
    *pte = 0;

  }
}

// Another helper for a helper
void cleanup_shared_mapping(struct proc *p, struct mem_block *mapping) {
    // Decrement reference count
    mapping->ref--;
    // Only proceed to free memory if reference count hits zero
    if (mapping->ref == 0) {
        free_physical_pages(p, mapping);
    }
}


// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  cprintf("\n\nexit enters...\n\n");
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;


  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // free pointers in proc struct, should changes in child be visible in parent? copy over info?
  kfree((char*)curproc->wmapinfo);
  for (int i = 0; i < MAX_WMMAP_INFO; i++){
    if (curproc->arr[i]->valid == 1 && curproc->arr[i]->ref == 1){ // free if this is the only ref
      kfree((char*)curproc->arr[i]);
    }
    curproc->parent->arr[i]->ref -= 1;
  }
  
  cprintf("deallocing uvm...\n\n");
  deallocuvm(myproc()->pgdir, myproc()->sz, 0);
  cprintf("deallocuvm done\n");
  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&ptable.lock);

  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}
