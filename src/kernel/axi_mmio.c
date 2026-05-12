#include <linux/dma-map-ops.h>
#include <linux/acpi.h>
#include <linux/bitmap.h>
#include <linux/ioport.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/refcount.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>

#define AXI_MMIO_VERSION "0.1"

struct axi_mmio {
    struct platform_device *virtio_mmio;
    struct axi_mmio_pool *pool;
};

struct axi_mmio_pool {
    struct list_head node;
    struct list_head bounces;
    phys_addr_t dma_base;
    resource_size_t dma_size;
    void *dma_virt;
    unsigned long *dma_bitmap;
    unsigned long dma_pages;
    struct mutex dma_lock;
    refcount_t refs;
};

struct axi_mmio_bounce {
    struct list_head node;
    dma_addr_t dma_handle;
    phys_addr_t phys;
    size_t size;
};

static DEFINE_MUTEX(axi_mmio_pools_lock);
static LIST_HEAD(axi_mmio_pools);

static void axi_mmio_pool_put(struct axi_mmio_pool *pool)
{
    if (!pool) {
        return;
    }

    mutex_lock(&axi_mmio_pools_lock);
    if (!refcount_dec_and_test(&pool->refs)) {
        mutex_unlock(&axi_mmio_pools_lock);
        return;
    }
    list_del(&pool->node);
    mutex_unlock(&axi_mmio_pools_lock);

    bitmap_free(pool->dma_bitmap);
    memunmap(pool->dma_virt);
    kfree(pool);
}

static void axi_mmio_pool_release(void *data)
{
    axi_mmio_pool_put(data);
}

static struct axi_mmio_pool *axi_mmio_pool_get(phys_addr_t base,
                                               resource_size_t size)
{
    struct axi_mmio_pool *pool;
    struct axi_mmio_pool *new_pool;

    mutex_lock(&axi_mmio_pools_lock);
    list_for_each_entry(pool, &axi_mmio_pools, node) {
        if (pool->dma_base == base && pool->dma_size == size) {
            refcount_inc(&pool->refs);
            mutex_unlock(&axi_mmio_pools_lock);
            return pool;
        }
    }
    mutex_unlock(&axi_mmio_pools_lock);

    new_pool = kzalloc(sizeof(*new_pool), GFP_KERNEL);
    if (!new_pool) {
        return NULL;
    }
    new_pool->dma_base = base;
    new_pool->dma_size = size;
    new_pool->dma_pages = size >> PAGE_SHIFT;
    INIT_LIST_HEAD(&new_pool->bounces);
    mutex_init(&new_pool->dma_lock);
    refcount_set(&new_pool->refs, 1);

    new_pool->dma_virt = memremap(base, size, MEMREMAP_WB);
    if (!new_pool->dma_virt) {
        kfree(new_pool);
        return NULL;
    }

    new_pool->dma_bitmap = bitmap_zalloc(new_pool->dma_pages, GFP_KERNEL);
    if (!new_pool->dma_bitmap) {
        memunmap(new_pool->dma_virt);
        kfree(new_pool);
        return NULL;
    }

    mutex_lock(&axi_mmio_pools_lock);
    list_for_each_entry(pool, &axi_mmio_pools, node) {
        if (pool->dma_base == base && pool->dma_size == size) {
            refcount_inc(&pool->refs);
            mutex_unlock(&axi_mmio_pools_lock);
            bitmap_free(new_pool->dma_bitmap);
            memunmap(new_pool->dma_virt);
            kfree(new_pool);
            return pool;
        }
    }
    list_add(&new_pool->node, &axi_mmio_pools);
    mutex_unlock(&axi_mmio_pools_lock);

    return new_pool;
}

static void *axi_mmio_pool_vaddr(struct axi_mmio_pool *pool,
                                 dma_addr_t dma_handle)
{
    return pool->dma_virt + (dma_handle - pool->dma_base);
}

