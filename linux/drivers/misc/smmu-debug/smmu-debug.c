// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) Steve Jeong <steve@how2flow.net>
 *
 * SMMU debug framework — debugfs root and per-bus subdirs for runtime
 * SMMU test/benchmark drivers (smmu-test, smmu-bench).
 *
 * All directories are created lazily on first request and cached.
 * Concurrent first-touch from multiple consumer modules is serialised
 * by a single mutex.
 */

#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/module.h>
#include <linux/mutex.h>

#include "smmu-debug.h"

static DEFINE_MUTEX(smmu_debug_lock);
static struct dentry *smmu_debug_root_dir;
static struct dentry *smmu_debug_pcie;

static struct dentry *get_root_locked(void)
{
	if (!smmu_debug_root_dir)
		smmu_debug_root_dir = debugfs_create_dir("smmu", NULL);
	return smmu_debug_root_dir;
}

static struct dentry *get_bus_dir_locked(struct dentry **slot, const char *name)
{
	struct dentry *root;

	if (*slot)
		return *slot;

	root = get_root_locked();
	if (IS_ERR_OR_NULL(root))
		return NULL;

	*slot = debugfs_create_dir(name, root);
	return *slot;
}

struct dentry *smmu_debug_root(void)
{
	struct dentry *r;

	mutex_lock(&smmu_debug_lock);
	r = get_root_locked();
	mutex_unlock(&smmu_debug_lock);

	return r;
}
EXPORT_SYMBOL_GPL(smmu_debug_root);

struct dentry *smmu_debug_pcie_dir(void)
{
	struct dentry *r;

	mutex_lock(&smmu_debug_lock);
	r = get_bus_dir_locked(&smmu_debug_pcie, "pcie");
	mutex_unlock(&smmu_debug_lock);

	return r;
}
EXPORT_SYMBOL_GPL(smmu_debug_pcie_dir);

/*
 * Eagerly create /sys/kernel/debug/smmu/ at boot so the framework is
 * visible even before any consumer driver binds. Unlike the irq-debug
 * sibling, smmu test consumers (PCIe etc.) only probe when an
 * endpoint is enumerated, which may never happen on a given board —
 * so the lazy-on-first-touch model would leave the root invisible
 * and indistinguishable from "module not loaded".
 */
static int __init smmu_debug_init(void)
{
	struct dentry *r = smmu_debug_root();

	if (IS_ERR_OR_NULL(r)) {
		pr_warn("smmu-debug: failed to create debugfs root\n");
		return 0;
	}
	pr_info("smmu-debug: /sys/kernel/debug/smmu/ ready\n");
	return 0;
}
module_init(smmu_debug_init);

static void __exit smmu_debug_exit(void)
{
	mutex_lock(&smmu_debug_lock);
	debugfs_remove_recursive(smmu_debug_root_dir);
	smmu_debug_root_dir = NULL;
	smmu_debug_pcie = NULL;
	mutex_unlock(&smmu_debug_lock);
}
module_exit(smmu_debug_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Steve Jeong <steve@how2flow.net>");
MODULE_DESCRIPTION("Shared debugfs framework for SMMU runtime test drivers");
