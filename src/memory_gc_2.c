#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <stdbool.h>

#include "memory.h"
#include "fail.h"
#include "engine.h"

//#define debug(fmt, ...) fprintf(stderr, (fmt), __VA_ARGS__)
#define debug(fmt, ...) do{}while(false)

#define HEADER_SIZE 1

static uvalue_t* memory_start = NULL;
static uvalue_t* memory_end = NULL;

static uvalue_t* heap_start = NULL;
static uvalue_t* bitmap_start = NULL;

#define FL_SIZE 32
static uvalue_t* FL[FL_SIZE] = {NULL};

#ifdef GC_STATS
// counters for basic GC statistics
static uvalue_t gc_count = 0;
static uvalue_t live_count = 0; 
static uvalue_t marked_count = 0;
#endif

/*************************************
 * UTILS functions
 *************************************/

static inline void* addr_v_to_p(uvalue_t v_addr) {
  return (char*) memory_start + v_addr;
}

static inline uvalue_t addr_p_to_v(void* p_addr) {
  return (uvalue_t) ((char*) p_addr - (char*) memory_start);
}

static inline uvalue_t header_pack(tag_t tag, uvalue_t size) {
  return (size << (uint8_t) 8) | (uvalue_t) tag;
}

static inline tag_t header_unpack_tag(uvalue_t header) {
  return (tag_t) (header & (uint8_t) 0xFF);
}

static inline uvalue_t header_unpack_size(uvalue_t header) {
  return header >> (uint8_t) 8;
}

static inline uvalue_t get_block_size(uvalue_t* block){
  return header_unpack_size(block[-HEADER_SIZE]);
}

static inline uvalue_t real_size(uvalue_t size){
  return size == 0 ? 1 : size;
}

static inline tag_t get_block_tag(uvalue_t* block){
  return header_unpack_tag(block[-HEADER_SIZE]);
}

/*************************************
 * BITMAP functions
 *************************************/

static inline void bm_set(uvalue_t* block) {
  uvalue_t bytes = (uvalue_t)(block - heap_start);
  uvalue_t index = bytes / VALUE_BITS;
  uvalue_t mask = ((uvalue_t) 1) << (bytes % VALUE_BITS);
  bitmap_start[index] |= mask;
}

static inline void bm_clear(uvalue_t* block) {
  uvalue_t bytes = (uvalue_t)(block - heap_start);
  uvalue_t index = bytes / VALUE_BITS;
  uvalue_t mask = ~(((uvalue_t) 1) << (bytes % VALUE_BITS));
  bitmap_start[index] &= mask;
}

static inline int bm_is_set(uvalue_t* block) {
  uvalue_t bytes = (uvalue_t)(block - heap_start);
  uvalue_t index = bytes / VALUE_BITS;
  uvalue_t mask = ((uvalue_t) 1) << (bytes % VALUE_BITS);
  return (bitmap_start[index] & mask) != 0;
}

/*************************************
 * FREE LIST
 *************************************/

static void list_init(){
  for(size_t i = 0; i < FL_SIZE; i++){
    FL[i] = memory_start;
  }
}

static inline uvalue_t* list_next(const uvalue_t* element) {
  return addr_v_to_p(element[0]);
}

static void list_remove_next(uvalue_t* element){
  if(element != memory_start){
    uvalue_t* next = list_next(element);
    if(next != memory_start){
      element[0] = addr_p_to_v(list_next(next));
      next[0] = 0;
    }
  }
}

static void list_prepend(int idx, uvalue_t* element) {
  element[0] = addr_p_to_v(FL[idx]);
  FL[idx] = element;
}

static uvalue_t* list_pop_head(int idx){
  uvalue_t* block = FL[idx];
  uvalue_t* next = list_next(block);
  FL[idx] = next;

  return block;
}

static inline int list_idx(uvalue_t size){
  int idx = (int)real_size(size) - 1;
  return idx < FL_SIZE ? idx : FL_SIZE - 1;
}

static void list_print(){
  for(int idx = 0; idx < FL_SIZE; idx++){
    uvalue_t *cur = FL[idx];
    if(cur != memory_start){
      debug("FL[%d]: ", idx);
      while(cur != memory_start){
        debug("%d, ", get_block_size(cur));
        cur = list_next(cur);
      }
      debug("\n", NULL);
    }
  }
}
/**********************
 *  Marking
 **********************/

static void rec_mark(uvalue_t* root) {
  if (root > heap_start && root <= memory_end && bm_is_set(root)) {
    bm_clear(root);

    uvalue_t blocksize = get_block_size(root);
    for (uvalue_t i = 0; i < blocksize; ++i) {
      if (root[i] != 0 && (root[i] & 3) == 0) {
        rec_mark(addr_v_to_p(root[i]));
      }
    }

    #ifdef GC_STATS
    marked_count++;
    #endif
  }
}

static void mark() {
  debug("GC ****************************\n", NULL);
  rec_mark(engine_get_Ib());
  rec_mark(engine_get_Lb());
  rec_mark(engine_get_Ob());

  #ifdef GC_STATS
  gc_count++;
  #endif
}

/************************
 * Sweeping & coalescing
 ************************/

