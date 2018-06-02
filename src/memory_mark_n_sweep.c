#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <stdbool.h>

#include "memory.h"
#include "fail.h"
#include "engine.h"

#define HEADER_SIZE 1

static uvalue_t *memory_start = NULL;
static uvalue_t *memory_end = NULL;

static uvalue_t *heap_start = NULL;
static uvalue_t *bitmap_start = NULL;

#define FL_SIZE 32
static uvalue_t *FL[FL_SIZE] = {NULL};

#ifdef GC_STATS
static uvalue_t gc_count = 0;
#endif

/*************************************
 * UTILS
 *************************************/

static inline void *addr_v_to_p(uvalue_t v_addr){
    return (char *)memory_start + v_addr;
}

static inline uvalue_t addr_p_to_v(void *p_addr){
    return (uvalue_t)((char *)p_addr - (char *)memory_start);
}

static inline uvalue_t header_pack(tag_t tag, uvalue_t size){
    return (size << (uint8_t)8) | (uvalue_t)tag;
}

static inline tag_t header_unpack_tag(uvalue_t header){
    return (tag_t)(header & (uint8_t)0xFF);
}

static inline uvalue_t header_unpack_size(uvalue_t header){
    return header >> (uint8_t)8;
}

static inline uvalue_t get_block_size(uvalue_t *block){
    return header_unpack_size(block[-HEADER_SIZE]);
}

static inline uvalue_t real_size(uvalue_t size){
    return size == 0 ? 1 : size;
}

static inline tag_t get_block_tag(uvalue_t *block){
    return header_unpack_tag(block[-HEADER_SIZE]);
}

/*************************************
 * BITMAP
 *************************************/

static inline void bm_set(uvalue_t *block){
    uvalue_t bytes = (uvalue_t)(block - heap_start);
    uvalue_t index = bytes / VALUE_BITS;
    uvalue_t mask = ((uvalue_t)1) << (bytes % VALUE_BITS);
    bitmap_start[index] |= mask;
}

static inline void bm_clear(uvalue_t *block){
    uvalue_t bytes = (uvalue_t)(block - heap_start);
    uvalue_t index = bytes / VALUE_BITS;
    uvalue_t mask = ~(((uvalue_t)1) << (bytes % VALUE_BITS));
    bitmap_start[index] &= mask;
}

static inline int bm_is_set(uvalue_t *block){
    uvalue_t bytes = (uvalue_t)(block - heap_start);
    uvalue_t index = bytes / VALUE_BITS;
    uvalue_t mask = ((uvalue_t)1) << (bytes % VALUE_BITS);
    return (bitmap_start[index] & mask) != 0;
}

/*************************************
 * FREE LISTS
 *************************************/

static inline void list_init(){
    for (size_t i = 0; i < FL_SIZE; i++){
        FL[i] = memory_start;
    }
}

static inline uvalue_t *list_next(const uvalue_t *element){
    return addr_v_to_p(element[0]);
}

static inline void list_remove_next(uvalue_t *element){
    if (element != memory_start){
        uvalue_t *next = list_next(element);
        if (next != memory_start){
            element[0] = addr_p_to_v(list_next(next));
            next[0] = 0;
        }
    }
}

static inline void list_prepend(int idx, uvalue_t *element){
    element[0] = addr_p_to_v(FL[idx]);
    FL[idx] = element;
}

static inline void list_remove_head(int idx){
    FL[idx] = list_next(FL[idx]);
}

static inline int list_idx(uvalue_t size){
    int idx = (int)size - 1;
    return idx < FL_SIZE ? idx : FL_SIZE - 1;
}

/*************************************
 *  Marking
 *************************************/

static void rec_mark(uvalue_t *root){
    if (root > heap_start && root <= memory_end && bm_is_set(root)){
        bm_clear(root);

        uvalue_t blocksize = get_block_size(root);
        for (uvalue_t i = 0; i < blocksize; ++i){
            if (root[i] != 0 && (root[i] & 3) == 0){
                rec_mark(addr_v_to_p(root[i]));
            }
        }
    }
}

static void mark(){
    rec_mark(engine_get_Ib());
    rec_mark(engine_get_Lb());
    rec_mark(engine_get_Ob());

    #ifdef GC_STATS
    gc_count++;
    #endif
}

/*************************************
 * Sweeping & coalescing
 *************************************/
static inline uvalue_t coalesce(uvalue_t* start_free, uvalue_t* current, uvalue_t cur_size){
    current[-HEADER_SIZE] = 0;
    if (cur_size > 0)
        current[0] = 0;

    // update size of new free block
    uvalue_t free_size = (uvalue_t)(current - start_free) + cur_size;
    start_free[-HEADER_SIZE] = header_pack(tag_None, free_size);

    return free_size;
}

