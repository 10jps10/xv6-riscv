//
// Support functions for system calls that involve file descriptors.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "stat.h"
#include "proc.h"

struct devsw devsw[NDEV];
struct {
  struct spinlock lock;
  struct file file[NFILE];
} ftable;

void
fileinit(void)
{
  initlock(&ftable.lock, "ftable");
}

// Allocate a file structure.
struct file*
filealloc(void)
{
  struct file *f;

  acquire(&ftable.lock);
  for(f = ftable.file; f < ftable.file + NFILE; f++){
    if(f->ref == 0){
      f->ref = 1;
      release(&ftable.lock);
      return f;
    }
  }
  release(&ftable.lock);
  return 0;
}

// Increment ref count for file f.
struct file*
filedup(struct file *f)
{
  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("filedup");
  f->ref++;
  release(&ftable.lock);
  return f;
}

// Close file f.  (Decrement ref count, close when reaches 0.)
void
fileclose(struct file *f)
{
  struct file ff;

  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("fileclose");
  if(--f->ref > 0){
    release(&ftable.lock);
    return;
  }
  ff = *f;
  f->ref = 0;
  f->type = FD_NONE;
  release(&ftable.lock);

  if(ff.type == FD_PIPE){
    pipeclose(ff.pipe, ff.writable);
  } else if(ff.type == FD_INODE || ff.type == FD_DEVICE){
    begin_op();
    iput(ff.ip);
    end_op();
  }
}

// Get metadata about file f.
// addr is a user virtual address, pointing to a struct stat.
int
filestat(struct file *f, uint64 addr)
{
  struct proc *p = myproc();
  struct stat st;
  
  if(f->type == FD_INODE || f->type == FD_DEVICE){
    ilock(f->ip);
    stati(f->ip, &st);
    iunlock(f->ip);
    if(copyout(p->pagetable, addr, (char *)&st, sizeof(st)) < 0)
      return -1;
    return 0;
  }
  return -1;
}

// Read from file f.
// addr is a user virtual address.
int
fileread(struct file *f, uint64 addr, int n)
{
  int r = 0;

  if(f->readable == 0)
    return -1;

  if(f->type == FD_PIPE){
    r = piperead(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    if(f->major < 0 || f->major >= NDEV || !devsw[f->major].read)
      return -1;
    r = devsw[f->major].read(1, addr, n);
  } else if(f->type == FD_INODE){
    ilock(f->ip);
    if((r = readi(f->ip, 1, addr, f->off, n)) > 0)
      f->off += r;
    iunlock(f->ip);
  } else {
    panic("fileread");
  }

  return r;
}

// Write to file f.
// addr is a user virtual address.
int
filewrite(struct file *f, uint64 addr, int n)
{
  int r, ret = 0;

  if(f->writable == 0)
    return -1;

  if(f->type == FD_PIPE){
    ret = pipewrite(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    if(f->major < 0 || f->major >= NDEV || !devsw[f->major].write)
      return -1;
    ret = devsw[f->major].write(1, addr, n);
  } else if(f->type == FD_INODE){
    // write a few blocks at a time to avoid exceeding
    // the maximum log transaction size, including
    // i-node, indirect block, allocation blocks,
    // and 2 blocks of slop for non-aligned writes.
    // this really belongs lower down, since writei()
    // might be writing a device like the console.
    int max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE;
    int i = 0;
    while(i < n){
      int n1 = n - i;
      if(n1 > max)
        n1 = max;

      begin_op();
      ilock(f->ip);
      if ((r = writei(f->ip, 1, addr + i, f->off, n1)) > 0)
        f->off += r;
      iunlock(f->ip);
      end_op();

      if(r != n1){
        // error from writei
        break;
      }
      i += r;
    }
    ret = (i == n ? n : -1);
  } else {
    panic("filewrite");
  }

  return ret;
}

uint64
mmap(uint64 addr, int force_addr, int offset, int length, int prot, int flags, int fd, struct file * f)
{
  struct proc * p = myproc();
  // Find a free entry in process vma list
  int index = vma_find_free_entry(& (p->vma_list));
  if (index < 0)
    return -1; // There are no free vma entries left

  if (!force_addr)
  {
    //Undefined virtual address. Kernel will decide.
    addr = vma_get_new_addr(&(p->vma_list), length);
  }
  // Fill vma entry
  if (vma_fill(& (p->vma_list), index, addr, offset, length, prot, flags, fd, f) < 0) {
    return -1;  //VMA entry index was invalid . (This should never actually happen as it is already checked before)
  }
  //Increment ref count to file
  filedup(f);
  return addr;
}

int
munmap(uint64 addr, int length)
{
  //addr must be page aligned (according to 'man munmap')
  if ((addr % PGSIZE) != 0)
    return -1;
  //length may not be page aligned (according to 'man munmap')

  struct proc * p = myproc();
  // Find the vma entry associated to the VMA the address is pointing to
  int index1 = vma_find(& (p->vma_list), addr);
  if (index1 < 0)
    return -1;  //This address is not pointing to a VMA
  
  //Check if we are freeing from the beginning of the VMA.
  if (addr == p->vma_list.addr[index1])
  {
    int freed = vma_free_pages(& (p->vma_list), index1, addr, length, p->pagetable);
    if (freed < 0)
      return -1;
    p->vma_list.addr[index1] += freed;
    p->vma_list.offset[index1] += freed;
    p->vma_list.length[index1] -= freed;
  }
  //Check if we are freeing to the end of the VMA
  else if (PGROUNDUP(addr + length) == (p->vma_list.addr[index1] + p->vma_list.length[index1])) {
    int freed = vma_free_pages(& (p->vma_list), index1, addr, length, p->pagetable);
    if (freed < 0)
      return -1;
    p->vma_list.length[index1] -= freed;
  }
  //Else, we are freeing a region in the middle of the VMA
  else {
    //Our current VMA will be split into two, so we need another VMA entry
    int index2 = vma_find_free_entry(& (p->vma_list));
    if (index2 < 0)
      return -1;
    
    //Get length of the first VMA
    int length_vma1 = addr - p->vma_list.addr[index1];
    //Free requested VMA pages
    int freed = vma_free_pages(& (p->vma_list), index1, addr, length, p->pagetable);
    //Get address, length and offset of the second VMA
    uint64 addr_vma2 = addr + freed;
    int length_vma2 = p->vma_list.length[index1] - freed - length_vma1;
    int offset_vma2 = p->vma_list.offset[index1] + freed + length_vma1;

    //Update the first VMA and fill the second VMA
    p->vma_list.length[index1] = length_vma1;
    vma_fill(& (p->vma_list), index2, addr_vma2, offset_vma2, length_vma2, p->vma_list.prot[index1], p->vma_list.flags[index1], p->vma_list.fd[index1], p->vma_list.file[index1]);
    //Increment references to file as now we have two VMAs instead of one referencing the file
    filedup(p->vma_list.file[index2]);
  }

  return 0;
}