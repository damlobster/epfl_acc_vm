#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <stdbool.h>

#include "memory.h"
#include "fail.h"
#include "engine.h"

#define debug(fmt, ...) \
              fprintf(stderr, (fmt), __VA_ARGS__)
//#define debug(fmt, ...) \
//            do {} while(0)

static uvalue_t* memory_start = NULL;
static uvalue_t* memory_end = NULL;

static uvalue_t* heap_start = NULL;
static uvalue_t* bitmap_start = NULL;

#define FREELIST_SIZE 32
#define FREELIST_STEP 1
static uvalue_t* freelists[FREELIST_SIZE] = {NULL};

#define HEADER_SIZE 1

static void* addr_v_to_p(uvalue_t v_addr) {
    return (char*) memory_start + v_addr;
}

static uvalue_t addr_p_to_v(void* p_addr) {
    assert((char*) memory_start <= (char*) p_addr && (char*) p_addr <= (char*) memory_end);
    return (uvalue_t) ((char*) p_addr - (char*) memory_start);
}

// Header & block management
static uvalue_t header_pack(tag_t tag, uvalue_t size) {
    assert(size>0); //REMOVEME
    return (size << (uint8_t) 8) | (uvalue_t) tag;
}

static tag_t header_unpack_tag(uvalue_t header) {
    return (tag_t) (header & (uint8_t) 0xFF);
}

static uvalue_t header_unpack_size(uvalue_t header) {
    uvalue_t size = header >> (uint8_t) 8;
    assert(size>0); //REMOVEME
    return size;
}

static inline uvalue_t get_block_size(uvalue_t* block){
    assert((char*) memory_start < (char*) block);
    assert((char*) block <= (char*) memory_end);
    uvalue_t size = header_unpack_size(block[-1]);
    assert(size>0);
    return size;
}

/*************************************
 * BITMAP functions and macros
 *************************************/

static void bm_set(uvalue_t* block) {
    uvalue_t bytes = (uvalue_t)(block - heap_start);
    uvalue_t index = bytes / VALUE_BITS;
    uvalue_t mask = ((uvalue_t) 1) << (bytes % VALUE_BITS);
    bitmap_start[index] |= mask;
}

static void bm_clear(uvalue_t* block) {
    uvalue_t bytes = (uvalue_t)(block - heap_start);
    uvalue_t index = bytes / VALUE_BITS;
    uvalue_t mask = ~(((uvalue_t) 1) << (bytes % VALUE_BITS));
    bitmap_start[index] &= mask;
}

static int bm_is_set(uvalue_t* block) {
    uvalue_t bytes = (uvalue_t)(block - heap_start);
    uvalue_t index = bytes / VALUE_BITS;
    uvalue_t mask = ((uvalue_t) 1) << (bytes % VALUE_BITS);
    return (bitmap_start[index] & mask) != 0;
}

/*************************************
 * FREE LIST
 *************************************/

static void list_init(){
    for(int i = 0; i < FREELIST_SIZE; i++){
        freelists[i] = memory_start;
    }
}

static inline int list_idx(uvalue_t size){
    int idx = (int)(size - 1) / FREELIST_STEP;
    return idx < FREELIST_SIZE ? idx : FREELIST_SIZE - 1;
}

static uvalue_t list_is_empty(uvalue_t* element) {
    return element == memory_start;
}

static inline uvalue_t* list_next(const uvalue_t* element) {
    assert(element != NULL);
    assert(element > memory_start);
    return addr_v_to_p(element[0]);
}

static inline void list_prepend(int idx, uvalue_t* element) {
    assert(0 <= idx && idx < FREELIST_SIZE);
    element[0] = addr_p_to_v(freelists[idx]);
    freelists[idx] = element;
}

static inline void list_pop_head(int idx){
    assert(0 <= idx && idx < FREELIST_SIZE);
    assert(!list_is_empty(freelists[idx]));
    freelists[idx] = list_next(freelists[idx]);
}

// REMOVEME ******************************************
/*static void debug_list_size(uvalue_t* freelist){
    if(freelist!=NULL){
        int i = 0;
        uvalue_t* current = freelist;
        while(current != memory_start){
            i++;
            current = list_next(current);
        }
        
        debug("list_length = %d\n", i);
    }
}*/

/**********************
 *  Marking
 **********************/

static void rec_mark(uvalue_t* root) {
    if (root > heap_start && root <= memory_end && bm_is_set(root)) {
        bm_clear(root);
        uvalue_t blocksize = memory_get_block_size(root);
        
        for (uvalue_t i = 0; i < blocksize; ++i) {
            if (root[i] != 0 && (root[i] & 3) == 0) {
                rec_mark(addr_v_to_p(root[i]));
            }
        }
    }
}

static void mark() {
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
        debug("sw (%d)\n", block_size);
        
        if (bm_is_set(current)) {
            // not reachable --> free it
            bm_clear(current);
            memset(current, 0, block_size * sizeof(uvalue_t));
            current[-HEADER_SIZE] = header_pack(tag_None, block_size);
        }
        
        if (memory_get_block_tag(current) == tag_None) {
            // coalesce adjacent free blocks
            if(start_free < current){
                current[-HEADER_SIZE] = 0;
                current[0] = 0;
                current = start_free;
                block_size = get_block_size(start_free) + HEADER_SIZE + block_size;
                current[-1] = header_pack(tag_None, block_size);
                
                debug("sw col (%d)\n", block_size);
                if(last_list >= 0 && list_idx(block_size) != last_list){
                    debug("sw pop (%d)\n", last_list);
                    list_pop_head(last_list);
                }
            }

            // update free list
            int idx = list_idx(block_size);
            if(idx != last_list){
                list_prepend(idx, current);
                debug("sw pre (%d)\n", idx);
                last_list = idx;
            }
        } else {
            debug("sw used\n", NULL);
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
    assert(heap_start != NULL);
    if(size == 0){ size = 1; }

    // find best free list
    int idx = -1;
    for(int i = list_idx(size); i < FREELIST_SIZE; i++){
        if( !list_is_empty(freelists[i]) ){ idx = i; }
    }

    uvalue_t* block = NULL;
    if (idx >= 0) {
        uvalue_t* new_free = NULL;
        block = freelists[idx];
        uvalue_t total_size = get_block_size(block);
        size = size < total_size - 1 ? size : total_size;
        
        if(size < total_size){
            // split block
            new_free = block + size + HEADER_SIZE;
            uvalue_t new_free_size = total_size - size - HEADER_SIZE;
            assert(new_free_size > 0);
            new_free[-1] = header_pack(tag_None, new_free_size);
            list_pop_head(idx);
            list_prepend(list_idx(new_free_size), new_free);
        }
        
        bm_set(block);
        block[-HEADER_SIZE] = header_pack(tag, size);
        block[0] = 0;

    }else{
        idx = -1;
    }

    //REMOVEME ***********************************************
    assert(((unsigned long)block & 3) == 0);
    return block;
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
    assert(memory_start != NULL);
    free(memory_start);

    memory_start = memory_end = NULL;
    bitmap_start = heap_start = NULL;
    for(int i = 0; i < FREELIST_SIZE; i++){
        freelists[i] = NULL;
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
    list_prepend(FREELIST_SIZE-1, free);

}

uvalue_t memory_get_block_size(uvalue_t* block) {
    return header_unpack_size(block[-1]);
}

tag_t memory_get_block_tag(uvalue_t* block) {
    return header_unpack_tag(block[-1]);
}