static bool axi_mmio_pool_alloc(struct axi_mmio_pool *pool, size_t size,
                                dma_addr_t *dma_handle)
{
    unsigned long pages = PAGE_ALIGN(size) >> PAGE_SHIFT;
    unsigned long start;

    if (!pages) {
        return false;
    }

    mutex_lock(&pool->dma_lock);
    start = bitmap_find_next_zero_area(pool->dma_bitmap, pool->dma_pages, 0,
                                       pages, 0);
    if (start >= pool->dma_pages) {
        mutex_unlock(&pool->dma_lock);
        return false;
    }
    bitmap_set(pool->dma_bitmap, start, pages);
    mutex_unlock(&pool->dma_lock);

    *dma_handle = pool->dma_base + (start << PAGE_SHIFT);
    return true;
}

static void axi_mmio_pool_free(struct axi_mmio_pool *pool,
                               dma_addr_t dma_handle, size_t size)
{
    unsigned long offset;
    unsigned long pages = PAGE_ALIGN(size) >> PAGE_SHIFT;

    if (dma_handle < pool->dma_base || !pages) {
        return;
    }

    offset = (dma_handle - pool->dma_base) >> PAGE_SHIFT;
    if (offset + pages > pool->dma_pages) {
        return;
    }

    mutex_lock(&pool->dma_lock);
    bitmap_clear(pool->dma_bitmap, offset, pages);
    mutex_unlock(&pool->dma_lock);
}

static bool axi_mmio_copy_phys_to_pool(struct axi_mmio_pool *pool,
                                       dma_addr_t dma_handle, phys_addr_t phys,
                                       size_t size)
{
    void *src;

    src = memremap(phys, size, MEMREMAP_WB);
    if (!src) {
        return false;
    }
    memcpy(axi_mmio_pool_vaddr(pool, dma_handle), src, size);
    memunmap(src);
    return true;
}

static bool axi_mmio_copy_pool_to_phys(struct axi_mmio_pool *pool,
                                       dma_addr_t dma_handle, phys_addr_t phys,
                                       size_t size)
{
    void *dst;

    dst = memremap(phys, size, MEMREMAP_WB);
    if (!dst) {
        return false;
    }
    memcpy(dst, axi_mmio_pool_vaddr(pool, dma_handle), size);
    memunmap(dst);
    return true;
}

static struct axi_mmio_bounce *axi_mmio_bounce_find_locked(
    struct axi_mmio_pool *pool, dma_addr_t dma_handle)
{
    struct axi_mmio_bounce *bounce;

    list_for_each_entry(bounce, &pool->bounces, node) {
        if (bounce->dma_handle == dma_handle) {
            return bounce;
        }
    }
    return NULL;
}

static void axi_mmio_bounce_sync_for_device(struct axi_mmio_pool *pool,
                                            dma_addr_t dma_handle,
                                            size_t size,
                                            enum dma_data_direction dir)
{
    struct axi_mmio_bounce *bounce;

    if (dir == DMA_FROM_DEVICE) {
        return;
    }

    mutex_lock(&pool->dma_lock);
    bounce = axi_mmio_bounce_find_locked(pool, dma_handle);
    if (bounce) {
        phys_addr_t phys = bounce->phys;

        size = min(size, bounce->size);
        mutex_unlock(&pool->dma_lock);
        axi_mmio_copy_phys_to_pool(pool, dma_handle, phys, size);
        return;
    }
    mutex_unlock(&pool->dma_lock);
}

static void axi_mmio_bounce_sync_for_cpu(struct axi_mmio_pool *pool,
                                         dma_addr_t dma_handle, size_t size,
                                         enum dma_data_direction dir)
{
    struct axi_mmio_bounce *bounce;

    if (dir == DMA_TO_DEVICE) {
        return;
    }

    mutex_lock(&pool->dma_lock);
    bounce = axi_mmio_bounce_find_locked(pool, dma_handle);
    if (bounce) {
        phys_addr_t phys = bounce->phys;

        size = min(size, bounce->size);
        mutex_unlock(&pool->dma_lock);
        axi_mmio_copy_pool_to_phys(pool, dma_handle, phys, size);
        return;
    }
    mutex_unlock(&pool->dma_lock);
}

