#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>

#include "my_malloc.h"
//to avoid error, set alignment to 8
#define ALIGNMENT 8
//parse size in bytes
#define ALIGN(size) (((size) + (ALIGNMENT-1)) & ~(ALIGNMENT-1))
//AND operation with 0, set block to free
#define SIZE(size) ((size) & ~0x1)
//size of header, align it
#define META_HEADER_ALIGNED (ALIGN(sizeof(metadata_t)))
//size of footer, align it, value of footer is address of header
#define META_FOOTER_ALIGNED (ALIGN(sizeof(metadata_t*)))
#define META_TOTAL_ALIGNED (META_HEADER_ALIGNED+META_FOOTER_ALIGNED)

typedef struct metadata {
  size_t size;
  struct metadata* prev;
  struct metadata* next;
} metadata_t;
//total bytes that alloc
static size_t total_bytes = 0;
//total free bytes
static size_t free_bytes = 0;
//header of freelist
static metadata_t* freelist = NULL;
//tail of freelist
static metadata_t* freelist_tail = NULL;

//first address of memory space
static void* first_valid_addr = NULL;
//last address of memory space
static void* last_valid_addr = NULL;


//header of freelist for nonlock
static __thread metadata_t * head = NULL;
static __thread metadata_t * tail = NULL;

pthread_mutex_t mutex;
pthread_mutex_t mutex1;
//convert to block format
//write header, footer. size does not include header & footer size.
metadata_t *init_block(void *ptr, size_t size) {
  metadata_t *header = (metadata_t*) ptr;
  metadata_t **footer = (metadata_t**) (ptr + META_HEADER_ALIGNED + size);
  header->size = size;
  //value of footer is header's address
  //in order to find previous block in memory space
  *footer = header;
  header->prev = NULL;
  header->next = NULL;

  //assert(*((metadata_t **)((void *)header + META_HEADER_ALIGNED + SIZE(header->size))) == header);
  return header;
}

int init() {
  //initialize freelist, first and last address of memory space
  //one for head, one for tail.
  size_t size = META_TOTAL_ALIGNED * 2;
  //pthread_mutex_lock(&mutex);
  //sbrk(0);
  void *ptr = sbrk(size);
  //pthread_mutex_unlock(&mutex);
  if (ptr == (void*) -1) {
    return 0;
  }
  //record total bytes alloc
  total_bytes = size + META_TOTAL_ALIGNED;
  //intialize head of freelist to point to a block which size is 0
  freelist = init_block(ptr, 0);
  freelist->size = 0x1;
  //intialize tail of freelist to point to a block which size is 0
  freelist_tail = init_block(ptr + META_TOTAL_ALIGNED, 0);
  freelist_tail->size = 0x1;

  first_valid_addr = ptr + size;
  last_valid_addr = first_valid_addr;

  freelist->next = freelist_tail;
  freelist_tail->prev = freelist;

  return 1;
}

metadata_t *new_block(size_t size) {
  //when create a new block
  //assert(sbrk(0) == last_valid_addr);

 
  size_t alloc_size = size + META_TOTAL_ALIGNED;
  //pthread_mutex_lock(&mutex);
  //  sbrk(0);
  void *ptr = sbrk(alloc_size);
  //pthread_mutex_lock(&mutex);
  if (ptr == (void*) -1) {
    return NULL;
  }
  //record total bytes alloc
  total_bytes += alloc_size;
  last_valid_addr = ptr + alloc_size;
  //convert block format
  return init_block(ptr, size);
}

metadata_t *use_block(metadata_t *block) {
  //delete block from freelist
  assert((block->size & 0x1) == 0);
  metadata_t *prev = block->prev;
  metadata_t *next = block->next;
  prev->next = next;
  next->prev = prev;
  block->size |= 0x1;
  return block;
}
metadata_t *recycle_block(metadata_t *block) {
  //add block to freelist
  //set block free
  block->size &= ~0x1;
  metadata_t * curr=freelist;
  while(curr->next  != freelist_tail){
    if((unsigned long)curr <(unsigned long) block && (unsigned long)block > (unsigned long)curr->next){
      break;
    }
    curr=curr->next;
  }
  metadata_t *prevB = curr;
  metadata_t *nextB = curr->next;
  prevB->next = block;
  block->prev = prevB;
  nextB->prev = block;
  block->next = nextB;
  return block;
}

