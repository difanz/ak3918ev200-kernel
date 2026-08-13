#ifndef DMA_COHERENT_H
#define DMA_COHERENT_H

#ifdef CONFIG_HAVE_GENERIC_DMA_COHERENT
/*
 * These three functions are only for dma allocator.
 * Don't use them in device drivers.
 */
int dma_alloc_from_coherent(struct device *dev, ssize_t size,
				       dma_addr_t *dma_handle, void **ret);
int dma_release_from_coherent(struct device *dev, int order, void *vaddr);

int dma_mmap_from_coherent(struct device *dev, struct vm_area_struct *vma,
			    void *cpu_addr, size_t size, int *ret);
/*
 * Standard interface
 */
#define ARCH_HAS_DMA_DECLARE_COHERENT_MEMORY
int dma_declare_coherent_memory(struct device *dev, phys_addr_t phys_addr,
				dma_addr_t device_addr, size_t size, int flags);

void dma_release_declared_memory(struct device *dev);

void *dma_mark_declared_memory_occupied(struct device *dev,
					dma_addr_t device_addr, size_t size);

/*
 * Reports the device's coherent pool: size, used, largest allocatable run and
 * per-device attribution, in bytes. A driver that owns a "shared-dma-pool"
 * region publishes it with device_create_file().
 */
struct device_attribute;
extern struct device_attribute dev_attr_dma_coherent_pool;
#else
#define dma_alloc_from_coherent(dev, size, handle, ret) (0)
#define dma_release_from_coherent(dev, order, vaddr) (0)
#define dma_mmap_from_coherent(dev, vma, vaddr, order, ret) (0)
#endif

#endif
