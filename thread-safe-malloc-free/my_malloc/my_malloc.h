#ifndef head_malloc_h
#define head_malloc_h


#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

//Thread Safe malloc/free: locking version
void *ts_malloc_lock(size_t size);
void ts_free_lock(void *ptr);
//Thread Safe malloc/free: non-locking version
void *ts_malloc_nolock(size_t size);
void ts_free_nolock(void *ptr);

//void *ff_malloc(size_t size);
//void ff_free(void *ptr);
//void *bf_malloc(size_t size);
//void bf_free(void *ptr);

//unsigned long get_data_segment_size(); //in bytes
//unsigned long get_data_segment_free_space_size(); //in bytes

#endif /* head_malloc_h */
