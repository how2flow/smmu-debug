/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) Steve Jeong <steve@how2flow.net>
 *
 * Shared debugfs framework for runtime SMMU test/benchmark drivers.
 * Provides /sys/kernel/debug/smmu/ root and per-bus subdirs (pcie/, ...)
 * consumed by smmu-test and (future) smmu-bench drivers.
 */

#ifndef __SMMU_DEBUG_H
#define __SMMU_DEBUG_H

struct dentry;

struct dentry *smmu_debug_root(void);
struct dentry *smmu_debug_pcie_dir(void);

#endif /* __SMMU_DEBUG_H */
