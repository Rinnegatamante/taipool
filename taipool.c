#include <vitasdk.h>
#include "taipool.h"

static int dummy_thread(SceSize args, void *argp){return 0;}
static void* mempool_addr = NULL;
static size_t mempool_size = 0;
static size_t mempool_index = 0;
static SceUID mempool_id = 0;

int taipool_init(size_t size){
	SceUID pool_id = sceKernelCreateThread("mempool_thread", dummy_thread, 0x40, size, 0, 0, NULL);
	if (pool_id >= 0){
		SceKernelThreadInfo mempool_info;
		mempool_info.size = sizeof(SceKernelThreadInfo);
		sceKernelGetThreadInfo(pool_id, &mempool_info);
		mempool_addr = mempool_info.stack;
		mempool_size = size;
		mempool_index = 0;
		mempool_id = pool_id;
		return 0;
	}
	return pool_id;
}

void taipool_term(void){
	if (mempool_addr != NULL){
		sceKernelDeleteThread(mempool_id);
		mempool_addr = NULL;
		mempool_size = 0;
		mempool_index = 0;
	}
}

void* taipool_alloc(size_t size){
	if ((mempool_index + size) < mempool_size){
		void *addr = (void *)((unsigned int)mempool_addr + mempool_index);
		mempool_index += size;
		return addr;
	}
	return NULL;
}

void taipool_reset(void){
	mempool_index = 0;
}

size_t taipool_get_free_space(void){
	return mempool_size - mempool_index;
}