static void sweep(){
    list_init();

    uvalue_t *start_free = heap_start + HEADER_SIZE;
    uvalue_t *current = start_free;
    int last_list = -1;

    while (current <= memory_end){
        uvalue_t current_size = get_block_size(current);

        if (bm_is_set(current)){
            // block is not reachable --> free it
            bm_clear(current);
            current_size = real_size(current_size);
            memset(current, 0, current_size * sizeof(uvalue_t));
            current[-HEADER_SIZE] = header_pack(tag_None, current_size);
        }

        if (get_block_tag(current) == tag_None){
            // coalesce adjacent free blocks
            if (start_free < current){
                current_size = coalesce(start_free, current, current_size);
                current = start_free;
            }

            // update free lists
            int idx = list_idx(current_size);
            if (idx != last_list){
                if (last_list != -1){
                    list_remove_head(last_list);
                }
                list_prepend(idx, current);
                last_list = idx;
            }
        }else{ 
            // the block is not free -> point start_free on next block
            current_size = real_size(current_size);
            start_free = current + current_size + HEADER_SIZE;
            bm_set(current);
            last_list = -1;
        }

        current += current_size + HEADER_SIZE;
    }
}

/*************************************
 * Blocks allocation
 *************************************/

static uvalue_t *block_allocate(tag_t tag, uvalue_t size){
    uvalue_t realsize = real_size(size);
    int fl_idx = list_idx(realsize); 
    for (int idx = fl_idx; idx < FL_SIZE; idx++){
        #ifdef NO_0_BLOCKS
        if((fl_idx != FL_SIZE - 2) && (idx == fl_idx + 1)){ continue; }
        #endif

        uvalue_t *block = FL[idx];
        uvalue_t *prev = NULL;


        while (block != memory_start){
            uvalue_t total_size = get_block_size(block);

            #ifdef NO_0_BLOCKS
            if (realsize <= total_size && realsize != total_size - 1){
            #else
            if (realsize <= total_size){
            #endif
                // we found a candidate -> remove it from old free list
                if (prev == NULL){
                    list_remove_head(idx);
                }else{
                    list_remove_next(prev);
                }

                if (realsize < total_size){
                    // the allocated block is smaller -> split it 
                    uvalue_t *new_free = block + realsize + HEADER_SIZE;
                    uvalue_t new_free_size = total_size - realsize - HEADER_SIZE;
                    new_free[-HEADER_SIZE] = header_pack(tag_None, new_free_size);

                    if (new_free_size > 0){
                        // Note: if the remaining free size is 0, a tag_None block of size 0
                        // is created on the heap, I let him alone as it will be coalesced with
                        // one of the adjacent block when they are collected.
                        // My tests showed me that this is better (than try to find an another
                        // block with enough space) in term of performance and often result in an
                        // out of memory crash happening later (except for the maze program).
                        // To disable this: build with "make no0blocks"
                        list_prepend(list_idx(new_free_size), new_free);
                    }
                }

                // initilize the new block
                bm_set(block);
                block[-HEADER_SIZE] = header_pack(tag, size);
                block[0] = 0;
                return block;
            }

            // if we are here, we are in the last free list
            // -> go to next block
            prev = block;
            block = list_next(block);
        }
    }
    // no block found
    return NULL;
}

uvalue_t *memory_allocate(tag_t tag, uvalue_t size){
    assert(heap_start != NULL);

    uvalue_t *block = block_allocate(tag, size);
    if (block == NULL){
        // Ouch! Cleanup garbage!
        mark();
        sweep();
        block = block_allocate(tag, size);

        if (block == NULL){
            fail("cannot allocate %u bytes of memory", size);
        }
    }

    return block;
}

/*************************************
 * Memory initialization and teardown
 *************************************/
char *memory_get_identity(){
    return "Mark and Sweep GC";
}

void memory_setup(size_t total_byte_size){
    memory_start = calloc(total_byte_size, 1);
    if (memory_start == NULL)
        fail("cannot allocate %zd bytes of memory", total_byte_size);
    memory_end = memory_start + (total_byte_size / sizeof(value_t));
}

void memory_cleanup(){
    assert(memory_start != NULL);
    free(memory_start);

    memory_start = memory_end = NULL;
    bitmap_start = heap_start = NULL;
    for (int i = 0; i < FL_SIZE; i++){
        FL[i] = NULL;
    }

#ifdef GC_STATS
    printf("\nGC COUNT = %d\n", gc_count);
#endif
}

void *memory_get_start(){
    return memory_start;
}

void *memory_get_end(){
    return memory_end;
}

void memory_set_heap_start(void *p_addr){
    assert(p_addr != NULL);
    assert(bitmap_start == NULL);

    uvalue_t total = (uvalue_t)((char *)memory_end - (char *)p_addr) / sizeof(uvalue_t);
    uvalue_t bm_size = (total + VALUE_BITS - 1) / (VALUE_BITS + 1);
    uvalue_t heap_size = total - bm_size;

    bitmap_start = p_addr;
    heap_start = bitmap_start + bm_size;

    list_init();
    uvalue_t *free = heap_start + HEADER_SIZE;
    free[-HEADER_SIZE] = header_pack(tag_None, (uvalue_t)(heap_size - HEADER_SIZE));
    list_prepend(FL_SIZE - 1, free);
}

uvalue_t memory_get_block_size(uvalue_t *block){
    return get_block_size(block);
}

tag_t memory_get_block_tag(uvalue_t *block){
    return get_block_tag(block);
}
