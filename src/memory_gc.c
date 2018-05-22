#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <stdbool.h>

#include "memory.h"
#include "fail.h"
#include "engine.h"

#define debug(fmt, ...) fprintf(stdout, (fmt), __VA_ARGS__)

#define HEADER_SIZE 1

#define FL_SIZE 32
#define FL_RATIO 2
static uvalue_t* FL[FL_SIZE] = {NULL};
static uvalue_t FL_access[FL_SIZE] = {0};
static int FL_size = FL_SIZE;
static int FL_step = 1;

#ifdef GC_COUNT
static uvalue_t gc_count = 0;
#endif

static uvalue_t* memory_start = NULL;
static uvalue_t* memory_end = NULL;

static uvalue_t* heap_start = NULL;
static uvalue_t* bitmap_start = NULL;

/*************************************
 * UTILS functions
 *************************************/

static inline void* addr_v_to_p(uvalue_t v_addr) {
    return (char*) memory_start + v_addr;
}

static inline uvalue_t addr_p_to_v(void* p_addr) {
    //assert((char*) memory_start <= (char*) p_addr && (char*) p_addr <= (char*) memory_end);
    return (uvalue_t) ((char*) p_addr - (char*) memory_start);
}

static inline uvalue_t header_pack(tag_t tag, uvalue_t size) {
    return (size << (uint8_t) 8) | (uvalue_t) tag;
}

static inline tag_t header_unpack_tag(uvalue_t header) {
    return (tag_t) (header & (uint8_t) 0xFF);
}

static inline uvalue_t header_unpack_size(uvalue_t header) {
    uvalue_t size = header >> (uint8_t) 8;
    //assert(size>0); //REMOVEME
    return size;
}

static inline uvalue_t get_block_size(uvalue_t* block){
    //assert((char*) memory_start < (char*) block);
    //assert((char*) block <= (char*) memory_end);
    uvalue_t size = header_unpack_size(block[-1]);
    //assert(size>0);
    return size;
}