// size is the size needed.
metadata_t *split(metadata_t *block, size_t size) {
  if (SIZE(block->size) <= size + META_TOTAL_ALIGNED) return block;
  //mark the free space that the split block can use
  metadata_t *free_space = init_block((void*)block + size + META_TOTAL_ALIGNED, SIZE(block->size) - size - META_TOTAL_ALIGNED);
  //add to freelist
  recycle_block(free_space);
  return init_block(block, size);
}

metadata_t *get_best_free_block(size_t size) {
  //best fit logic
  //track the best fitted block
  metadata_t *curr = freelist->next;
  metadata_t *best = NULL;
  while (curr != NULL) {
    if (SIZE(curr->size) >= size && (best == NULL || curr->size < best->size)) {
      best = curr;
    }
    curr = curr->next;
  }
  if (best != NULL) {
    //if found, split it and return
    return split(use_block(best), size);
  }
  //if not found, alloc a new block and return it
  return new_block(size);
}


void *ts_malloc_lock(size_t size){
  //best fit malloc
  //align the size
  pthread_mutex_lock(&mutex);
  size = ALIGN(size);
  if (freelist == NULL) {
    if (!init()) return NULL;
  }
  //use
  metadata_t *block = get_best_free_block(size);
  if (block == NULL) {
    return NULL;
  }
  block->size |= 0x1;
  pthread_mutex_unlock(&mutex);
  //assert(*((metadata_t **)((void *)block + META_HEADER_ALIGNED + SIZE(block->size))) == block);
  return (void *)block + META_HEADER_ALIGNED;
}

void ts_free_lock(void *ptr){
  //best fit free
  //ff_free(ptr);
  //find the header of that block
  pthread_mutex_lock(&mutex);
  metadata_t *header = (metadata_t *)(ptr - META_HEADER_ALIGNED);
  if (*((metadata_t **)(ptr + SIZE(header->size))) != header) {
    perror("Error! Free a wild pointer");
    return;
  }
  //assert(header->size & 0x1 == 1);
  //find the previous block
  void* prev_ptr = ptr - META_TOTAL_ALIGNED;
  if (prev_ptr > first_valid_addr) {
    metadata_t *prev = *((metadata_t **)prev_ptr);
    //assert(*((metadata_t **)((void *)prev + META_HEADER_ALIGNED + SIZE(prev->size))) == prev);

   
    if (prev->size & 0x1 == 0) {
      //if can merge previous and target blocks
      use_block(prev);
      header = init_block(prev, SIZE(prev->size) + SIZE(header->size) + META_TOTAL_ALIGNED);
    }
  }
  //find the next block of target
  void* next_ptr = (void *)header + SIZE(header->size) + META_TOTAL_ALIGNED;
  if (next_ptr < last_valid_addr) {
    metadata_t *next = (metadata_t *)next_ptr;
    assert(*((metadata_t **)((void *)next + META_HEADER_ALIGNED + SIZE(next->size))) == next);
    if (next->size & 0x1 == 0) {
      //if can merge next and target blocks
      use_block(next);
      header = init_block(header, SIZE(header->size) + SIZE(next->size) + META_TOTAL_ALIGNED);
    }
  }
  //put the merged block to freelist
  recycle_block(header);
  pthread_mutex_unlock(&mutex);
  
}



int init_nolock(){
  //initialize freelist, first and last address of memory space
  //one for head, one for tail.
  size_t size = META_TOTAL_ALIGNED * 2;
  pthread_mutex_lock(&mutex1);
  //  printf("when initialize\n");
  void *ptr = sbrk(size);
  pthread_mutex_unlock(&mutex1);
  if (ptr == (void*) -1) {
    return 0;
  }
  
  //intialize head of freelist to point to a block which size is 0
  head = init_block(ptr, 0);
  head->size = 0x1;
  //intialize tail of freelist to point to a block which size is 0
  tail = init_block(ptr + META_TOTAL_ALIGNED, 0);
  tail->size = 0x1;

  
 

  head->next = tail;
  tail->prev = head;
  // printf("after initialize call sbrk\n");
  return 1;
  
}
metadata_t *new_block_nolock(size_t size) {
  //when create a new block
  //assert(sbrk(0) == last_valid_addr);


  size_t alloc_size = size + META_TOTAL_ALIGNED;

  pthread_mutex_lock(&mutex1);
  // printf("sbrk a new block\n");
  void *ptr = sbrk(alloc_size);
  pthread_mutex_unlock(&mutex1);

  if (ptr == (void*) -1) {
    return NULL;
  }
  //record total bytes alloc
  
  
  //convert block format
  return init_block(ptr, size);
}


