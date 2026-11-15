#include <vitasdk.h>
#include "taipool.h"
#include <string.h>

#define POOL_PADDING 0x100 // Difference between stack pointer and mempool start in bytes
#define TAIPOOL_ALIGNMENT 8

static int dummy_thread(SceSize args, void *argp){return 0;}
static void* mempool_addr = NULL;
static SceUID mempool_id = 0;
static size_t mempool_size = 0;
static size_t mempool_free = 0;
static SceUID taipool_sema = 0;

// memblock header struct
typedef struct mempool_block_hdr {
	uint8_t used;
	size_t size;
} mempool_block_hdr;

static int _taipool_align_size(size_t size, size_t* aligned_size) {
	if (size > SIZE_MAX - (TAIPOOL_ALIGNMENT - 1)) return -1;
	*aligned_size = (size + TAIPOOL_ALIGNMENT - 1) & ~(size_t)(TAIPOOL_ALIGNMENT - 1);
	return 0;
}

// Locks taipool mempool access (blocking)
static void _taipool_lock_access() {
	sceKernelWaitSema(taipool_sema, 1, NULL);
}

// Unlocks taipool mempool access
static void _taipool_unlock_access() {
	sceKernelSignalSema(taipool_sema, 1);
}

// Allocks a new block on mempool
static void* _taipool_alloc_block(size_t size) {
	size_t i = 0;
	mempool_block_hdr* hdr = (mempool_block_hdr*)mempool_addr;
	
	// Always align to 8 bytes
	size = (size + 7) & ~(size_t)7;
	
	// Checking for a big enough free memblock
	while (i < mempool_size) {
		if (!hdr->used) {
			if (hdr->size >= size) {
				
				// Reserving memory
				hdr->used = 1;
				size_t old_size = hdr->size;
				
				// Splitting blocks
				if (old_size - size >= sizeof(mempool_block_hdr) + 8) {
					hdr->size = size;
					mempool_block_hdr* new_hdr = (mempool_block_hdr*)(mempool_addr + i + sizeof(mempool_block_hdr) + size);
					new_hdr->used = 0;
					new_hdr->size = old_size - (size + sizeof(mempool_block_hdr));
					mempool_free -= (sizeof(mempool_block_hdr) + size);
				} else {
					mempool_free -= old_size;
				}
				
				return (void*)(mempool_addr + i + sizeof(mempool_block_hdr));
			}
		}
		
		// Jumping to next block
		i += (hdr->size + sizeof(mempool_block_hdr));
		hdr = (mempool_block_hdr*)(mempool_addr + i);
		
	}
	return NULL;
}

// Frees a block on mempool
static void _taipool_free_block(void* ptr) {
	mempool_block_hdr* hdr = (mempool_block_hdr*)(ptr - sizeof(mempool_block_hdr));
	hdr->used = 0;
	mempool_free += hdr->size;
}

// Merge contiguous free blocks in a bigger one
static void _taipool_merge_blocks() {
	size_t i = 0;
	mempool_block_hdr* hdr = (mempool_block_hdr*)mempool_addr;
	mempool_block_hdr* previousBlock = NULL;
	
	while (i < mempool_size) {
		if (!hdr->used) {
			if (previousBlock != NULL) {
				previousBlock->size += (hdr->size + sizeof(mempool_block_hdr));
				mempool_free += sizeof(mempool_block_hdr); 
			} else {
				previousBlock = hdr;
			}
		} else {
			previousBlock = NULL;
		}
		
		// Jumping to next block
		i += hdr->size + sizeof(mempool_block_hdr);
		hdr = (mempool_block_hdr*)(mempool_addr + i);
		
	}
}

// Extend an allocated block by a given size
static void* _taipool_extend_block(void* ptr, size_t size) {
	uint8_t* block_end;
	uint8_t* pool_end = (uint8_t*)mempool_addr + mempool_size;
	mempool_block_hdr* hdr = (mempool_block_hdr*)((uint8_t*)ptr - sizeof(mempool_block_hdr));
	mempool_block_hdr* next_block;
	size_t combined_size;
	size_t remaining;

	block_end = (uint8_t*)ptr + hdr->size;
	if (block_end >= pool_end || (size_t)(pool_end - block_end) < sizeof(mempool_block_hdr)) return NULL;

	next_block = (mempool_block_hdr*)block_end;
	if (next_block->used) return NULL;
	if (next_block->size > (size_t)(pool_end - block_end) - sizeof(mempool_block_hdr)) return NULL;

	combined_size = hdr->size + sizeof(mempool_block_hdr) + next_block->size;
	if (combined_size < size) return NULL;

	remaining = combined_size - size;
	if (remaining >= sizeof(mempool_block_hdr) + TAIPOOL_ALIGNMENT) {
		mempool_block_hdr* new_block;
		size_t old_size = hdr->size;
		hdr->size = size;
		new_block = (mempool_block_hdr*)((uint8_t*)ptr + size);
		new_block->used = 0;
		new_block->size = remaining - sizeof(mempool_block_hdr);
		mempool_free -= size - old_size;
	} else {
		hdr->size = combined_size;
		mempool_free -= next_block->size;
	}
	
	return ptr;
}

