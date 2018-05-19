#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <stdbool.h>

#include "memory.h"
#include "fail.h"
#include "engine.h"

static uvalue_t* memory_start = NULL;
static uvalue_t* memory_end = NULL;

static uvalue_t* heap_start = NULL;
static uvalue_t* bitmap_start = NULL;

static uvalue_t* freelist = NULL;

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
    return (size << (uint8_t) 8) | (uvalue_t) tag;
}

static tag_t header_unpack_tag(uvalue_t header) {
    return (tag_t) (header & (uint8_t) 0xFF);
}

static uvalue_t header_unpack_size(uvalue_t header) {
    return header >> (uint8_t) 8;
}


/*************************************
 * BITMAP functions and macros
 *************************************/

/*#define BMB_GET_WORD(n) (bitmap_start[(size_t)((n) - heap_start) / VALUE_BITS])
#define BMB_GET_BIT_IDX(n) ((size_t)((n) - heap_start) % VALUE_BITS)

static inline void bm_set(const uvalue_t* p_addr) {
    BMB_GET_WORD(p_addr) |= (size_t) 1 << BMB_GET_BIT_IDX(p_addr);
}

static inline void bm_clear(const uvalue_t* v_addr) {
    BMB_GET_WORD(v_addr) &= ((size_t) 1 << BMB_GET_BIT_IDX(v_addr)) ^ ~((size_t) 1);
}

static inline int bm_is_set(const uvalue_t* p_addr) {
    return (BMB_GET_WORD(p_addr) >> BMB_GET_BIT_IDX(p_addr)) & (uint8_t) 1;
}*/
static void bm_set(uvalue_t* block) {
    size_t bytes = (size_t)(block - heap_start);
    size_t word = bytes / VALUE_BITS;
    size_t bit = bytes % VALUE_BITS;
    uvalue_t mask = ((uvalue_t) 1) << bit;
    bitmap_start[word] |= mask;
}

static void bm_clear(uvalue_t* block) {
    size_t bytes = (size_t)(block - heap_start);
    size_t word = bytes / VALUE_BITS;
    size_t bit = bytes % VALUE_BITS;
    uvalue_t mask = ~(((uvalue_t) 1) << bit);
    bitmap_start[word] &= mask;
}

static int bm_is_set(uvalue_t* block) {
    size_t bytes = (size_t)(block - heap_start);
    size_t word = bytes / VALUE_BITS;
    size_t bit = bytes % VALUE_BITS;
    uvalue_t mask = ((uvalue_t) 1) << bit;
    return (bitmap_start[word] & mask) != 0;
}

/*************************************
 * FREE LIST
 *************************************/

static inline uvalue_t* list_next(const uvalue_t* element) {
    assert(element != NULL);
    return addr_v_to_p(*element);
}

static inline void list_set_next(uvalue_t* element, uvalue_t* next) {
    if(element==NULL){
        freelist = next;
    }else {
        *element = addr_p_to_v(next);
    }
}

/**********************
 *  Marking
 **********************/

static void rec_mark(uvalue_t* root) {
    if (root > heap_start && root < memory_end && bm_is_set(root)) {
        bm_clear(root);
        uvalue_t blocksize = memory_get_block_size(root);
        for (uvalue_t i = 0; i < blocksize; ++i) {
            if (root[i] != 0 && (root[i] & (uvalue_t)3) == 0) {
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

static inline uvalue_t get_block_size(uvalue_t* block){
    assert((char*) memory_start < (char*) block && (char*) block <= (char*) memory_end);
    uvalue_t size = header_unpack_size(block[-1]);
    return size;
}


static void sweep() {

    uvalue_t* start_free = heap_start + HEADER_SIZE;

    uvalue_t* current = freelist = start_free;
    uvalue_t* list_last = current;
    
    while (current <= memory_end) {

        uvalue_t* block = current;
        uvalue_t block_size = get_block_size(block);

        if (bm_is_set(block)) {

            // not reachable --> free it
            bm_clear(block);
            memset(block, 0, block_size * sizeof(uvalue_t));

            if(start_free != current){
                // coalesce adjacent free blocks
                block = start_free;
                block_size = get_block_size(start_free) + HEADER_SIZE + block_size;
            }
            block[-1] = header_pack(tag_None, block_size);

            // update free list
            if(list_last != block){
                list_set_next(list_last, block);
            }

        } else {

            start_free += HEADER_SIZE + block_size;

        }

        current += HEADER_SIZE + block_size;
    }
}

/****************************
 * Blocks allocation
 ****************************/

static uvalue_t* block_allocate(tag_t tag, uvalue_t size) {
    assert(heap_start != NULL);
    assert(freelist != NULL);
    size = size < 1 ? 1 : size;

    uvalue_t* current = freelist;
    uvalue_t* prev = NULL;
    uvalue_t* best = NULL;
    uvalue_t* best_prev = NULL;
    uvalue_t best_size = ~((uvalue_t) 0);

    while (current < memory_end) {

        uvalue_t bsize = get_block_size(current);
        if (bsize == size) {
            best_prev = prev;
            best = current;
            break;
        } else if (bsize > size && bsize < best_size) {
            best_prev = prev;
            best = current;
            best_size = bsize;
        }

        prev = current;
        current = list_next(current);
    }

    if (best != NULL) {
        bm_set(best);

        if(size == best_size){
            // same size
            list_set_next(best_prev, list_next(best));
            best[-1] = header_pack(tag, size);
        }else if(size == best_size - 1){
            // to small to split
            list_set_next(best_prev, list_next(best));
            best[-1] = header_pack(tag, best_size);
        }else{
            best[-1] = header_pack(tag, size);
            
            // split block
            uvalue_t* new_block = best + size + HEADER_SIZE;
            if(new_block >= memory_end){
                return NULL;
            }
            new_block[-1] = header_pack(tag_None, best_size - size - HEADER_SIZE);
            list_set_next(new_block, list_next(best));
            list_set_next(best_prev, new_block);
        }
        
    }
    return best;
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
    freelist = NULL;
}

void* memory_get_start() {
    return memory_start;
}

void* memory_get_end() {
    return memory_end;
}

void memory_set_heap_start(void* _heap_start) {
    assert(_heap_start != NULL);
    assert(bitmap_start == NULL);

    uvalue_t heap_size = (uvalue_t) ((char*) memory_end - (char*) _heap_start) / sizeof(uvalue_t);
    size_t bm_size = (heap_size + VALUE_BITS - 1) / (VALUE_BITS + 1);

    bitmap_start = _heap_start;
    heap_start = bitmap_start + bm_size;

    freelist = heap_start + HEADER_SIZE;
    freelist[-HEADER_SIZE] = header_pack(tag_None, heap_size - HEADER_SIZE);
    list_set_next(freelist, memory_end);
}

uvalue_t memory_get_block_size(uvalue_t* block) {
    return header_unpack_size(block[-1]);
}

tag_t memory_get_block_tag(uvalue_t* block) {
    return header_unpack_tag(block[-1]);
}