static void sweep() {
  list_init();

  uvalue_t* start_free = heap_start + HEADER_SIZE;
  uvalue_t free_size = 0;
  uvalue_t* current = start_free;
  int last_list = -1;
  
  while (current <= memory_end) {

    uvalue_t current_size = get_block_size(current);

    if (bm_is_set(current)) {
      assert(get_block_tag(current) != tag_None);
      // block is not reachable --> free it
      bm_clear(current);
      current_size = real_size(current_size);
      memset(current, 0, current_size * sizeof(uvalue_t));
      current[-HEADER_SIZE] = header_pack(tag_None, current_size);
    }
    
    if (get_block_tag(current) == tag_None) {
      debug("F ", NULL);
      free_size += current_size;
      // coalesce adjacent free blocks
      if(start_free < current){
        current[-HEADER_SIZE] = 0;
        if(current_size > 0){
          current[0] = 0;
        }
        free_size += HEADER_SIZE;

        // update size of new free block
        start_free[-HEADER_SIZE] = header_pack(tag_None, free_size);

        // rewind current pointer to start of coalesced block
        current = start_free;
        current_size = free_size;
      }

      // update free lists
      int idx = list_idx(free_size);
      if(idx != last_list){
        if(last_list != -1){
          list_pop_head(last_list);
        }
        if(current_size > 0){
          list_prepend(idx, current);
          last_list = idx;
        }
      }
    } else { // the block is not free
      // point start_free on next block
      current_size = real_size(current_size);
      start_free = current + current_size + HEADER_SIZE;
      bm_set(current);
      last_list = -1;
      free_size = 0;
      debug("N ", NULL);

      #ifdef GC_STATS
      live_count++;
      #endif
    }

    debug("%lu %lu %d %d\n", current - heap_start, start_free - heap_start, current_size, free_size);
    // move to next block
    current += current_size + HEADER_SIZE;
  }

  list_print();
}

/****************************
 * Blocks allocation
 ****************************/

// I choosed to use the first fit strategy
static uvalue_t* block_allocate(tag_t tag, uvalue_t size) {
  assert(tag != tag_None);

  uvalue_t realsize = real_size(size);
  debug("BA %u", size);
  for(int idx = list_idx(realsize); idx < FL_SIZE; idx++){
    uvalue_t* free_block = FL[idx];
    uvalue_t* prev = NULL;

    while(free_block != memory_start){
      assert(free_block >= heap_start);
      assert(free_block <= memory_end);
      debug(", (%lu)", free_block - heap_start);
      uvalue_t* new_free = NULL;
      uvalue_t free_size = get_block_size(free_block);

      if(realsize <= free_size){
        debug(", f=%d", free_size);

        // we found a candidate -> remove it from old free list
        if(prev == NULL){
          list_pop_head(idx);
        }else{
          list_remove_next(prev);
        }

        if(realsize < free_size){
          // the allocated block is smaller -> split it
          new_free = free_block + realsize + HEADER_SIZE;
          uvalue_t new_free_size = free_size - realsize - HEADER_SIZE;
          new_free[-HEADER_SIZE] = header_pack(tag_None, new_free_size);
          debug(", nf=%d", new_free_size);
          if(new_free_size != 0){
            debug("*", NULL);
            list_prepend(list_idx(new_free_size), new_free);
          }
        }
          
        // initilize the new block
        bm_set(free_block);
        free_block[-HEADER_SIZE] = header_pack(tag, size);
        free_block[0] = 0;
        debug("\n", NULL);
        return free_block;
      }
        
      // if we are here, we are in the last free list
      // -> go to next block
      prev = free_block;
      free_block = list_next(free_block);
    }
  }
  debug(" ->GC\n", NULL);
  // no block found
  return NULL;
}

uvalue_t* memory_allocate(tag_t tag, uvalue_t size) {
  assert(heap_start != NULL);

  uvalue_t* block = block_allocate(tag, size);
  if (block == NULL) {
    // Ouch! Cleanup garbage!
    mark();
    sweep();
    block = block_allocate(tag, size);

    if (block == NULL) {
      fail("cannot allocate %u bytes of memory", size);
    }
  }

  return block;
}

/********************************
 * Memory initialization and teardown
 ********************************/
char* memory_get_identity() {
  return "Mark and Sweep GC";
}

void memory_setup(size_t total_byte_size) {
  memory_start = calloc(total_byte_size, 1);
  if (memory_start == NULL)
    fail("cannot allocate %zd bytes of memory", total_byte_size);
  memory_end = memory_start + (total_byte_size / sizeof(value_t));
}

void memory_cleanup() {
  assert(memory_start != NULL);
  free(memory_start);

  memory_start = memory_end = NULL;
  bitmap_start = heap_start = NULL;
  for(int i = 0; i < FL_SIZE; i++){
    FL[i] = NULL;
  }

  #ifdef GC_STATS
  printf("\n**********************************");
  printf("\nGC COUNT = %d", gc_count);
  printf("\nMarked count = %d", marked_count);
  printf("\nLive count = %d", live_count);
  printf("\n**********************************\n");
  #endif
}

void* memory_get_start() {
  return memory_start;
}

void* memory_get_end() {
  return memory_end;
}

void memory_set_heap_start(void* p_addr) {
  assert(p_addr != NULL);
  assert(bitmap_start == NULL);

  uvalue_t total = (uvalue_t) ((char*) memory_end - (char*) p_addr) / sizeof(uvalue_t);
  uvalue_t bm_size = (total + VALUE_BITS - 1) / (VALUE_BITS + 1);
  uvalue_t heap_size = total - bm_size;
  
  bitmap_start = p_addr;
  heap_start = bitmap_start + bm_size;

  list_init();
  uvalue_t* free = heap_start + HEADER_SIZE;
  free[-HEADER_SIZE] = header_pack(tag_None, (uvalue_t)(heap_size - HEADER_SIZE));
  list_prepend(FL_SIZE-1, free);
}

uvalue_t memory_get_block_size(uvalue_t* block) {
  return get_block_size(block);
}

tag_t memory_get_block_tag(uvalue_t* block) {
  return get_block_tag(block);
}
