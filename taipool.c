#include <vitasdk.h>
#include "taipool.h"

static int dummy_thread(SceSize args, void *argp){return 0;}
static void* mempool_addr = NULL;
static SceUID mempool_id = 0;
static uint32_t mempool_size = 0;
static uint32_t mempool_free = 0;

// memblock header struct
typedef struct mempool_block_hdr{
	uint8_t used;
	uint32_t size;
} mempool_block_hdr;

// Allocks a new block on mempool
static void* _taipool_alloc_block(size_t size){
	int i = 0;
	mempool_block_hdr* hdr = (mempool_block_hdr*)mempool_addr;
	
	// Checking for a big enough free memblock
	while (i < mempool_size){
		if (!hdr->used){
			if (hdr->size >= size){
				
				// Reserving memory
				hdr->used = 1;
				uint32_t old_size = hdr->size;
				hdr->size = size;
				
				// Splitting blocks
				mempool_block_hdr* new_hdr = (mempool_block_hdr*)(mempool_addr + i + sizeof(mempool_block_hdr) + size);
				new_hdr->used = 0;
				new_hdr->size = old_size - size - sizeof(mempool_block_hdr);
				
				mempool_free -= (sizeof(mempool_block_hdr) + size);
				return (void*)(mempool_addr + i + sizeof(mempool_block_hdr));
			}
		}
		
		// Jumping to next block
		i += hdr->size + sizeof(mempool_block_hdr);
		hdr = (mempool_block_hdr*)(mempool_addr + i);
		
	}
	return NULL;
}

// Frees a block on mempool
static void _taipool_free_block(void* ptr){
	mempool_block_hdr* hdr = (mempool_block_hdr*)(ptr - sizeof(mempool_block_hdr));
	hdr->used = 0;
	mempool_free += hdr->size;
}

// Merge contiguous free blocks in a bigger one
static void _taipool_merge_blocks(){
	int i = 0;
	mempool_block_hdr* hdr = (mempool_block_hdr*)mempool_addr;
	mempool_block_hdr* previousBlock = NULL;
	
	while (i < mempool_size){
		if (!hdr->used){
			if (previousBlock != NULL){
				previousBlock->size += hdr->size + sizeof(mempool_block_hdr);
				mempool_free += sizeof(mempool_block_hdr); 
			}else{
				previousBlock = hdr;
			}
		}else{
			previousBlock = NULL;
		}
		
		// Jumping to next block
		i += hdr->size + sizeof(mempool_block_hdr);
		hdr = (mempool_block_hdr*)(mempool_addr + i);
		
	}
}

// Extend an allocated block by a given size
static void* _taipool_extend_block(void* ptr, size_t size){
	mempool_block_hdr* hdr = (mempool_block_hdr*)(ptr - sizeof(mempool_block_hdr));
	mempool_block_hdr* next_block = (mempool_block_hdr*)(ptr + hdr->size);
	size_t extra_size = size - hdr->size;
	
	// Checking if enough contiguous blocks are available
	while (extra_size > 0){
		if (next_block->used) return NULL;
		extra_size -= (next_block->size + sizeof(mempool_block_hdr));
		next_block = (mempool_block_hdr*)(next_block + sizeof(mempool_block_hdr) + next_block->size);
	}
	
	// Extending current block
	hdr->size = size;
	mempool_free -= extra_size;
	
	return ptr;
}

// Compact an allocated block to a given size
static void _taipool_compact_block(void* ptr, size_t size){
	mempool_block_hdr* hdr = (mempool_block_hdr*)(ptr - sizeof(mempool_block_hdr));
	size_t old_size = hdr->size;
	hdr->size = size;
	mempool_block_hdr* new_block = (mempool_block_hdr*)(ptr + hdr->size);
	new_block->used = 0;
	new_block->size = old_size - size - sizeof(mempool_block_hdr);
	mempool_free += new_block->size;
}

// Resets taipool mempool
void taipool_reset(void){
	mempool_block_hdr* master_block = (mempool_block_hdr*)mempool_addr;
	master_block->used = 0;
	master_block->size = mempool_size - sizeof(mempool_block_hdr);
	mempool_free = master_block->size;
}

// Terminate taipool mempool
void taipool_term(void){
	if (mempool_addr != NULL){
		sceKernelDeleteThread(mempool_id);
		mempool_addr = NULL;
		mempool_size = 0;
		mempool_free = 0;
	}
}

// Initialize taipool mempool
int taipool_init(size_t size){
	
	if (mempool_addr != NULL) taipool_term();
	
	// Creating a thread in order to reserve requested memory
	SceUID pool_id = sceKernelCreateThread("mempool_thread", dummy_thread, 0x40, size, 0, 0, NULL);
	if (pool_id >= 0){
		SceKernelThreadInfo mempool_info;
		mempool_info.size = sizeof(SceKernelThreadInfo);
		sceKernelGetThreadInfo(pool_id, &mempool_info);
		mempool_addr = mempool_info.stack;
		mempool_id = pool_id;
		mempool_size = size;
		
		// Initializing mempool as a single block
		taipool_reset();
		
		return 0;
	}
	return pool_id;
}

// Frees a block on taipool mempool
void taipool_free(void* ptr){
	_taipool_free_block(ptr);
	_taipool_merge_blocks();
}

// Allocates a new block on taipool mempool
void* taipool_alloc(size_t size){
	if (size >= mempool_free) return _taipool_alloc_block(size);
	else return NULL;
}

// Allocates a new block on taipool mempool and zero-initialize it
void* taipool_calloc(size_t num, size_t size){
	void* res = taipool_alloc(num * size);
	if (res != NULL) memset(res, 0, num * size);
	return res;
}

// Reallocates a currently allocated block on taipool mempool
void* taipool_realloc(void* ptr, size_t size){
	mempool_block_hdr* hdr = (mempool_block_hdr*)(ptr - sizeof(mempool_block_hdr));
	void* res = NULL;
	if (hdr->size < size){ // Increasing size
	
		// Trying to extend the block with contiguous blocks (right side)
		void* res = _taipool_extend_block(ptr, size);
		if (res == NULL){
		
			// Trying to extend the block by fully relocating it
			res = taipool_alloc(size);
			if (res != NULL){
				memcpy(res, ptr, hdr->size);
				taipool_free(ptr);
			}else{
			
				// Trying to extend the block with contiguous blocks
				size_t orig_size = hdr->size;
				taipool_free(ptr);
				res = taipool_alloc(size);
				if (res == NULL){
					hdr->used = 1;
					hdr->size = orig_size;
				}else if (res != ptr){
					memmove(res, ptr, orig_size);
				}
				
			}
		}
	}else{ // Reducing size
		_taipool_compact_block(ptr, size);
		_taipool_merge_blocks();
		return ptr;
	}
	return res;
}

// Returns currently free space on mempool
size_t taipool_get_free_space(void){
	return mempool_free;
}