static void *axi_mmio_dma_alloc(struct device *dev, size_t size,
                                dma_addr_t *dma_handle, gfp_t gfp,
                                unsigned long attrs)
{
    struct axi_mmio *axi = dev_get_drvdata(dev->parent);
    struct axi_mmio_pool *pool = axi ? axi->pool : NULL;
    dma_addr_t dma;

    (void)gfp;
    (void)attrs;

    if (!pool || !axi_mmio_pool_alloc(pool, size, &dma)) {
        return NULL;
    }

    *dma_handle = dma;
    return axi_mmio_pool_vaddr(pool, dma);
}

static void axi_mmio_dma_free(struct device *dev, size_t size, void *vaddr,
                              dma_addr_t dma_handle, unsigned long attrs)
{
    struct axi_mmio *axi = dev_get_drvdata(dev->parent);
    struct axi_mmio_pool *pool = axi ? axi->pool : NULL;

    (void)vaddr;
    (void)attrs;

    if (!pool) {
        return;
    }

    axi_mmio_pool_free(pool, dma_handle, size);
}

static dma_addr_t axi_mmio_dma_map_phys(struct device *dev, phys_addr_t phys,
                                        size_t size,
                                        enum dma_data_direction dir,
                                        unsigned long attrs)
{
    struct axi_mmio *axi = dev_get_drvdata(dev->parent);
    struct axi_mmio_pool *pool = axi ? axi->pool : NULL;
    struct axi_mmio_bounce *bounce;
    dma_addr_t dma_handle;

    (void)attrs;

    if (!pool || !size) {
        return DMA_MAPPING_ERROR;
    }

    if (phys >= pool->dma_base && size <= pool->dma_size &&
        phys - pool->dma_base <= pool->dma_size - size) {
        return phys;
    }

    bounce = kzalloc(sizeof(*bounce), GFP_KERNEL);
    if (!bounce) {
        return DMA_MAPPING_ERROR;
    }

    if (!axi_mmio_pool_alloc(pool, size, &dma_handle)) {
        kfree(bounce);
        return DMA_MAPPING_ERROR;
    }

    if (dir != DMA_FROM_DEVICE &&
        !axi_mmio_copy_phys_to_pool(pool, dma_handle, phys, size)) {
        axi_mmio_pool_free(pool, dma_handle, size);
        kfree(bounce);
        return DMA_MAPPING_ERROR;
    }

    bounce->dma_handle = dma_handle;
    bounce->phys = phys;
    bounce->size = size;

    mutex_lock(&pool->dma_lock);
    list_add(&bounce->node, &pool->bounces);
    mutex_unlock(&pool->dma_lock);

    return dma_handle;
}

static void axi_mmio_dma_unmap_phys(struct device *dev, dma_addr_t dma_handle,
                                    size_t size,
                                    enum dma_data_direction dir,
                                    unsigned long attrs)
{
    struct axi_mmio *axi = dev_get_drvdata(dev->parent);
    struct axi_mmio_pool *pool = axi ? axi->pool : NULL;
    struct axi_mmio_bounce *bounce;

    (void)attrs;

    if (!pool || dma_handle < pool->dma_base || size > pool->dma_size ||
        dma_handle - pool->dma_base > pool->dma_size - size) {
        return;
    }

    mutex_lock(&pool->dma_lock);
    bounce = axi_mmio_bounce_find_locked(pool, dma_handle);
    if (bounce) {
        list_del(&bounce->node);
    }
    mutex_unlock(&pool->dma_lock);

    if (!bounce) {
        return;
    }

    if (dir != DMA_TO_DEVICE) {
        axi_mmio_copy_pool_to_phys(pool, dma_handle, bounce->phys,
                                   min(size, bounce->size));
    }
    axi_mmio_pool_free(pool, dma_handle, bounce->size);
    kfree(bounce);
}

