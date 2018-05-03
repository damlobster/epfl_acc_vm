#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>

#include "memory.h"
#include "fail.h"

static uvalue_t* memory_start = NULL;
static uvalue_t* memory_end = NULL;
static uvalue_t* free_boundary = NULL;

#define HEADER_SIZE 1

// Header management

static uvalue_t header_pack(tag_t tag, uvalue_t size) {
  return (size << 8) | (uvalue_t)tag;
}

static tag_t header_unpack_tag(uvalue_t header) {
  return (tag_t)(header & 0xFF);
}

static uvalue_t header_unpack_size(uvalue_t header) {
  return header >> 8;
}

char* memory_get_identity() {
  return "no GC (memory is never freed)";
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
  memory_start = memory_end = free_boundary = NULL;
}

void* memory_get_start() {
  return memory_start;
}

void* memory_get_end() {
  return memory_end;
}

void memory_set_heap_start(void* heap_start) {
  assert(free_boundary == NULL);
  free_boundary = heap_start;
}

uvalue_t* memory_allocate(tag_t tag, uvalue_t size) {
  assert(free_boundary != NULL);

  const uvalue_t total_size = size + HEADER_SIZE;
  if (free_boundary + total_size > memory_end)
    fail("no memory left (block of size %u requested)", size);

  *free_boundary = header_pack(tag, size);
  uvalue_t* res = free_boundary + HEADER_SIZE;
  free_boundary += total_size;
  int* a = 0;
  *a += 1;
  return res;
}

uvalue_t memory_get_block_size(uvalue_t* block) {
  return header_unpack_size(block[-1]);
}

tag_t memory_get_block_tag(uvalue_t* block) {
  return header_unpack_tag(block[-1]);
}
