/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QCOM_SECURE_IOMMU_H
#define QCOM_SECURE_IOMMU_H
#include <linux/kconfig.h>
#include <linux/types.h>
struct iommu_domain;
#if IS_REACHABLE(CONFIG_ARM_SMMU)
bool qcom_smmu_secure_domain_healthy(struct iommu_domain *domain);
#else
static inline bool qcom_smmu_secure_domain_healthy(struct iommu_domain *domain)
{
	return false;
}
#endif
#endif
