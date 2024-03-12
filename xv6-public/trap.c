#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"

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
  // lazy allocation means allocating physical memory in the case of a page fault (we are currently doing it in sysfile.c)
    uint faultyAddr = rcr2();
    int inMapping = 0;
    struct proc* p = myproc();
    mem_block* mapping;
    // find if page is in mapping
    for (int i = 0; i < MAX_WMMAP_INFO; i++){
      if (p->arr[i] != 0){ // is this faultyAddr a part of an existing mapping?
        if (faultyAddr >= p->arr[i]->start && faultyAddr <= p->arr[i]->end){
          inMapping = 1;
          mapping = p->arr[i];
          break;
        }
      }
    }
    if (inMapping){
      uint startAddr = mapping->start;
      int pagesNeeded = PGROUNDUP(mapping->length) / PGSIZE;

       // mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm)
      // this is the logic for allocating physical memory for our virtual
      for (int i = 0; i < pagesNeeded; i++){
        char* mem;
        mem = kalloc(); // kalloc to give us va that maps to pa
        if (mem == 0){
          panic("kalloc");
        }
        memset(mem, 0, PGSIZE);
        cprintf("calling mappages with pgdir = %p, va: %x, size: %d, pa: %x perm: %d\n", p->pgdir, startAddr, PGSIZE, V2P(mem), PTE_W | PTE_U);
        if (mappages(p->pgdir, (void*) startAddr, PGSIZE, V2P(mem), PTE_W | PTE_U) != 0){
          cprintf("mappages failed\n");
          kfree(mem);
        }
        startAddr = startAddr + PGSIZE; // increment va to map to physical
        cprintf("startAddr: %x\n", startAddr);
      }


      // handle page fault - map virtual to physical memory, do we write to file in page fault case?
      // does this mean we copy over va->phys logic here (code that loops and users kalloc and mappages)
      cprintf("handle page fault at addr: %x\n", faultyAddr);
      // behavior depends on flags? MAP_ANONYMOUS case means ignoring the file write
      // if map_shared then we want to write contents of memory to a file.
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