metadata_t *use_block_nolock(metadata_t *block) {
  //delete block from freelist
  assert((block->size & 0x1) == 0);
  if(head==tail){
    head=NULL;
    tail=NULL;
    block->next=NULL;
    block->prev=NULL;
    block->size |= 0x1;
    return block;
  }

  if(block==head ){
    head=head->next;
    head->prev=NULL;
    block->next=NULL;
    block->size |= 0x1;
    return block;
  }
  if(block==tail){
    tail=tail->prev;
    tail->next=NULL;
    block->prev=NULL;
    block->next=NULL;
    block->size |= 0x1;
    return block;
  }
  metadata_t *prev = block->prev;
  metadata_t *next = block->next;
  prev->next = next;
  next->prev = prev;
  block->size |= 0x1;
  return block;
}
metadata_t *recycle_block_nolock(metadata_t *block) {
  //add block to freelist
  //set block free
  
  block->size &= ~0x1;

  if(head==NULL){
    head=block;
    tail=head;
    return head;
  }
  if(head==tail){
    if((unsigned long)block<(unsigned long)head){
      head->prev=block;
      block->next=head;
      head=block;
      return head;
    }
    block->prev=tail;
    tail->next=block;
    tail=block;
    return head;
  }

  if((unsigned long) block<(unsigned long)head){
    head->prev=block;
    block->next=head;
    head=block;
    return head;
  }
  if((unsigned long)block>(unsigned long) tail){
    tail->next=block;
    block->prev=tail;
    tail=block;
    return head;
  }
  //insert between head and tail
  metadata_t * curr = head;
  while(curr->next != tail){

    if((unsigned long)curr < (unsigned long)block && (unsigned long)(curr->next) > (unsigned long)block){
      break;
    }

    curr=curr->next;
  }

  metadata_t * prevB = curr;
  metadata_t * nextB = curr->next;
  prevB->next=block;
  block->prev=prevB;
  nextB->prev=block;
  block->next=nextB;

  return block;
 
 
}


metadata_t *split_nolock(metadata_t *block, size_t size) {
  if (SIZE(block->size) <= size + META_TOTAL_ALIGNED) return block;
  //mark the free space that the split block can use
  metadata_t *free_space = init_block((void*)block + size + META_TOTAL_ALIGNED, SIZE(block->size) - size - META_TOTAL_ALIGNED);
  //add to freelist
  recycle_block_nolock(free_space);
  return init_block(block, size);
}


metadata_t *get_best_free_block_nolock(size_t size) {
  //best fit logic
  //track the best fitted block
  if(head==NULL){
    return new_block_nolock(size);
  }
  metadata_t *curr = head;
  metadata_t *best = NULL;
  while (curr != NULL) {
    if (SIZE(curr->size) >= size && (best == NULL || curr->size < best->size)) {
      best = curr;
    }
    curr = curr->next;
  }
  if (best != NULL) {
    //if found, split it and return
    return split_nolock(use_block_nolock(best), size);
  }
  //if not found, alloc a new block and return it
  return new_block_nolock(size);
}


void *ts_malloc_nolock(size_t size){
  //best fit malloc
  //align the size

  size = ALIGN(size);
  // if (head == NULL) {
  // init_nolock();
  // }
 
  metadata_t *block = get_best_free_block_nolock(size);
  if (block == NULL) {
    return NULL;
  }
  block->size |= 0x1;

  //assert(*((metadata_t **)((void *)block + META_HEADER_ALIGNED + SIZE(block->size))) == block);


  return (void *)block + META_HEADER_ALIGNED;
}



void ts_free_nolock(void *ptr){

  metadata_t *header = (metadata_t *)(ptr - META_HEADER_ALIGNED);


  //put the free block to freelist
  recycle_block_nolock(header);



}