// Compact an allocated block to a given size
static void _taipool_compact_block(void* ptr, size_t size) {
	mempool_block_hdr* hdr = (mempool_block_hdr*)((uint8_t*)ptr - sizeof(mempool_block_hdr));
	size_t old_size = hdr->size;
	size_t remaining;
	mempool_block_hdr* new_block;

	if (size >= old_size) return;
	remaining = old_size - size;
	if (remaining < sizeof(mempool_block_hdr) + TAIPOOL_ALIGNMENT) return;

	hdr->size = size;
	new_block = (mempool_block_hdr*)((uint8_t*)ptr + hdr->size);
	new_block->used = 0;
	new_block->size = remaining - sizeof(mempool_block_hdr);
	mempool_free += new_block->size;
}

// Resets taipool mempool
void taipool_reset(void) {
	if (mempool_addr != NULL) {
		_taipool_lock_access();
		mempool_block_hdr* master_block = (mempool_block_hdr*)mempool_addr;
		master_block->used = 0;
		master_block->size = mempool_size - sizeof(mempool_block_hdr);
		mempool_free = master_block->size;
		_taipool_unlock_access();
	}
}

// Terminate taipool mempool
void taipool_term(void) {
	if (mempool_addr != NULL) {
		_taipool_lock_access();
		sceKernelDeleteThread(mempool_id);
		sceKernelDeleteSema(taipool_sema);
		mempool_addr = NULL;
		mempool_size = 0;
		mempool_free = 0;
	}
}

// Initialize taipool mempool
int taipool_init(size_t size) {
	
	if (mempool_addr != NULL) {
		taipool_term();
	}
	
	// Creating a thread in order to reserve requested memory
	SceUID pool_id = sceKernelCreateThread("mempool_thread", dummy_thread, 0x40, size, 0, 0, NULL);
	if (pool_id >= 0) {
		SceKernelThreadInfo mempool_info;
		mempool_info.size = sizeof(SceKernelThreadInfo);
		if (!sceKernelGetThreadInfo(pool_id, &mempool_info)) {
			mempool_addr = mempool_info.stack + POOL_PADDING;
			mempool_id = pool_id;
			mempool_size = mempool_info.stackSize - POOL_PADDING;
			
			// Setting up a semaphore for mutual exclusion
			taipool_sema = sceKernelCreateSema("taipool_sema", 0, 1, 1, 0);
			if (taipool_sema < 0) {
				SceUID error = taipool_sema;
				sceKernelDeleteThread(mempool_id);
				mempool_addr = NULL;
				mempool_id = 0;
				mempool_size = 0;
				mempool_free = 0;
				taipool_sema = 0;
				return error;
			}

			// Initializing mempool as a single block
			taipool_reset();
			
			return 0;
		}
	}
	return pool_id;
}

// Frees a block on taipool mempool
void taipool_free(void* ptr) {
	_taipool_lock_access();
	_taipool_free_block(ptr);
	_taipool_merge_blocks();
	_taipool_unlock_access();
}

// Allocates a new block on taipool mempool
void* taipool_alloc(size_t size) {
	_taipool_lock_access();
	void* res = NULL;
	if (size <= mempool_free) res = _taipool_alloc_block(size);
	_taipool_unlock_access();
	return res;
}

// Allocates a new block on taipool mempool and zero-initialize it
void* taipool_calloc(size_t num, size_t size) {
	size_t total;
	void* res;

	if (size != 0 && num > SIZE_MAX / size) return NULL;
	total = num * size;
	res = taipool_alloc(total);
	if (res != NULL) memset(res, 0, total);
	return res;
}

// Reallocates a currently allocated block on taipool mempool
void* taipool_realloc(void* ptr, size_t size) {
	size_t aligned_size;
	mempool_block_hdr* hdr;
	void* res = NULL;

	if (_taipool_align_size(size, &aligned_size) < 0) return NULL;

	_taipool_lock_access();
	hdr = (mempool_block_hdr*)((uint8_t*)ptr - sizeof(mempool_block_hdr));
	if (hdr->size < aligned_size) { // Increasing size
		res = _taipool_extend_block(ptr, aligned_size);
		if (res == NULL) {
			res = _taipool_alloc_block(aligned_size);
			if (res != NULL) {
				memcpy(res, ptr, hdr->size);
				_taipool_free_block(ptr);
				_taipool_merge_blocks();
			}
		}
	} else { // Reducing size
		_taipool_compact_block(ptr, aligned_size);
		_taipool_merge_blocks();
		res = ptr;
	}
	_taipool_unlock_access();
	return res;
}

// Returns currently free space on mempool
size_t taipool_get_free_space(void){
	return mempool_free;
}
