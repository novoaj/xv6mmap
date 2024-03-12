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

/*
* Will perform the insertion in sorted order by start time. creates new mem_block to be inserted.
* relies on wmap to check if the interval: (start, end) will fit into mmap BEFORE calling insert_mapping.
* this function will also update out wmapinfo struct 
*/
int insert_mapping(uint start, uint end, int flags, int length, int numMappings, int fd) { // need to pass in start, end, flags, etc. to init mem_block struct to add to array
// arr is initially full of null pointers (0 values), need idx of array to point to a mem_block struct with values we give in parameters to this function
    cprintf("inserting into mmap\n");
    // We already check in the FIXED flag case if the required addr is going to fit
    // We also find the leftmost ADDR that will fit in our mmap when calling it from the ELSE condition
    // Means we just need to find the index that this new mapping belongs, and move other mem_block pointers to make space for this new one
    struct proc* p = myproc();
    // if arr is full
    if (numMappings == MAX_WMMAP_INFO) {
      cprintf("can't add any more memory mappings\n");
      return FAILED; // Array is full, return error code
    }
    // arr[idx] is initially NULL (0), need it to point to a mem_block struct after this insert method. do we need to kalloc?
    mem_block* new_mapping = (mem_block*)kalloc();
    if (new_mapping == 0){
      panic("kalloc()");
      return FAILED;
    }
    new_mapping->start = start;
    new_mapping->end = end;
    new_mapping->flags = flags;
    new_mapping->length = length;
    new_mapping->fd = fd;
    // find index to insert into

    if (numMappings == 0){ // empty case
      p->arr[0] = new_mapping;
      p->wmapinfo->addr[0] = p->arr[0]->start;
      p->wmapinfo->length[0] = p->arr[0]->length;
      p->wmapinfo->n_loaded_pages[0] = PGROUNDUP(p->arr[0]->length) / PGSIZE;
      p->wmapinfo->total_mmaps = p->wmapinfo->total_mmaps + 1;
      cprintf("block inserted - idx: %d, start: %x, end: %x, length: %d, flags: %d\n", 0, start, end, length, flags);
      return p->arr[0]->start;
    }
    // find correct idx to insert, if already something at idx, move to idx+1 and all the ones to the right as well
    // want to sort by start addr
    // Find the correct position to insert the new mapping
    int insert_idx = numMappings;
    while (insert_idx > 0 && p->arr[insert_idx - 1]->start > start) { // if ith mapping belongs before i-1 mapping, shift i-1 to i (right shift)
      p->arr[insert_idx] = p->arr[insert_idx - 1];
      p->wmapinfo->addr[insert_idx] = p->wmapinfo->addr[insert_idx - 1];
      p->wmapinfo->length[insert_idx] = p->wmapinfo->length[insert_idx - 1];
      p->wmapinfo->n_loaded_pages[insert_idx] = p->wmapinfo->n_loaded_pages[insert_idx - 1];

      insert_idx--;
    }
    // insert mapping
    p->arr[insert_idx] = new_mapping;
    p->wmapinfo->addr[insert_idx] = p->arr[insert_idx]->start;
    p->wmapinfo->length[insert_idx] = p->arr[insert_idx]->length;
    p->wmapinfo->n_loaded_pages[insert_idx] = PGROUNDUP(p->arr[insert_idx]->length) / PGSIZE;
    p->wmapinfo->total_mmaps = p->wmapinfo->total_mmaps + 1;
    cprintf("block inserted - idx: %d, start: %x, end: %x, length: %d, flags: %d\n", insert_idx, start, end, flags, length);
    return new_mapping->start;
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

  if (flags & MAP_FIXED) {
    cprintf("MAP_FIXED flag is set...\n");
    // use given address, try to add to our map
    if (addr % PGSIZE != 0){
      return FAILED;
    }
    uint end = addr + PGROUNDUP(length) - 1;

    // check if provided addr is going to fit in our existing mmappings
    for (int i = 0; i < MAX_WMMAP_INFO; i++){
      if (p->arr[i] != 0){
        // will this mapping overlap with an existing one?
        if (addr > p->arr[i]->start && addr < p->arr[i]->end){ // start doesn't fall in the middle of an existing mapping
          return FAILED;
        }
        if (end > p->arr[i]->start && end < p->arr[i]->end){ // end doesn't fall in the middle of an existing mapping
          return FAILED;
        }
      }
    }
    // insert operation into array
    cprintf("inserting new mapping: start: %x, end: %x, flags: %d\n", addr, end, flags);
    uint insertAddr = insert_mapping(addr, end, flags, (length), p->wmapinfo->total_mmaps, fd);
    
    if (insertAddr == FAILED){ // successful insert?
      return FAILED;
    }
    // print array after insert:
    cprintf("\n\narray after inserting: \n\n");
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
      uint insertAddr = insert_mapping(leftmostAddr, end, flags, (length), p->wmapinfo->total_mmaps, fd);
      if (insertAddr == FAILED){
        return FAILED;
      }
      // succesful insert, update wmapinfo
      // p->wmapinfo->addr[p->wmapinfo->total_mmaps] = insertAddr;
      // p->wmapinfo->n_loaded_pages[p->wmapinfo->total_mmaps] = PGROUNDUP(length) / PGSIZE;
      // p->wmapinfo->length[p->wmapinfo->total_mmaps] = PGROUNDUP(length);
      // p->wmapinfo->total_mmaps++;

      // print array after insert:
      cprintf("\n\narray after inserting: \n\n");
      for (int i = 0; i < MAX_WMMAP_INFO; i++){
        cprintf("%p\n",p->arr[i]);
      }
      return insertAddr;
    
    }
    return FAILED;
}

