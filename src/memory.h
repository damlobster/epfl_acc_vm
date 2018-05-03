#ifndef MEMORY_H
#define MEMORY_H

#include <stdlib.h>
#include "vmtypes.h"

typedef enum {
  tag_String = 200,
  tag_RegisterFrame = 201,
  tag_Function = 202,
  tag_None = 255
} tag_t;

/* Returns a string identifying the memory system */
char* memory_get_identity(void);

/* Setup the memory allocator and garbage collector */
void memory_setup(size_t total_size);

/* Tear down the memory */
void memory_cleanup(void);

/* Get first memory address */
void* memory_get_start(void);

/* Get last memory address */
void* memory_get_end(void);

/* Set the heap start, following the code area */
void memory_set_heap_start(void* heap_start);

/* Allocate block, return physical pointer to the new block */
uvalue_t* memory_allocate(tag_t tag, uvalue_t size);

/* Unpack block size from a physical pointer */
uvalue_t memory_get_block_size(uvalue_t* block);

/* Unpack block tag from a physical pointer */
tag_t memory_get_block_tag(uvalue_t* block);

#endif
