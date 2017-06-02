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
	uint8_t* pool_ptr = (uint8_t*)mempool_addr;
	
	// Checking for a big enough free memblock
	while (i < mempool_size){
		if (!hdr->used){
			if (hdr->size >= size){
				
				// Reserving memory
				hdr->used = 1;
				uint32_t old_size = hdr->size;
				hdr->size = size;
				
				// Splitting blocks
				mempool_block_hdr* new_hdr = (mempool_block_hdr*)&pool_ptr[i + sizeof(mempool_block_hdr) + size];
				new_hdr->used = 0;
				new_hdr->size = old_size - size - sizeof(mempool_block_hdr);
				
				mempool_free -= (sizeof(mempool_block_hdr) + size);
				return (void*)(mempool_addr + i + sizeof(mempool_block_hdr));
			}
		}
		
		// Jumping to next block
		i += hdr->size + sizeof(mempool_block_hdr);
		pool_ptr = (uint8_t*)(mempool_addr + i);
		hdr = (mempool_block_hdr*)pool_ptr;
		
	}
	return NULL;
}

// Frees a block on mempool
static void _taipool_free_block(void* ptr){
	mempool_block_hdr* hdr = (mempool_block_hdr*)(ptr - sizeof(mempool_block_hdr));
	hdr->used = 0;
	mempool_free += hdr->size;
}

// Merge contiguous blocks in a bigger one
static void _taipool_merge_blocks(){
	int i = 0;
	mempool_block_hdr* hdr = (mempool_block_hdr*)mempool_addr;
	uint8_t* pool_ptr = (uint8_t*)mempool_addr;
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
		pool_ptr = (uint8_t*)(mempool_addr + i);
		hdr = (mempool_block_hdr*)pool_ptr;
		
	}
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
	
	// Creating a thread in order to reserving requested memory
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

// Allocates a new block on taipool mempool
void* taipool_alloc(size_t size){
	if (size >= mempool_free) return _taipool_alloc_block(size);
	else return NULL;
}

// Frees a block on taipool mempool
void taipool_free(void* ptr){
	_taipool_free_block(ptr);
	_taipool_merge_blocks();
}

// Returns currently free space on mempool
size_t taipool_get_free_space(void){
	return mempool_free;
}