int
sys_wunmap(void) {
  uint addr;

  argint(0, (int*)&addr);

  // addr must be page aligned
  if (addr % PGSIZE != 0){
    cprintf("Address not page aligned\n");
    return FAILED;
  }
  // must be start of an existing mmap
  struct proc* p = myproc();
  mem_block* toFree;
  int mappingExists = 0;
  int location = 0;
  for (int i = 0; i < p->wmapinfo->total_mmaps; i++){
    // if we can't find the mmap to remove, return FAILED
    if (p->arr[i]->start == addr){
      mappingExists = 1;
      location = i;
      toFree = p->arr[i];
      break;
    }
  }

  if (mappingExists == 0){ // no existing mapping with start = addr
    return FAILED;
  }

  // if NOT MAP_ANON and NOT MAP_PRIVATE: write to file BEFORE unmapping
  if ((toFree->flags & MAP_ANONYMOUS) == 0 && (toFree->flags & MAP_PRIVATE) == 0){
    // if neither of these flags are set, write to file
    struct file* f = p->ofile[toFree->fd];
    // f->off = 0;
    filewrite(f, (void*)toFree->start, toFree->length);
  }
  // remove mapping in PT (walk pg dir) - keep in mind can be multiple pages
  // remove mapping from data structure (remove operetion on p->arr)
  cprintf("mapping to remove at idx: %d\n", location);
  // p->arr[location]; // points to the mem_block we need to remove. check flags and see if file backed. if file backed, write to file
  // TODO: MAP_PRIVATE case requires us to go into fork or allocproc to copy over new mem mappings (Same virtual addrs but need to generate new physical ones for child process)
  // TODO: MAP_SHARED means this mapping is file backed
  // TODO: MAP_ANONYMOUS means this mapping is not file backed
  // NOTE: these ^ don't neccesarily happen here, most logic is probably going to be written in fork or exit or allocproc. 
  // TODO: consider flags (if MAP_SHARED, write mem data back to the file)
  // TODO: write remove method for out mmap array (needs to keep array in order and contiguous - no holes in array, when removing ith index, i+1 onward should shift left in arr)
  return SUCCESS;
}

uint sys_wremap(){
  return -1;
}

int sys_getpgdirinfo(){
  cprintf("getting pgdirinfo...\n\n");
  struct pgdirinfo* pdinfo;
  argptr(0, (char**)&pdinfo, sizeof(struct pgdirinfo*));

  if (pdinfo == 0){ // NULL pointer
    return FAILED;
  }
  struct proc* p = myproc();
  // only collect pages that have PTE_U set
  pde_t* pgDir = p->pgdir;
  int count = 0;

  for (int i = 0; i < NPDENTRIES; i++){ // loop through all the page directories
  // are they present are they PTE_U? if so check PTEs stored in them
  // find PDE using PDX(va)? but we want to go through all PDXs anyway
    pde_t* pde = &pgDir[i];
    if ((PTE_FLAGS(*pde) & PTE_P) && (PTE_FLAGS(*pde) & PTE_U)){
      // cprintf("P and U flag set- pde: %p\n",pde);
      // cprintf("*pde: %x\n", *pde);
      // go to PT that this PDE points to
      pte_t* pgTable = (pte_t*)P2V(PTE_ADDR(*pde));
      cprintf("pt: %x\n", pgTable);
      // iter through PTEs
      for (int j = 0; j < NPTENTRIES; j++){
        pte_t* pte = &pgTable[j]; // PPN and offset
        if ((PTE_FLAGS(*pte) & PTE_P) && (PTE_FLAGS(*pte) & PTE_U)){ 
          cprintf("pte: %x\n", pte);
          cprintf("PTE_ADDR(*pte): %x\n", PTE_ADDR(*pte));
          cprintf("*pte: %x\n", *pte);
          cprintf("*pte - PTE_ADDR(*pte): %x\n\n", *pte - PTE_ADDR(*pte));
          uint offset = 0x000; // offset is 0
          // is this offset^
          pte_t* pa = (pte_t*)P2V(PTE_ADDR(*pte));
          uint va = PGADDR(i, j, offset); // finds va, need to find OFFSET to get va
          // va has 32 bits -> 10 for PDI, 10 for PTI, 12 for offset
          // i is PDI, j is PTI, what is offset? offset is 3 hex digits or 12 bits, last 12 bits of va
          cprintf("va: i: %x, j: %x, offset: %x\n", i,j,offset);
          cprintf("va: %x\n", va);
          cprintf("pa: %x\n", pa);
          cprintf("count: %d\n", count);
          // cprintf("pa: %x\n", V2P(PTE_ADDR(pte)));
          if (count < MAX_UPAGE_INFO){ 
            pdinfo->va[count] = va;
            pdinfo->pa[count] = (uint) pa;
          }
          count++;
          pdinfo->n_upages = count;
          // all of the page table entries that are present and in the user addr space
        }
      }
    }
  }
  
  // fill in struct from this proc pgdir - use myproc, look at pgdir field
  return SUCCESS;
}

int sys_getwmapinfo(){
  // getwmapinfo
  struct wmapinfo* wminfo;
  argptr(0, (char**)&wminfo, sizeof(struct wmapinfo*));

  if (wminfo == 0){
    return FAILED;
  }
  struct proc* p = myproc();
  wminfo->total_mmaps = p->wmapinfo->total_mmaps;
  for (int i = 0; i < p->wmapinfo->total_mmaps; i++){
    wminfo->addr[i] = p->wmapinfo->addr[i];
    wminfo->length[i] = p->wmapinfo->length[i];
    wminfo->n_loaded_pages[i] = p->wmapinfo->n_loaded_pages[i];
  }

  // use wmap struct from our process to fill in wminfo
  return SUCCESS;
}