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
#define debug(fmt, ...) do {} while(0)

#define HEADER_SIZE 1

static uvalue_t* memory_start = NULL;
static uvalue_t* memory_end = NULL;

static uvalue_t* heap_start = NULL;
static uvalue_t* bitmap_start = NULL;

// I use a "bitmap" to mark the non empty free lists (see list_find())
#define FL_SIZE 512
static uvalue_t FL_mbm = 0;
static uvalue_t FL_bm[FL_SIZE / VALUE_BITS] = {0};
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
    uvalue_t size = header >> (uint8_t) 8;
    return size;
}

static inline uvalue_t get_block_size(uvalue_t* block){
    uvalue_t size = header_unpack_size(block[-1]);
    return size;
}

static inline uvalue_t real_size(uvalue_t size){
    return size == 0 ? 1 : size;
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
    for(size_t i = 0; i < FL_SIZE; i++){
        FL[i] = memory_start;
        if(i % VALUE_BITS == 0){FL_bm[i / FL_SIZE] = 0;}
    }
    FL_mbm = 0;
}

static inline uvalue_t* list_next(const uvalue_t* element) {
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

static inline int list_idx(uvalue_t size){
    int idx = (int)real_size(size) - 1;
    return idx < FL_SIZE ? idx : FL_SIZE - 1;
}

static inline void list_prepend(int idx, uvalue_t* element) {
    // if the freelist was empty, mark it non empty
    int midx = idx / (int)VALUE_BITS;
    debug("PRE idx=%d, midx=%d (x%x", idx, midx, FL_mbm);

    //assert(element != memory_start);
    //assert(FL[idx] == memory_start);

    FL_mbm |= 1U << midx;
    debug("->x%x), FL_bm[%d]=(x%x->", FL_mbm, midx, FL_bm[midx]);
    FL_bm[midx] |= 1U << ((uvalue_t)idx % VALUE_BITS);
    debug("x%x)\n", FL_bm[midx]);
    element[0] = addr_p_to_v(FL[idx]);
    FL[idx] = element;
}

static inline void list_remove_head(int idx){
    assert(FL[idx] > memory_start);
    FL[idx] = list_next(FL[idx]);

    // Update freelists bitmap
    int midx = idx / (int)VALUE_BITS;
        debug("DEL idx=%d, midx=%d (x%x", idx, midx, FL_mbm);
    if(FL_bm[midx] == 0U){
        FL_mbm &= ~0U ^ (1U << midx);
    }
        debug("->x%x), FL_bm[%d]=(x%x->", FL_mbm, midx, FL_bm[midx]);
    FL_bm[midx] &= ~(1U << ((uvalue_t)idx % VALUE_BITS));
        debug("x%x)\n", FL_bm[midx]);
}

static inline int _list_find(uvalue_t size){
        debug("_list_find(%u) (FL_mbm=x%x)\n", size, FL_mbm);
    if(size >= FL_SIZE) return -1;
    int idx = list_idx(size);
    int midx = idx / (int)VALUE_BITS;
    
        debug("  idx=%d, midx=%u\n", idx, midx);

    uvalue_t mask = ~0U << midx;
    uvalue_t bits = FL_mbm & mask;
    
        debug("  mmask=x%x, mbits=x%x\n", mask, bits);

    // if master bm is 0 -> all free list >= size are empty
    if(bits == 0){
        return -1;
    }
    
    midx = __builtin_ctzl(bits);
        debug("  midx=%d\n", midx);
    // start searching from the freelist containing blocks of same size 
    idx = (uvalue_t) idx % VALUE_BITS;
        debug("  idx%%32=%d\n", idx);

    mask = ~0U << (idx);
        debug("  mask=x%x\n", mask);
    if(idx != VALUE_BITS - 1){
        // skip the freelist of size = size+1
        mask ^= 1U << (idx + 1);
    }
    
    bits = FL_bm[midx] & mask;
        debug("  FL_bm[%d]=x%x, mask=x%x, bits=x%x\n", midx, FL_bm[midx], mask, bits);
    // if bm is 0 -> all free list >= size are empty
    if(bits == 0){
        return _list_find(((uvalue_t)(midx+1) * VALUE_BITS + 1));
    }

    idx = __builtin_ctzl(bits);
        debug("  idx=%d\n", midx * (int)VALUE_BITS + idx);
    return midx * (int)VALUE_BITS + idx;
}

static inline int list_find(uvalue_t size){
    int idx = _list_find(size);
    debug("%d\n", idx);
/*     if((idx == -1) && ((size-1) % VALUE_BITS == VALUE_BITS - 1)){
        idx = _list_find(size + 2);
        debug("...%d\n", idx);
    }*/    
    return idx;
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

static inline void mark() {

    debug("GC !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n", NULL);

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

static inline void sweep() {
    debug("FL_mbm = %x (before sw)\n", FL_mbm);

    list_init();

    uvalue_t* start_free = heap_start + HEADER_SIZE;
    uvalue_t* current = start_free;
    int last_list = -1;
    
    while (current <= memory_end) {

        uvalue_t block_size = real_size(get_block_size(current)); 

        if (bm_is_set(current)) {
            // block is not reachable --> free it
            bm_clear(current);
            memset(current, 0, block_size * sizeof(uvalue_t));
            current[-HEADER_SIZE] = header_pack(tag_None, block_size);
        }
        
        if (get_block_tag(current) == tag_None) {
            
            // coalesce adjacent free blocks
            if(start_free < current){
                current[-HEADER_SIZE] = 0;
                current[0] = 0; 
                
                // rewind current pointer to start of coalesced block
                current = start_free; 
                
                // update size of new free block
                block_size += get_block_size(start_free) + HEADER_SIZE;
                current[-HEADER_SIZE] = header_pack(tag_None, block_size);
            }

            // update free lists
            int idx = list_idx(block_size);
            if(idx != last_list){
                if(last_list != -1){
                    list_remove_head(last_list);
                }
                list_prepend(idx, current);
                last_list = idx;
            }
        } else { // the block is not free
            // point start_free on next block
            start_free = current + block_size + HEADER_SIZE;
            bm_set(current);
            last_list = -1;

            #ifdef GC_STATS
            live_count++;
            #endif
        }

        // move to next block
        current += block_size + HEADER_SIZE;
    }

    debug("FL_mbm = %x (after sw)\n", FL_mbm);
}

/****************************
 * Blocks allocation
 ****************************/

// I choosed to use the first fit strategy
static inline uvalue_t* block_allocate(tag_t tag, uvalue_t size) {    
    uvalue_t* block = NULL;
    uvalue_t* prev = NULL;
    uvalue_t realsize = real_size(size);

    int idx = list_find(realsize);
    if(idx < 0) return NULL;

    prev = NULL;
    block = FL[idx];

    if(block == memory_start){
        debug("BA empty list\n", NULL);
    }
    while(block != memory_start){
        uvalue_t* new_free = NULL;
        uvalue_t total_size = get_block_size(block);

        if(realsize <= total_size){

            // we found a candidate -> remove it from old free list
            if(prev == NULL){
                list_remove_head(idx);
            }else{
                list_remove_next(prev);
            }

            if(realsize < total_size){
                // the allocated block is smaller -> split it
                new_free = block + realsize + HEADER_SIZE;
                uvalue_t new_free_size = total_size - realsize - HEADER_SIZE;
                new_free[-HEADER_SIZE] = header_pack(tag_None, new_free_size);

                debug("SW nfreesize=%u\n", new_free_size);
                list_prepend(list_idx(new_free_size), new_free);
            }
            
            // initilize the new block
            bm_set(block);
            block[-HEADER_SIZE] = header_pack(tag, size);
            block[0] = 0;
            
            return block;
        }
        
        // if we are here, we are in last free list
        // -> go to next block
        prev = block;
        block = list_next(block);
    }
    
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
