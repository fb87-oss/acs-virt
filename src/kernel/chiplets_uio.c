#include <linux/acpi.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uio_driver.h>

#define CHIPLET_UIO_MMIO_BASE 0x0010feb00000ULL
#define CHIPLET_UIO_MMIO_SIZE 0x1000ULL
#define CHIPLET_UIO_DMA_BASE 0x001000000000ULL
#define CHIPLET_UIO_DMA_SIZE 0x20000000ULL
#define CHIPLET_UIO_IRQ_BASE 16

struct chiplets_uio_dev {
    struct uio_info info;
    spinlock_t lock;
    bool irq_enabled;
    unsigned index;
};

static irqreturn_t chiplets_uio_irq(int irq, struct uio_info *info)
{
    struct chiplets_uio_dev *dev = info->priv;
    unsigned long flags;

    spin_lock_irqsave(&dev->lock, flags);
    if (dev->irq_enabled) {
        disable_irq_nosync(irq);
        dev->irq_enabled = false;
    }
    spin_unlock_irqrestore(&dev->lock, flags);
    return IRQ_HANDLED;
}

static int chiplets_uio_irqcontrol(struct uio_info *info, s32 irq_on)
{
    struct chiplets_uio_dev *dev = info->priv;
    unsigned long flags;

    spin_lock_irqsave(&dev->lock, flags);
    if (irq_on && !dev->irq_enabled) {
        dev->irq_enabled = true;
        enable_irq(info->irq);
    } else if (!irq_on && dev->irq_enabled) {
        disable_irq_nosync(info->irq);
        dev->irq_enabled = false;
    }
    spin_unlock_irqrestore(&dev->lock, flags);
    return 0;
}

static int chiplets_uio_mmap(struct uio_info *info, struct vm_area_struct *vma)
{
    unsigned long map_index = vma->vm_pgoff;
    unsigned long requested = vma->vm_end - vma->vm_start;
    struct uio_mem *mem;
    unsigned long pfn;

    if (map_index >= MAX_UIO_MAPS) {
        return -EINVAL;
    }

    mem = &info->mem[map_index];
    if (!mem->size || requested > mem->size) {
        return -EINVAL;
    }

    pfn = mem->addr >> PAGE_SHIFT;
    vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP);

    if (map_index == 0) {
        vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
    }

    return remap_pfn_range(vma, vma->vm_start, pfn, requested,
                           vma->vm_page_prot);
}

static unsigned chiplets_uio_index(struct platform_device *pdev)
{
    struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    const char *name = dev_name(&pdev->dev);

    if (res && resource_size(res) > 0) {
        return (res->start - CHIPLET_UIO_MMIO_BASE) / CHIPLET_UIO_MMIO_SIZE;
    }

    if (strstr(name, ":01")) {
        return 1;
    }
    return 0;
}

static int chiplets_uio_probe(struct platform_device *pdev)
{
    struct chiplets_uio_dev *dev;
    struct resource *dma_res;
    struct resource *mmio_res;
    int irq;

    dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
    if (!dev) {
        return -ENOMEM;
    }

    dev->index = chiplets_uio_index(pdev);
    spin_lock_init(&dev->lock);
    dev->irq_enabled = true;

    dev->info.name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "chiplets-uio%u",
                                    dev->index);
    if (!dev->info.name) {
        return -ENOMEM;
    }
    dev->info.version = "0.1";
    irq = platform_get_irq_optional(pdev, 0);
    dev->info.irq = irq >= 0 ? irq : CHIPLET_UIO_IRQ_BASE + dev->index;
    dev->info.irq_flags = 0;
    dev->info.handler = chiplets_uio_irq;
    dev->info.irqcontrol = chiplets_uio_irqcontrol;
    dev->info.mmap = chiplets_uio_mmap;
    dev->info.priv = dev;

    mmio_res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    dev->info.mem[0].name = "mmio";
    dev->info.mem[0].addr = mmio_res ? mmio_res->start :
                             CHIPLET_UIO_MMIO_BASE +
                                 dev->index * CHIPLET_UIO_MMIO_SIZE;
    dev->info.mem[0].size = mmio_res ? resource_size(mmio_res) :
                             CHIPLET_UIO_MMIO_SIZE;
    dev->info.mem[0].memtype = UIO_MEM_PHYS;

    dma_res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
    dev->info.mem[1].name = "frontend-ram";
    dev->info.mem[1].addr = dma_res ? dma_res->start : CHIPLET_UIO_DMA_BASE;
    dev->info.mem[1].size = dma_res ? resource_size(dma_res) :
                             CHIPLET_UIO_DMA_SIZE;
    dev->info.mem[1].memtype = UIO_MEM_PHYS;

    platform_set_drvdata(pdev, dev);
    return devm_uio_register_device(&pdev->dev, &dev->info);
}

static const struct acpi_device_id chiplets_uio_acpi_ids[] = {
    { "PRP0001", 0 },
    { }
};
MODULE_DEVICE_TABLE(acpi, chiplets_uio_acpi_ids);

static const struct of_device_id chiplets_uio_of_ids[] = {
    { .compatible = "chiplets,uio" },
    { }
};
MODULE_DEVICE_TABLE(of, chiplets_uio_of_ids);

static struct platform_driver chiplets_uio_driver = {
    .probe = chiplets_uio_probe,
    .driver = {
        .name = "chiplets-uio",
        .acpi_match_table = chiplets_uio_acpi_ids,
        .of_match_table = chiplets_uio_of_ids,
    },
};
module_platform_driver(chiplets_uio_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Chiplets UIO smoke-test platform driver");
