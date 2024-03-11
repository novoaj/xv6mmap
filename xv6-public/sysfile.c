//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "mmu.h"
#include "proc.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

#include "wmap.h"
#include "memlayout.h"
// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  if(argint(n, &fd) < 0)
    return -1;
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *curproc = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd] == 0){
      curproc->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

int
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

int
sys_read(void)
{
  struct file *f;
  int n;
  char *p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argptr(1, &p, n) < 0)
    return -1;
  return fileread(f, p, n);
}

int
sys_write(void)
{
  struct file *f;
  int n;
  char *p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argptr(1, &p, n) < 0)
    return -1;
  return filewrite(f, p, n);
}

int
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

int
sys_fstat(void)
{
  struct file *f;
  struct stat *st;

  if(argfd(0, 0, &f) < 0 || argptr(1, (void*)&st, sizeof(*st)) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
int
sys_link(void)
{
  char name[DIRSIZ], *new, *old;
  struct inode *dp, *ip;

  if(argstr(0, &old) < 0 || argstr(1, &new) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

//PAGEBREAK!
int
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], *path;
  uint off;

  if(argstr(0, &path) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;
  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && ip->type == T_FILE)
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    dp->nlink++;  // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }

  if(dirlink(dp, name, ip->inum) < 0)
    panic("create: dirlink");

  iunlockput(dp);

  return ip;
}

int
sys_open(void)
{
  char *path;
  int fd, omode;
  struct file *f;
  struct inode *ip;

  if(argstr(0, &path) < 0 || argint(1, &omode) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  end_op();

  f->type = FD_INODE;
  f->ip = ip;
  f->off = 0;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
  return fd;
}

int
sys_mkdir(void)
{
  char *path;
  struct inode *ip;

  begin_op();
  if(argstr(0, &path) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

int
sys_mknod(void)
{
  struct inode *ip;
  char *path;
  int major, minor;

  begin_op();
  if((argstr(0, &path)) < 0 ||
     argint(1, &major) < 0 ||
     argint(2, &minor) < 0 ||
     (ip = create(path, T_DEV, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

int
sys_chdir(void)
{
  char *path;
  struct inode *ip;
  struct proc *curproc = myproc();
  
  begin_op();
  if(argstr(0, &path) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(curproc->cwd);
  end_op();
  curproc->cwd = ip;
  return 0;
}

int
sys_exec(void)
{
  char *path, *argv[MAXARG];
  int i;
  uint uargv, uarg;

  if(argstr(0, &path) < 0 || argint(1, (int*)&uargv) < 0){
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv))
      return -1;
    if(fetchint(uargv+4*i, (int*)&uarg) < 0)
      return -1;
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    if(fetchstr(uarg, &argv[i]) < 0)
      return -1;
  }
  return exec(path, argv);
}

int
sys_pipe(void)
{
  int *fd;
  struct file *rf, *wf;
  int fd0, fd1;

  if(argptr(0, (void*)&fd, 2*sizeof(fd[0])) < 0)
    return -1;
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      myproc()->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  fd[0] = fd0;
  fd[1] = fd1;
  return 0;
}
/*
 * memory array functions
 */
// insertion sort? sort by start addr, would help us when inserting new addrs
// https://www.geeksforgeeks.org/insertion-sort/
// void sort_mem_blocks(struct proc *p) {
//   const int ARR_SIZE = 16;

//   struct mem_block *sorted = 0;

//   for (int i = 0; i < ARR_SIZE; i++) {
//     if (p->arr[i]!= 0){
//       continue;
//     }
//     struct mem_block *current = p->arr[i];
//     p->arr[i] = p->arr[i]->next;

//     if (sorted == 0 || (current->end - current->start) <= (sorted->end - sorted->start)) {
//       current->next = sorted;
//       sorted = current;
//     } else {
//       struct mem_block *temp = sorted;

//       while (temp->next != 0 && (temp->next->end - temp->next->start) < (current->end - current->start)) {
//         temp = temp->next;
//       }

//       current->next = temp->next;
//       temp->next = current;

//     }
//     p->arr[i] = sorted;
//   }
// }

/* Insertion Logic
* Needs to interact with the sort_mem_block function
* Will perform the insertion
*/
int insert_mapping(mem_block* arr[], uint start, uint end, int flags, int length, int numMappings) { // need to pass in start, end, flags, etc. to init mem_block struct to add to array
// arr is initially full of null pointers (0 values), need idx of array to point to a mem_block struct with values we give in parameters to this function
    cprintf("inserting into mmap\n");
    // We already check in the FIXED flag case if the required addr is going to fit
    // We also find the leftmost ADDR that will fit in our mmap when calling it from the ELSE condition
    // Means we just need to find the index that this new mapping belongs, and move other mem_block pointers to make space for this new one

    // if arr is full
    if (numMappings == MAX_WMMAP_INFO) {
      cprintf("can't add any more memory mappings\n");
      return FAILED; // Array is full, return error code
    }
    // arr[idx] is initially NULL (0), need it to point to a mem_block struct after this insert method. do we need to kalloc?
    mem_block* new_mapping = (mem_block*)kalloc();
    new_mapping->start = start;
    new_mapping->end = end;
    new_mapping->flags = flags;
    new_mapping->length = length;
    // find index to insert into
    int i = 0; // Start from the first element of the array

    if (numMappings == 0){ // empty case
      arr[0] = new_mapping;
      cprintf("block inserted - start: %x, end: %x, length: %d, flags: %d\n", start, end, flags, length);
      return arr[0]->start;
    }
    // find correct idx to insert, if already something at idx, move to idx+1 and all the ones to the right as well
    // want to sort by start addr
    // Find the correct position to insert the new mapping
    while (i < MAX_WMMAP_INFO && arr[i] != 0 && arr[i]->start > start) {
        // Check if the new mapping fits between the end of the previous mapping and the start of the next mapping
        if (i > 0 && arr[i - 1] != 0 && (arr[i - 1]->end + 1) >= start && arr[i]->start <= (end)) {
            // Insert the new mapping at the correct position
            arr[i + 1] = new_mapping;
            cprintf("inserting mapping aat idx: %d\n", i+1);
            return arr[i+1]->start;
        }
        arr[i] = arr[i+1]; // Move elements greater than new_mapping to the right
        i++;
    }
    // Insert the new mapping at the correct position
    arr[i + 1] = new_mapping;
    // Check if the new mapping goes beyond MAX_ADDR
    if (arr[i+1]->start + arr[i+1]->length > MAX_ADDR) {
        kfree((char*)new_mapping);
        return FAILED; // Return error code
    }
    cprintf("inserted at idx: %d\n", i+1);
    cprintf("block inserted - start: %x, end: %x, length: %d, flags: %d\n", start, end, flags, length);
    return arr[i+1]->start;
}

/*
 * P4 syscall functions
 */
int
sys_wmap(void){
  // uint wmap(uint addr, int length, int flags, int fd);
  uint addr; // VA we MUST use for the MAP_FIXED
  int length;
  int flags; // we don't care about addr given to us if MAP_FIXED is not set
  int fd; // ignore in case of Map anonymous
  // MAP_SHARED means we have to do some copying from parent-child in fork and exit calls in proc.c
  // MAP_PRIVATE means mappings are not shared between parent and child processes.
  argint(2, &flags);
  argint(0, (int*)&addr);
  argint(1, &length);
  // if (flags & MAP_ANONYMOUS) {
  //   cprintf("flags & MAP_ANONYMOUS: %d\n",flags & MAP_ANONYMOUS);
  // }
  // invalid args?
  if ((addr < 0)  || (length < 1)){ // || argint(3, &fd) < 0) {
    // uint uflags = (uint) flags;
    // cprintf("uflags: %d\n", uflags);
    cprintf("error with args: addr: %d, length: %d, flags: %d\n", addr, length, flags);
    cprintf("%d, %d, %d\n", MAP_ANONYMOUS, MAP_FIXED, MAP_PRIVATE);
    return FAILED;
  }
  argint(3, &fd);
  struct proc *p = myproc();
  // cprintf("p: %p\n", p);

  if (flags & MAP_FIXED) {
    cprintf("MAP_FIXED flag is set...\n");
    // use given address, try to add to our map
    if (addr % PGSIZE != 0){
      return FAILED;
    }
    // TODO check if provided addr is going to fit in our existing mmappings

    // p->arr; // array of length 16, should hold our mmappings
    int pagesNeeded = PGROUNDUP(length) / PGSIZE;
    uint end = addr + PGROUNDUP(length) - 1;
    
    uint startAddr = addr;
    // mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm)
    for (int i = 0; i < pagesNeeded; i++){
      char* mem;
      mem = kalloc(); // kalloc to give us va that maps to pa
      if (mem == 0){
        panic("kalloc");
      }
      memset(mem, 0, PGSIZE);
      cprintf("calling mappages with pgdir = %p, va: %x, size: %d, pa: %x perm: %d\n", p->pgdir, addr, PGSIZE, V2P(mem), PTE_W | PTE_U);
      if (mappages(p->pgdir, (void*) startAddr, PGSIZE, V2P(mem), PTE_W | PTE_U) != 0){
        cprintf("mappages failed\n");
        kfree(mem);
      }
      startAddr = startAddr + PGSIZE; // increment va to map to physical
      cprintf("startAddr: %x\n", startAddr);
    }
    // TODO insert operation into array
    
    cprintf("inserting new mapping: start: %x, end: %x, flags: %d\n", addr, end, flags);
    uint insertAddr = insert_mapping((mem_block**)&p->arr, addr, end, flags, length, p->wmapinfo->total_mmaps);
    
    if (insertAddr == FAILED){ // successful insert?
      return FAILED;
    }
    // succesful insert, update wmapinfo
    p->wmapinfo->addr[p->wmapinfo->total_mmaps] = insertAddr;
    p->wmapinfo->n_loaded_pages[p->wmapinfo->total_mmaps] = PGROUNDUP(length) / PGSIZE;
    p->wmapinfo->length[p->wmapinfo->total_mmaps] = PGROUNDUP(length);
    p->wmapinfo->total_mmaps++;
    // print array after insert:
    for (int i = 0; i < MAX_WMMAP_INFO; i++){
      cprintf("%p\n",p->arr[i]);
    }
    return insertAddr;
  } else{
    // not map fixed, we find place for insert
    // check if array is full
    int numMappings = p->wmapinfo->total_mmaps;
    if (numMappings == MAX_WMMAP_INFO){
        return FAILED;
    }
    // find leftmost available place to insert the new mapping
    int prevEnd = MIN_ADDR - 1;
    int curStart = MIN_ADDR; 
    uint leftmostAddr = MIN_ADDR; // initially minimum address
    cprintf("leftmostaddr before calculation: %x\n", leftmostAddr);
    // if array is empty
    for (int i = 0; i < MAX_WMMAP_INFO; i++){
      // at each iteration, prevEnd will hold the i-1->endAddr curStart will hold the ith startAddr
      // if curStart - prevEnd > PGROUNDUP(length), then there is enough room to fit the block starting at prevEnd + 1

      // if this pointer is null, we can assign it to prevEnd+1
      if (p->arr[i] == 0) {
        leftmostAddr = prevEnd + 1;
        break; 
      }
      curStart = p->arr[i]->start; // we know that ith element of array is nonnull
      if (curStart - prevEnd > PGROUNDUP(length)){ // can we fit our mapping between the ith and i-1 mappings?
        leftmostAddr = prevEnd + 1;
      }
      
      // increment prevEnd
      prevEnd = p->arr[i]->end;
    }
    cprintf("leftmostaddr after new calculation: %x\n", leftmostAddr);

      int end = leftmostAddr + PGROUNDUP(length) - 1;
      // TODO insert at leftmostAddr in our array of mappings
      cprintf("inserting at leftmostAddr: %x, end: %x\n", leftmostAddr, end);
      uint insertAddr = insert_mapping((mem_block**)&p->arr, leftmostAddr, end, flags, length, p->wmapinfo->total_mmaps);
      if (insertAddr == FAILED){
        return FAILED;
      }
      // succesful insert, update wmapinfo
      p->wmapinfo->addr[p->wmapinfo->total_mmaps] = insertAddr;
      p->wmapinfo->n_loaded_pages[p->wmapinfo->total_mmaps] = PGROUNDUP(length) / PGSIZE;
      p->wmapinfo->length[p->wmapinfo->total_mmaps] = PGROUNDUP(length);
      p->wmapinfo->total_mmaps++;
      // print array after insert:
      for (int i = 0; i < MAX_WMMAP_INFO; i++){
        cprintf("%p\n",p->arr[i]);
      }
      return insertAddr;
    
    }
    return FAILED;
}

int
sys_wunmap(void) {
  return -1;
}