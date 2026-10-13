// SPDX-License-Identifier: GPL-2.0-only
/* SM8450 encoder ARP memory is protected even for nonsecure video sessions. */
#include <linux/dma-mapping.h>
#include <linux/dma-map-ops.h>
#include <linux/soc/qcom/secure-iommu.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/genalloc.h>
#include <linux/iommu.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/sizes.h>
#include <linux/of_platform.h>
#include <linux/slab.h>
#include <dt-bindings/firmware/qcom,scm.h>

#include "iris_core.h"
#include "iris_instance.h"
#include "iris_secure_arp.h"

struct iris_secure_ctx {
	struct device *dev;
	struct iommu_domain *domain;
	struct gen_pool *pool;
	struct mutex lock;
	struct list_head buffers;
	bool poisoned;
};

struct iris_secure_buffer {
	struct list_head list;
	struct iris_secure_ctx *ctx;
	void *pages;
	size_t size;
	unsigned long iova;
	u64 owners;
};

static void iris_secure_retain(struct iris_secure_buffer *mem, const char *reason)
{
	phys_addr_t phys = virt_to_phys(mem->pages);

	mem->ctx->poisoned = true;

	dev_err(mem->ctx->dev, "retaining ARP %pa iova %#lx size %zu: %s\n",
		&phys, mem->iova, mem->size, reason);
}

/* Caller holds ctx->lock; failed transitions retain both memory and IOVA. */
static void iris_secure_release(struct iris_secure_buffer *mem, size_t mapped)
{
	struct iris_secure_ctx *ctx = mem->ctx;
	struct qcom_scm_vmperm hlos = { QCOM_SCM_VMID_HLOS, QCOM_SCM_PERM_RWX };

	if (!qcom_smmu_secure_domain_healthy(ctx->domain)) {
		iris_secure_retain(mem, "IOMMU domain faulted before release");
		return;
	}
	if (mapped && iommu_unmap(ctx->domain, mem->iova, mapped) != mapped) {
		iris_secure_retain(mem, "incomplete IOMMU unmap");
		return;
	}
	if (!qcom_smmu_secure_domain_healthy(ctx->domain)) {
		iris_secure_retain(mem, "IOMMU synchronization failed");
		return;
	}
	if (qcom_scm_assign_mem(virt_to_phys(mem->pages), mem->size,
				&mem->owners, &hlos, 1)) {
		iris_secure_retain(mem, "ownership revoke failed");
		return;
	}
	gen_pool_free(ctx->pool, mem->iova, mem->size);
	free_pages_exact(mem->pages, mem->size);
	list_del(&mem->list);
	kfree(mem);
}