static int axi_mmio_dma_map_sg(struct device *dev, struct scatterlist *sgl,
                               int nents, enum dma_data_direction dir,
                               unsigned long attrs)
{
    struct axi_mmio *axi = dev_get_drvdata(dev->parent);
    struct axi_mmio_pool *pool = axi ? axi->pool : NULL;
    struct scatterlist *sg;
    int i;

    (void)attrs;

    if (!pool) {
        return -EINVAL;
    }

    for_each_sg(sgl, sg, nents, i) {
        dma_addr_t addr;

        if (!axi_mmio_pool_alloc(pool, sg->length, &addr)) {
            int j;
            struct scatterlist *mapped;

            for_each_sg(sgl, mapped, i, j) {
                axi_mmio_pool_free(pool, sg_dma_address(mapped),
                                   sg_dma_len(mapped));
                sg_dma_address(mapped) = DMA_MAPPING_ERROR;
                sg_dma_len(mapped) = 0;
            }
            return -ENOMEM;
        }

        if (dir != DMA_FROM_DEVICE) {
            sg_copy_to_buffer(sg, 1, axi_mmio_pool_vaddr(pool, addr),
                              sg->length);
        }

        sg_dma_address(sg) = addr;
        sg_dma_len(sg) = sg->length;
    }
    return nents;
}

static void axi_mmio_dma_unmap_sg(struct device *dev, struct scatterlist *sgl,
                                  int nents, enum dma_data_direction dir,
                                  unsigned long attrs)
{
    struct axi_mmio *axi = dev_get_drvdata(dev->parent);
    struct axi_mmio_pool *pool = axi ? axi->pool : NULL;
    struct scatterlist *sg;
    int i;

    (void)attrs;

    if (!pool) {
        return;
    }

    for_each_sg(sgl, sg, nents, i) {
        dma_addr_t addr = sg_dma_address(sg);
        unsigned int len = sg_dma_len(sg);

        if (addr == DMA_MAPPING_ERROR || !len) {
            continue;
        }
        if (dir != DMA_TO_DEVICE) {
            sg_copy_from_buffer(sg, 1, axi_mmio_pool_vaddr(pool, addr), len);
        }
        axi_mmio_pool_free(pool, addr, len);
        sg_dma_address(sg) = DMA_MAPPING_ERROR;
        sg_dma_len(sg) = 0;
    }
}

static void axi_mmio_dma_sync_single_for_cpu(struct device *dev,
                                             dma_addr_t dma_handle,
                                             size_t size,
                                             enum dma_data_direction dir)
{
    struct axi_mmio *axi = dev_get_drvdata(dev->parent);
    struct axi_mmio_pool *pool = axi ? axi->pool : NULL;

    if (!pool) {
        return;
    }
    axi_mmio_bounce_sync_for_cpu(pool, dma_handle, size, dir);
}

static void axi_mmio_dma_sync_single_for_device(struct device *dev,
                                                dma_addr_t dma_handle,
                                                size_t size,
                                                enum dma_data_direction dir)
{
    struct axi_mmio *axi = dev_get_drvdata(dev->parent);
    struct axi_mmio_pool *pool = axi ? axi->pool : NULL;

    if (!pool) {
        return;
    }
    axi_mmio_bounce_sync_for_device(pool, dma_handle, size, dir);
}

static int axi_mmio_dma_supported(struct device *dev, u64 mask)
{
    (void)dev;
    return mask == DMA_BIT_MASK(64);
}

static const struct dma_map_ops axi_mmio_dma_ops = {
    .alloc = axi_mmio_dma_alloc,
    .free = axi_mmio_dma_free,
    .map_phys = axi_mmio_dma_map_phys,
    .unmap_phys = axi_mmio_dma_unmap_phys,
    .map_sg = axi_mmio_dma_map_sg,
    .unmap_sg = axi_mmio_dma_unmap_sg,
    .sync_single_for_cpu = axi_mmio_dma_sync_single_for_cpu,
    .sync_single_for_device = axi_mmio_dma_sync_single_for_device,
    .dma_supported = axi_mmio_dma_supported,
};

static void axi_mmio_unregister_child(void *data)
{
    platform_device_unregister(data);
}

static int axi_mmio_copy_resources(struct platform_device *pdev,
                                   struct resource **resources)
{
    struct resource *mmio;
    int irq;

    mmio = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    if (!mmio) {
        return -EINVAL;
    }

    irq = platform_get_irq(pdev, 0);
    if (irq < 0) {
        return irq;
    }

    *resources = devm_kcalloc(&pdev->dev, 2, sizeof(**resources), GFP_KERNEL);
    if (!*resources) {
        return -ENOMEM;
    }