static inline tag_t get_block_tag(uvalue_t* block) {
    return header_unpack_tag(block[-1]);
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

static inline void list_init(){
    uvalue_t total = 0;
    for(int i = 0; i < FL_size; i++){
        total += FL_access[i];
    }
    
    if(total > 0){
        total /= FL_RATIO;
        uvalue_t count = 0;
        FL_size = 0;
        for(; FL_size < FL_SIZE && count < total/FL_RATIO; FL_size++){
            count += FL_access[FL_size];
            FL_access[FL_size] = 0;
            //count += FL_prep[i];
        }
    }else{
        FL_step = 1;
        FL_size = FL_SIZE;
    }
    
    for(uvalue_t i = 0; i < FL_SIZE; i++){
        FL[i] = memory_start;
    }
}

static inline int list_idx(uvalue_t size){
    int idx = (int)(size - 1) / FL_step;
    return idx < FL_size ? idx : FL_size - 1;
}

static inline uvalue_t* list_next(const uvalue_t* element) {
    //assert(element != NULL);
    //assert(element > memory_start);
    return addr_v_to_p(element[0]);
}

static inline void list_remove_next(uvalue_t* element){
    if(element != memory_start){
        uvalue_t* next = list_next(element);
        if(next != memory_start){
            element[0] = addr_p_to_v(list_next(next));
            next[0] = 0;
        }
    }
}

static inline void list_prepend(int idx, uvalue_t* element) {
    //assert(0 <= idx && idx < FL_SIZE);
    element[0] = addr_p_to_v(FL[idx]);
    FL[idx] = element;
    //FL_access[idx]++;
}

static inline void list_pop_head(int idx){
    //assert(0 <= idx && idx < FL_SIZE);
    //assert(!list_is_empty(FL[idx]));
    FL[idx] = list_next(FL[idx]);
    //FL_access[idx]++;
}

/**********************
 *  Marking
 **********************/

static inline void rec_mark(uvalue_t* root) {
    if (root > heap_start && root <= memory_end && bm_is_set(root)) {
        bm_clear(root);
        uvalue_t blocksize = get_block_size(root);
        
        for (uvalue_t i = 0; i < blocksize; ++i) {
            if (root[i] != 0 && (root[i] & 3) == 0) {
                rec_mark(addr_v_to_p(root[i]));
            }
        }
    }
}

static void mark() {
#ifdef GC_COUNT
    gc_count++;
#endif
    rec_mark(engine_get_Ib());
    rec_mark(engine_get_Lb());
    rec_mark(engine_get_Ob());
}

/************************
 * Sweeping & coalescing
 ************************/

static void sweep() {
    
    list_init();
    uvalue_t* start_free = heap_start + HEADER_SIZE;
    uvalue_t* current = start_free;
    int last_list = -1;
    
    while (current <= memory_end) {

        uvalue_t block_size = get_block_size(current);
        //assert(block_size<=(memory_end-bitmap_start));
        
        if (bm_is_set(current)) {
            // not reachable --> free it
            bm_clear(current);
            memset(current, 0, block_size * sizeof(uvalue_t));
            current[-HEADER_SIZE] = header_pack(tag_None, block_size);
            //assert(block_size<=(memory_end-bitmap_start));
        }
        
        if (get_block_tag(current) == tag_None) {
            // coalesce adjacent free blocks
            if(start_free < current){
                current[-HEADER_SIZE] = 0;
                current[0] = 0;
                current = start_free;
                block_size += get_block_size(start_free) + HEADER_SIZE;
                current[-1] = header_pack(tag_None, block_size);
                
                //assert(block_size<=(memory_end-bitmap_start));
            }

            // update free list
            int idx = list_idx(block_size);
            if(idx != last_list){
                if(last_list >= 0){
                    list_pop_head(last_list);
                }
                list_prepend(idx, current);
                last_list = idx;
            }
        } else {
            start_free = current + block_size + HEADER_SIZE;
            bm_set(current);
            last_list = -1;
        }

        current += block_size + HEADER_SIZE;
    }
}

/****************************
 * Blocks allocation
 ****************************/

static uvalue_t* block_allocate(tag_t tag, uvalue_t size) {
    //assert(heap_start != NULL);
    if(size == 0){ size = 1; }

    uvalue_t* block = NULL;
    uvalue_t* prev = NULL;
    
    // find best free list
    int idx = list_idx(size);
    for(int i = idx; i < FL_size; i++){
        //debug("qwertz %d\n", i);
        
        prev = NULL;
        block = FL[i];
        //debug("x\n", NULL);
        while(block != memory_start){
            FL_access[idx]++;
            uvalue_t* new_free = NULL;
            uvalue_t total_size = get_block_size(block);
            
            if(size <= total_size){
                size = size < total_size - 1 ? size : total_size;
                if(size < total_size){
                    // split block
                    new_free = block + size + HEADER_SIZE;
                    uvalue_t new_free_size = total_size - size - HEADER_SIZE;
                    //assert(new_free_size > 0);
                    new_free[-1] = header_pack(tag_None, new_free_size);
                    if(prev == NULL){
                        list_pop_head(i);
                    }else{
                        list_remove_next(prev);
                    }
                    list_prepend(list_idx(new_free_size), new_free);
                }else{
                    if(prev == NULL){
                        list_pop_head(i);
                    }else{
                        list_remove_next(prev);
                    }
                }
                
                bm_set(block);
                block[-HEADER_SIZE] = header_pack(tag, size);
                block[0] = 0;
                
                //assert(((unsigned long)block & 3) == 0);
                return block;
            }
            
            prev = block;
            block = list_next(block);
        }
    }
    
    return NULL;
}

uvalue_t* memory_allocate(tag_t tag, uvalue_t size) {
    assert(heap_start != NULL);

    uvalue_t* block = block_allocate(tag, size);
    if (block == NULL) {
        mark();
        sweep();
        block = block_allocate(tag, size);
    }

    if (block == NULL) {
        fail("cannot allocate %u bytes of memory", size);
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
#ifdef GC_COUNT
    printf("GC COUNT = %d\n", gc_count);
    fflush(stdout);
#endif
    debug("FL_size=%d\n", FL_size);
    debug("FL_step=%d\n", FL_step);
    for(int i=0; i <FL_size; i++){
        debug("%d: %u\n", i, FL_access[i]);
    }
    
    assert(memory_start != NULL);
    free(memory_start);

    memory_start = memory_end = NULL;
    bitmap_start = heap_start = NULL;
    for(int i = 0; i < FL_SIZE; i++){
        FL[i] = NULL;
    }
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
