#ifndef _VMA_H_
#define _VMA_H_

#include "types.h"
#include "param.h"
#include "riscv.h"

#define INITIAL_BOTTOM_ADDR ((MAXVA) - (2*PGSIZE))

// Virtual Memory Area
struct vma {
  uint64 addr[MAXVMA];
  uint64 length[MAXVMA];
  uint64 offset[MAXVMA];    // File offset (where, in the file, is the start of the region that the vma will cover)
  int prot[MAXVMA];         // Protection options (READ, WRITE...)
  int flags[MAXVMA];        // Map options (SHARED, PRIVATE...)
  int fd[MAXVMA];
  struct file * file[MAXVMA];
  uint64 bottom_addr; // VMAs will be allocated at the end of proc memory. When a vma is allocated, bottom_addr will decrease as much as said vma takes.
};

#endif // _VMA_H_