    (*resources)[0] = *mmio;
    (*resources)[1] = (struct resource)DEFINE_RES_IRQ(irq);
    return 2;
}

static int axi_mmio_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct axi_mmio *axi;
    struct resource *dma;
    struct resource *resources;
    phys_addr_t dma_base;
    resource_size_t dma_size;
    int num_resources;
    int ret;

    dma = platform_get_resource(pdev, IORESOURCE_MEM, 1);
    if (!dma) {
        return dev_err_probe(dev, -EINVAL,
                             "missing restricted DMA/CMA resource\n");
    }

    axi = devm_kzalloc(dev, sizeof(*axi), GFP_KERNEL);
    if (!axi) {
        return -ENOMEM;
    }
    dma_base = dma->start;
    dma_size = resource_size(dma);

    if (!IS_ALIGNED(dma_base, PAGE_SIZE) ||
        !IS_ALIGNED(dma_size, PAGE_SIZE) || !(dma_size >> PAGE_SHIFT)) {
        return dev_err_probe(dev, -EINVAL,
                             "restricted DMA aperture must be page aligned\n");
    }

    axi->pool = axi_mmio_pool_get(dma_base, dma_size);
    if (!axi->pool) {
        return dev_err_probe(dev, -ENOMEM,
                             "failed to initialize restricted DMA aperture\n");
    }
    ret = devm_add_action_or_reset(dev, axi_mmio_pool_release, axi->pool);
    if (ret) {
        return ret;
    }

    platform_set_drvdata(pdev, axi);

    num_resources = axi_mmio_copy_resources(pdev, &resources);
    if (num_resources < 0) {
        return dev_err_probe(dev, num_resources,
                             "failed to copy virtio-mmio resources\n");
    }

    axi->virtio_mmio = platform_device_alloc("virtio-mmio", PLATFORM_DEVID_AUTO);
    if (!axi->virtio_mmio) {
        return -ENOMEM;
    }

    axi->virtio_mmio->dev.parent = dev;
    axi->virtio_mmio->dev.fwnode = dev_fwnode(dev);
    axi->virtio_mmio->dev.dma_mask = &axi->virtio_mmio->dev.coherent_dma_mask;
    axi->virtio_mmio->dev.coherent_dma_mask = DMA_BIT_MASK(64);
    set_dma_ops(&axi->virtio_mmio->dev, &axi_mmio_dma_ops);

    ret = platform_device_add_resources(axi->virtio_mmio, resources,
                                        num_resources);
    if (ret) {
        platform_device_put(axi->virtio_mmio);
        return dev_err_probe(dev, ret, "failed to add child resources\n");
    }

    ret = platform_device_add(axi->virtio_mmio);
    if (ret) {
        platform_device_put(axi->virtio_mmio);
        return dev_err_probe(dev, ret, "failed to add virtio-mmio child\n");
    }

    ret = devm_add_action_or_reset(dev, axi_mmio_unregister_child,
                                   axi->virtio_mmio);
    if (ret) {
        return ret;
    }

    dev_info(dev, "virtio-mmio vring DMA restricted to %pa-%pa\n",
             &axi->pool->dma_base,
             &(phys_addr_t){axi->pool->dma_base + axi->pool->dma_size - 1});
    return 0;
}

static const struct of_device_id axi_mmio_of_ids[] = {
    { .compatible = "axi,mmio" },
    { }
};
MODULE_DEVICE_TABLE(of, axi_mmio_of_ids);

static const struct acpi_device_id axi_mmio_acpi_ids[] = {
    { "AXI0001", 0 },
    { }
};
MODULE_DEVICE_TABLE(acpi, axi_mmio_acpi_ids);

static struct platform_driver axi_mmio_driver = {
    .probe = axi_mmio_probe,
    .driver = {
        .name = "axi-mmio",
        .acpi_match_table = axi_mmio_acpi_ids,
        .of_match_table = axi_mmio_of_ids,
    },
};
module_platform_driver(axi_mmio_driver);

MODULE_LICENSE("GPL");
MODULE_VERSION(AXI_MMIO_VERSION);
MODULE_DESCRIPTION("AXI virtio-mmio frontend with restricted vring DMA pool");
