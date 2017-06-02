# taipool
Mempool library for taiHEN user plugins.

taipool is a minimalistic library giving a mempool implementation to taiHEN user plugins developers.<br>
By default, user plugins don't have malloc/free implementation due to lack of newlib and sceKernelAllocMemBlock is main application dependant in terms of how much memory can be allocated.<br>
With taipool you can create a mempool by using a little trick: normally sceKernelCreateThread in user plugins environment can be used with relatively big stackSize parameter (up to 4 MBs should be fine).<br>
What taipool does is to create a new thread without starting it and using reserved stack memory as a mempool for our usage.
