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
void sort_mem_blocks(struct proc *p) {
  const int ARR_SIZE = 16;

  struct mem_block *sorted = 0;

  for (int i = 0; i < ARR_SIZE; i++) {
    if (p->arr[i]!= 0){
      continue;
    }
    struct mem_block *current = p->arr[i];
    p->arr[i] = p->arr[i]->next;

    if (sorted == 0 || (current->end - current->start) <= (sorted->end - sorted->start)) {
      current->next = sorted;
      sorted = current;
    } else {
      struct mem_block *temp = sorted;

      while (temp->next != 0 && (temp->next->end - temp->next->start) < (current->end - current->start)) {
        temp = temp->next;
      }

      current->next = temp->next;
      temp->next = current;

    }
    p->arr[i] = sorted;
  }
}

/* Insertion Logic
* Needs to interact with the sort_mem_block function
* Will perform the insertion
*/
int insert_mapping(mem_block* arr[], int size, uint start, uint end, int flags, int length) { // need to pass in start, end, flags, etc. to init mem_block struct to add to array
// arr is initially full of null pointers (0 values), need idx of array to point to a mem_block struct with values we give in parameters to this function
    int i = size - 1; // Start from the last element of the array
    
    // Check if the array is already full
    if (arr[size - 1] != 0) {
        return FAILED; // Array is full, return error code
    }
    // new_mapping->start = start;
    // new_mapping->end = end;
    // new_mapping->flags = flags;
    // new_mapping->length = length;
    // Find the correct position to insert the new mapping
    while (i >= 0 && arr[i] != 0 && arr[i]->start > start) {
        // Check if the new mapping fits between the end of the previous mapping and the start of the next mapping
        if (i > 0 && arr[i - 1] != 0 && (arr[i - 1]->end + 1) >= start && arr[i]->start <= (start + length)) {
            // Insert the new mapping at the correct position
            arr[i + 1]->start = start;
            arr[i + 1]->end = end;
            arr[i + 1]->flags = flags;
            arr[i + 1]->length = length;

            return arr[i+1]->start;
        }
        arr[i + 1] = arr[i]; // Move elements greater than new_mapping to the right
        i--;
    }
    // Insert the new mapping at the correct position
    arr[i + 1]->start = start;
    arr[i + 1]->end = end;
    arr[i + 1]->flags = flags;
    arr[i + 1]->length = length;
    // Check if the new mapping goes beyond MAX_ADDR
    if (arr[i+1]->start + arr[i+1]->length > MAX_ADDR) {
        return FAILED; // Return error code
    }
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
  if (flags & MAP_ANONYMOUS) {
    cprintf("flags & MAP_ANONYMOUS: %d\n",flags & MAP_ANONYMOUS);
  }
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
  cprintf("p: %p\n", p);

  cprintf("PGROUNDUP(length): %d\n", PGROUNDUP(length));
  cprintf("PGROUNDUP(length): %d\n", PGROUNDUP(4095));
  cprintf("PGROUNDDOWN(4098): %d\n", PGROUNDDOWN(4098));
  cprintf("PGROUNDDOWN(8078): %d\n", PGROUNDDOWN(8078));
  if (flags & MAP_FIXED) {
    cprintf("MAP_FIXED flag is set...\n");
    // use given address, try to add to our map
    if (addr % PGSIZE != 0){
      return FAILED;
    }
    // TODO check if provided addr is going to fit in our existing mmappings

    // p->arr; // array of length 16, should hold our mmappings
    int pagesNeeded = PGROUNDUP(length) / PGSIZE;
    char* mem;
    // allocate physical pages, map virtual to physical
    for (int i = 0; i < pagesNeeded; i++){
      mem = kalloc();
      mappages(p->pgdir, (void*) addr, PGSIZE, V2P(mem), PTE_W | PTE_U);
      addr = addr + PGSIZE; // increment va to map to physical
    }
    // TODO insert operation into array

    return pagesNeeded;
  } else{
    // not map fixed, we find place for insert
    // if array is full
    if (p->arr[15] != 0){
        return FAILED;
    }
    int leftmostAddr = 0;
    cprintf("leftmostaddr: %d\n", leftmostAddr);
    // if array is empty
    if (p->arr[0] == 0){
      if (MIN_ADDR + length <= MAX_ADDR){
        leftmostAddr = MIN_ADDR;
      }else{
        return FAILED;
      }
    }else{
      leftmostAddr = MAX_ADDR;
      // iter through array to find leftmost block our mapping will fit
      for (int i = 0; i < 16; i++) {
        if (p->arr[i] != 0){
          if (i==0 || (p->arr[i]->start - p->arr[i-1]->end) >= PGROUNDUP(length)){
            if (p->arr[i]->end + 1 + length <= MAX_ADDR) {
              leftmostAddr = p->arr[i]->end + 1;
              break;
            }else{
              return FAILED;
            }
          }
        }
      }
      if (leftmostAddr + length > MAX_ADDR){
        return FAILED;
      }
      }
      // TODO insert at leftmostAddr in our array of mappings
      return leftmostAddr;
    
    }
  return FAILED;
}

int
sys_wunmap(void) {
  return -1;
}