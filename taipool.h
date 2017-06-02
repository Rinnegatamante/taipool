#ifndef _TAIPOOL_H_
#define _TAIPOOL_H_

/* 
 *  Initialize a mempool
 *  NOTE: mempool is NOT allocated on Heap
 */
int taipool_init(size_t size);

/*
 *  Terminate already initialized mempool
 */
void taipool_term(void);

/*
 *  Returns currently available free space on mempool
 */
size_t taipool_get_free_space(void);

/*
 *  Resets mempool
 */
void taipool_reset(void);

/*
 *  Allocate a memory block on mempool
 */
void* taipool_alloc(size_t size);

#endif