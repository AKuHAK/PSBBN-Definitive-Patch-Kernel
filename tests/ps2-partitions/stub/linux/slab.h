#ifndef STUB_SLAB_H
#define STUB_SLAB_H
#define GFP_KERNEL	0
#define kmalloc(size, flags)	malloc(size)
#define kfree(p)	free(p)
#endif
