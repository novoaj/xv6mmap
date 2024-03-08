#ifndef _MEMLAYOUT_H_
#define _MEMLAYOUT_H_

#include "types.h"

struct mem_block {
    uint start; // Start address of the block
    uint end;   // End address of the block
    struct mem_block *prev; // Pointer to the previous block
    struct mem_block *next; // Pointer to the next block
};

#endif
