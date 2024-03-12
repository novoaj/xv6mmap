#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"
#include "wmap.h"

// Interrupt descriptor table (shared by all CPUs).
struct gatedesc idt[256];
extern uint vectors[];  // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;

void
tvinit(void)
{
  int i;

  for(i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE<<3, vectors[i], 0);
  SETGATE(idt[T_SYSCALL], 1, SEG_KCODE<<3, vectors[T_SYSCALL], DPL_USER);

  initlock(&tickslock, "time");
}

void
idtinit(void)
{
  lidt(idt, sizeof(idt));
}

//PAGEBREAK: 41
void
trap(struct trapframe *tf)
{
  if(tf->trapno == T_SYSCALL){
    if(myproc()->killed)
      exit();
    myproc()->tf = tf;
    syscall();
    if(myproc()->killed)
      exit();
    return;
  }

  switch(tf->trapno){
  case T_IRQ0 + IRQ_TIMER:
    if(cpuid() == 0){
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
    }
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE+1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case T_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case T_IRQ0 + 7:
  case T_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n",
            cpuid(), tf->cs, tf->eip);
    lapiceoi();
    break;
  // Add T_PGFLT
  case T_PGFLT: // T_PGFLT = 14
  // "In lazy allocation, you should only map the page that's currently being accessed."
  //    if page fault addr is part of a mapping: // lazy allocation
    uint faultyAddr = rcr2();
    int inMapping = 0;
    struct proc* p = myproc();
    mem_block* mapping;
    int location;
    // find if page is in mapping
    for (int i = 0; i < MAX_WMMAP_INFO; i++){
      if (p->arr[i] != 0){ // is this faultyAddr a part of an existing mapping?
        if (faultyAddr >= p->arr[i]->start && faultyAddr <= p->arr[i]->end){
          inMapping = 1;
          mapping = p->arr[i];
          location = i;
          break;
        }
      }
    }
    if (inMapping){
      // cprintf("PGFAULT\n");
      // lazy allocation means one page at a time
      char* mem;
      mem = kalloc(); // kalloc to give us va that maps to pa
      if (mem == 0){
        panic("kalloc");
      }
      memset(mem, 0, PGSIZE); // maybe only map 'length' amount of space? with maximum of a page (4096)
        // cprintf("calling mappages with pgdir = %p, va: %x, size: %d, pa: %x perm: %d\n", p->pgdir, startAddr, PGSIZE, V2P(mem), PTE_W | PTE_U);
      if (mappages(p->pgdir, (void*) faultyAddr, PGSIZE, V2P(mem), PTE_W | PTE_U) != 0){
        cprintf("mappages failed\n");          
        kfree(mem);
      }
        // write to memory if not map anon
      if ((mapping->flags & MAP_ANONYMOUS) == 0){
        struct file *f = p->ofile[mapping->fd];
        fileread(f, (char*) faultyAddr, PGSIZE);
      }
      // here we consider the page to be "loaded"
      p->wmapinfo->n_loaded_pages[location] += 1; 
      // p->wmapinfo->addr[p->wmapinfo->total_mmaps] = faultyAddr;
      // p->wmapinfo->length[p->wmapinfo->total_mmaps] = PGSIZE;
      // p->wmapinfo->total_mmaps += 1;

      // only add page that causes page fault, however, mapping->start that page is a mapping of should be wmapinfo[start]
      break;
    }else{
      cprintf("Segmentation Fault\n");
      break;
    }


  //PAGEBREAK: 13
  default:
    if(myproc() == 0 || (tf->cs&3) == 0){
      // In kernel, it must be our mistake.
      cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n",
              tf->trapno, cpuid(), tf->eip, rcr2());
      panic("trap");
    }
    // In user space, assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "eip 0x%x addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno,
            tf->err, cpuid(), tf->eip, rcr2());
    myproc()->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running
  // until it gets to the regular system call return.)
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if(myproc() && myproc()->state == RUNNING &&
     tf->trapno == T_IRQ0+IRQ_TIMER)
    yield();

  // Check if the process has been killed since we yielded
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();
}