int iris_secure_arp_alloc(struct iris_inst *inst, struct iris_buffer *buffer)
{
	struct qcom_scm_vmperm secure = { QCOM_SCM_VMID_CP_NON_PIXEL, QCOM_SCM_PERM_RW };
	struct iris_core *core = inst->core;
	struct iris_secure_buffer *mem;
	struct iris_secure_ctx *ctx;
	dma_addr_t clean_dma;
	size_t mapped;
	int ret;

	if (dev_is_dma_coherent(core->dev))
		return -EOPNOTSUPP;
	if (!core->secure_arp_dev)
		return -ENODEV;
	ctx = dev_get_drvdata(core->secure_arp_dev);
	if (!ctx)
		return -EAGAIN;
	mem = kzalloc(sizeof(*mem), GFP_KERNEL);
	if (!mem)
		return -ENOMEM;
	mem->ctx = ctx;
	mem->size = PAGE_ALIGN(buffer->buffer_size);
	mem->pages = alloc_pages_exact(mem->size, GFP_KERNEL | __GFP_ZERO);
	if (!mem->pages) {
		kfree(mem);
		return -ENOMEM;
	}
	/*
	 * Clean/invalidate the zeroed CPU mapping before HLOS loses access.
	 * Use the ordinary parent DMA device, never the driver-managed child.
	 */
	clean_dma = dma_map_single(core->dev, mem->pages, mem->size, DMA_BIDIRECTIONAL);
	if (dma_mapping_error(core->dev, clean_dma)) {
		free_pages_exact(mem->pages, mem->size);
		kfree(mem);
		return -ENOMEM;
	}
	dma_unmap_single(core->dev, clean_dma, mem->size, DMA_BIDIRECTIONAL);

	mutex_lock(&ctx->lock);
	if (ctx->poisoned || !qcom_smmu_secure_domain_healthy(ctx->domain)) {
		ret = -EIO;
		goto free_pages;
	}
	mem->iova = gen_pool_alloc(ctx->pool, mem->size);
	if (!mem->iova) {
		ret = -ENOMEM;
		goto free_pages;
	}
	list_add_tail(&mem->list, &ctx->buffers);
	mem->owners = BIT_ULL(QCOM_SCM_VMID_HLOS);
	ret = qcom_scm_assign_mem(virt_to_phys(mem->pages), mem->size,
				  &mem->owners, &secure, 1);
	if (ret) {
		iris_secure_retain(mem, "ownership grant failed");
		goto unlock;
	}
	/* Fixed 4K mappings never require an atomic page-table split at unmap. */
	for (mapped = 0; mapped < mem->size; mapped += SZ_4K) {
		ret = iommu_map(ctx->domain, mem->iova + mapped,
				virt_to_phys(mem->pages) + mapped, SZ_4K,
				IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
		if (ret) {
			iris_secure_release(mem, mapped);
			goto unlock;
		}
	}
	if (!qcom_smmu_secure_domain_healthy(ctx->domain)) {
		iris_secure_retain(mem, "IOMMU fault during mapping");
		ret = -EIO;
		goto unlock;
	}
	buffer->secure = mem;
	buffer->device_addr = mem->iova;
	dev_dbg(ctx->dev, "ARP secure size %zu iova %#lx\n", mem->size, mem->iova);
	mutex_unlock(&ctx->lock);
	return 0;
free_pages:
	free_pages_exact(mem->pages, mem->size);
	kfree(mem);
unlock:
	mutex_unlock(&ctx->lock);
	return ret;
}

void iris_secure_arp_free(struct iris_inst *inst, struct iris_buffer *buffer)
{
	struct iris_secure_buffer *mem = buffer->secure;
	struct iris_secure_ctx *ctx = mem->ctx;

	mutex_lock(&ctx->lock);
	if ((buffer->attr & BUF_ATTR_QUEUED) && !inst->session_closed)
		iris_secure_retain(mem, "firmware release/close not acknowledged");
	else
		iris_secure_release(mem, mem->size);
	mutex_unlock(&ctx->lock);
}

static int iris_secure_probe(struct platform_device *pdev)
{
	struct iris_secure_ctx *ctx;
	u32 vmid;
	int ret;

	if (PAGE_SIZE != SZ_4K ||
	    of_property_read_u32(pdev->dev.of_node, "qcom,iommu-vmid", &vmid) ||
	    vmid != QCOM_SCM_VMID_CP_NON_PIXEL)
		return -EINVAL;
	if (!qcom_scm_is_available())
		return -EPROBE_DEFER;
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->dev = &pdev->dev;
	mutex_init(&ctx->lock);
	INIT_LIST_HEAD(&ctx->buffers);
	ctx->pool = gen_pool_create(PAGE_SHIFT, -1);
	if (!ctx->pool) {
		ret = -ENOMEM;
		goto free_ctx;
	}
	ret = gen_pool_add(ctx->pool, 0x01000000, 0x24800000, -1);
	if (ret)
		goto free_pool;
	ret = iommu_device_claim_dma_owner(&pdev->dev, ctx);
	if (ret)
		goto free_pool;
	ctx->domain = iommu_paging_domain_alloc(&pdev->dev);
	if (IS_ERR(ctx->domain)) {
		ret = PTR_ERR(ctx->domain);
		goto release_owner;
	}
	ret = iommu_attach_device(ctx->domain, &pdev->dev);
	if (ret)
		goto free_domain;
	platform_set_drvdata(pdev, ctx);
	dev_info(&pdev->dev, "secure ARP domain ready, IOVA 0x01000000..0x25800000\n");
	return 0;
free_domain:
	iommu_domain_free(ctx->domain);
release_owner:
	iommu_device_release_dma_owner(&pdev->dev);
free_pool:
	gen_pool_destroy(ctx->pool);
free_ctx:
	mutex_destroy(&ctx->lock);
	kfree(ctx);
	return ret;
}

static LIST_HEAD(iris_secure_quarantine);
static DEFINE_MUTEX(iris_secure_quarantine_lock);

static void iris_secure_remove(struct platform_device *pdev)
{
	struct iris_secure_ctx *ctx = platform_get_drvdata(pdev);
	struct iris_secure_buffer *mem;

	/*
	 * Normal sessions release before removal. Unknown ownership/access is
	 * deliberately retained until reboot, never converted into free RAM.
	 */
	if (!list_empty(&ctx->buffers)) {
		dev_err(&pdev->dev, "protected ARP remains; quarantining memory until reboot\n");
		mutex_lock(&iris_secure_quarantine_lock);
		list_for_each_entry(mem, &ctx->buffers, list) {
			gen_pool_free(ctx->pool, mem->iova, mem->size);
			mem->ctx = NULL;
		}
		list_splice_tail_init(&ctx->buffers, &iris_secure_quarantine);
		mutex_unlock(&iris_secure_quarantine_lock);
	}
	iommu_detach_device(ctx->domain, &pdev->dev);
	iommu_domain_free(ctx->domain);
	iommu_device_release_dma_owner(&pdev->dev);
	gen_pool_destroy(ctx->pool);
	mutex_destroy(&ctx->lock);
	kfree(ctx);
}


static const struct of_device_id iris_secure_match[] = {
	{ .compatible = "qcom,sm8450-iris-secure-arp" },
	{ }
};
MODULE_DEVICE_TABLE(of, iris_secure_match);

struct platform_driver iris_secure_arp_driver = {
	.probe = iris_secure_probe,
	.remove = iris_secure_remove,
	.driver_managed_dma = true,
	.driver = {
		.name = "iris-secure-arp",
		.of_match_table = iris_secure_match,
		.suppress_bind_attrs = true,
	},
};

static void iris_secure_unregister(void *data)
{
	platform_device_unregister(data);
}

int iris_secure_arp_init(struct iris_core *core)
{
	struct device_node *np;
	struct platform_device *pdev;

	np = of_get_child_by_name(core->dev->of_node, "secure-arp");
	if (!np)
		return 0;
	pdev = of_platform_device_create(np, NULL, core->dev);
	of_node_put(np);
	if (!pdev)
		return -ENODEV;
	core->secure_arp_dev = &pdev->dev;
	return devm_add_action_or_reset(core->dev, iris_secure_unregister, pdev);
}
