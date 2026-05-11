// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) Steve Jeong <steve@how2flow.net>
 *
 * ARM SMMUv3 PCIe runtime test.
 *
 * Bound manually (the id_table is empty) via:
 *   echo VVVV DDDD > /sys/bus/pci/drivers/smmu-test-pcie/new_id
 *   echo 0000:bb:dd.f > /sys/bus/pci/drivers/<orig>/unbind
 *   echo 0000:bb:dd.f > /sys/bus/pci/drivers/smmu-test-pcie/bind
 *
 * Exposes /sys/kernel/debug/smmu/pcie/pcie-test<N>/:
 *   - info:       RO, BDF, vendor:device, IOMMU group, attached domain
 *                 state, two embedded test buffers, op counters and last
 *                 dma_memcpy result.
 *   - attach:     W,  alloc UNMANAGED paging domain via
 *                 iommu_paging_domain_alloc() and attach to the device.
 *                 STE programming + CMDQ sync happen inside the SMMU
 *                 driver as a side effect.
 *   - detach:     W,  drop any active mappings, release the dmaengine
 *                 channel, detach, free the test domain. Device returns
 *                 to its default (DMA/identity) domain.
 *   - map:        W,  "<iova_hex> <size_hex> [prot] [buf_id]" — iommu_map
 *                 of the embedded test buffer at the requested IOVA.
 *                 buf_id is 0 (default, src buffer) or 1 (dst buffer).
 *                 Multiple mappings tracked in a list.
 *   - unmap:      W,  "<iova_hex> <size_hex>" — iommu_unmap and remove
 *                 from list (size must match an existing entry).
 *   - iova2phys:  RW, write IOVA to query, read shows last
 *                 "iova:0xX phys:0xY" pair from iommu_iova_to_phys().
 *   - dma_memcpy: W,  "<src_iova> <dst_iova> <size>" — drive an actual
 *                 DMA via the dmaengine framework using a memcpy-capable
 *                 channel that lives in the same IOMMU group as this
 *                 device. The DMA addresses passed to the engine are the
 *                 caller-provided IOVAs, so the SMMU translates them
 *                 through our UNMANAGED domain. Result (PASS/FAIL) is
 *                 verified by memcmp and exposed via info.
 *
 * dma_memcpy fills the src buffer's mapped range with a deterministic
 * byte sequence, runs the DMA, then memcmps src vs dst kernel views to
 * confirm SMMU translated correctly. Triggering an unmapped destination
 * is a valid fault-injection path: the SMMU will produce an event and
 * the DMA will time out.
 */

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/dmaengine.h>
#include <linux/err.h>
#include <linux/gfp.h>
#include <linux/iommu.h>
#include <linux/list.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "smmu-debug.h"

#define DRIVER_NAME		"smmu-test-pcie"

/*
 * 64 KiB physically contiguous test buffer (order 4) per slot. Two
 * slots: buffer 0 = DMA source, buffer 1 = DMA destination. Big enough
 * for multi-page IOVA mapping experiments without stressing the page
 * allocator. Caller may iommu_map any sub-range up to TEST_BUF_SIZE.
 */
#define TEST_BUF_ORDER		4
#define TEST_BUF_SIZE		(PAGE_SIZE << TEST_BUF_ORDER)
#define TEST_NUM_BUFS		2

#define DMA_TIMEOUT_MS		2000

struct smmu_test_pcie_dev {
	struct pci_dev		*pdev;
	struct device		*dev;
	struct dentry		*dir;

	struct mutex		lock;
	struct iommu_domain	*domain;	/* NULL = not attached */
	struct list_head	mappings;
	unsigned int		nr_mappings;

	/* Two embedded test buffers used as physical targets of iommu_map. */
	void			*buf_va[TEST_NUM_BUFS];
	phys_addr_t		buf_pa[TEST_NUM_BUFS];

	/* Lazily acquired memcpy-capable channel sharing this dev's group. */
	struct dma_chan		*dma_chan;

	/* Last iova2phys query, protected by ->lock. */
	bool			last_valid;
	u64			last_iova;
	phys_addr_t		last_phys;

	/* Last dma_memcpy result, protected by ->lock. */
	bool			dma_has_result;
	bool			dma_pass;
	size_t			dma_xfer_size;
	char			dma_status[64];

	atomic64_t		attach_cnt;
	atomic64_t		detach_cnt;
	atomic64_t		map_cnt;
	atomic64_t		unmap_cnt;
	atomic64_t		dma_cnt;
};

struct smmu_test_mapping {
	struct list_head	node;
	unsigned long		iova;
	size_t			size;
	int			prot;
	unsigned int		buf_id;		/* 0 = src buf, 1 = dst buf */
	phys_addr_t		pa;		/* idev->buf_pa[buf_id] */
	void			*va;		/* idev->buf_va[buf_id] */
};

static atomic_t smmu_test_pcie_index = ATOMIC_INIT(0);

/* ===== test buffers ===== */

static int test_bufs_alloc(struct smmu_test_pcie_dev *idev)
{
	unsigned int i;

	for (i = 0; i < TEST_NUM_BUFS; i++) {
		unsigned long va = __get_free_pages(GFP_KERNEL, TEST_BUF_ORDER);

		if (!va)
			goto err;
		idev->buf_va[i] = (void *)va;
		idev->buf_pa[i] = virt_to_phys(idev->buf_va[i]);
	}
	return 0;
err:
	while (i-- > 0) {
		free_pages((unsigned long)idev->buf_va[i], TEST_BUF_ORDER);
		idev->buf_va[i] = NULL;
	}
	return -ENOMEM;
}

static void test_bufs_free(struct smmu_test_pcie_dev *idev)
{
	unsigned int i;

	for (i = 0; i < TEST_NUM_BUFS; i++) {
		if (idev->buf_va[i]) {
			free_pages((unsigned long)idev->buf_va[i],
				   TEST_BUF_ORDER);
			idev->buf_va[i] = NULL;
		}
	}
}

/* ===== mapping list (caller holds idev->lock) ===== */

static struct smmu_test_mapping *
mapping_find_locked(struct smmu_test_pcie_dev *idev, unsigned long iova)
{
	struct smmu_test_mapping *m;

	list_for_each_entry(m, &idev->mappings, node)
		if (m->iova == iova)
			return m;
	return NULL;
}

static void mappings_drop_all_locked(struct smmu_test_pcie_dev *idev)
{
	struct smmu_test_mapping *m, *tmp;

	list_for_each_entry_safe(m, tmp, &idev->mappings, node) {
		if (idev->domain)
			iommu_unmap(idev->domain, m->iova, m->size);
		list_del(&m->node);
		kfree(m);
	}
	idev->nr_mappings = 0;
}

/* ===== dmaengine channel filter ===== */

/*
 * Pick a memcpy channel whose owner device shares this idev's IOMMU
 * group. With our domain attached to idev->dev, every device in the
 * same group sees the same translation, so DMAs from such a channel
 * use our mappings.
 */
static bool match_chan_iommu_group(struct dma_chan *chan, void *priv)
{
	struct smmu_test_pcie_dev *idev = priv;
	struct iommu_group *cgrp, *tgrp;
	bool match = false;

	cgrp = iommu_group_get(chan->device->dev);
	tgrp = iommu_group_get(idev->dev);
	if (cgrp && tgrp && cgrp == tgrp)
		match = true;
	if (cgrp)
		iommu_group_put(cgrp);
	if (tgrp)
		iommu_group_put(tgrp);
	return match;
}

/* ===== attach / detach (caller holds idev->lock) ===== */

static int do_attach_locked(struct smmu_test_pcie_dev *idev)
{
	struct iommu_domain *dom;
	int ret;

	if (idev->domain)
		return -EBUSY;

	dom = iommu_paging_domain_alloc(idev->dev);
	if (IS_ERR(dom))
		return PTR_ERR(dom);

	ret = iommu_attach_device(dom, idev->dev);
	if (ret) {
		iommu_domain_free(dom);
		return ret;
	}

	idev->domain = dom;
	idev->last_valid = false;
	atomic64_inc(&idev->attach_cnt);
	return 0;
}

static int do_detach_locked(struct smmu_test_pcie_dev *idev)
{
	if (!idev->domain)
		return -ENOENT;

	mappings_drop_all_locked(idev);

	if (idev->dma_chan) {
		dmaengine_terminate_sync(idev->dma_chan);
		dma_release_channel(idev->dma_chan);
		idev->dma_chan = NULL;
	}

	iommu_detach_device(idev->domain, idev->dev);
	iommu_domain_free(idev->domain);
	idev->domain = NULL;
	idev->last_valid = false;
	atomic64_inc(&idev->detach_cnt);
	return 0;
}

/* ===== file ops: attach / detach ===== */

static ssize_t attach_write(struct file *file, const char __user *ubuf,
			    size_t count, loff_t *ppos)
{
	struct smmu_test_pcie_dev *idev = file->private_data;
	int ret;

	mutex_lock(&idev->lock);
	ret = do_attach_locked(idev);
	mutex_unlock(&idev->lock);

	if (ret)
		dev_err(idev->dev, "attach failed: %d\n", ret);
	return ret ? ret : count;
}

static const struct file_operations attach_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.write	= attach_write,
};

static ssize_t detach_write(struct file *file, const char __user *ubuf,
			    size_t count, loff_t *ppos)
{
	struct smmu_test_pcie_dev *idev = file->private_data;
	int ret;

	mutex_lock(&idev->lock);
	ret = do_detach_locked(idev);
	mutex_unlock(&idev->lock);

	if (ret)
		dev_err(idev->dev, "detach failed: %d\n", ret);
	return ret ? ret : count;
}

static const struct file_operations detach_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.write	= detach_write,
};

/* ===== generic ulong list parser ===== */

/*
 * Parse up to @max space/tab-separated unsigned longs from @line. @line
 * is consumed. Returns the count actually parsed (0..max), or -EINVAL
 * on a malformed token.
 */
static int parse_ulongs(char *line, unsigned long *vals, int max)
{
	char *tok;
	int n = 0;

	while (n < max) {
		tok = strsep(&line, " \t");
		if (!tok)
			break;
		if (!*tok)
			continue;
		if (kstrtoul(tok, 0, &vals[n]))
			return -EINVAL;
		n++;
	}
	return n;
}

/* ===== file ops: map / unmap ===== */

static ssize_t map_write(struct file *file, const char __user *ubuf,
			 size_t count, loff_t *ppos)
{
	struct smmu_test_pcie_dev *idev = file->private_data;
	char buf[64];
	unsigned long vals[4];
	unsigned long iova, size;
	unsigned int buf_id;
	int prot, n_vals;
	struct smmu_test_mapping *m;
	size_t n;
	int ret;

	n = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, n))
		return -EFAULT;
	buf[n] = '\0';

	n_vals = parse_ulongs(strim(buf), vals, ARRAY_SIZE(vals));
	if (n_vals < 2)
		return n_vals < 0 ? n_vals : -EINVAL;

	iova   = vals[0];
	size   = vals[1];
	prot   = (n_vals >= 3) ? (int)vals[2] : (IOMMU_READ | IOMMU_WRITE);
	buf_id = (n_vals >= 4) ? (unsigned int)vals[3] : 0;

	if (size == 0 || size > TEST_BUF_SIZE)
		return -EINVAL;
	if (buf_id >= TEST_NUM_BUFS)
		return -EINVAL;

	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;

	mutex_lock(&idev->lock);
	if (!idev->domain) {
		ret = -ENOTCONN;
		goto out_unlock;
	}
	if (mapping_find_locked(idev, iova)) {
		ret = -EEXIST;
		goto out_unlock;
	}
	ret = iommu_map(idev->domain, iova, idev->buf_pa[buf_id], size,
			prot, GFP_KERNEL);
	if (ret)
		goto out_unlock;

	m->iova   = iova;
	m->size   = size;
	m->prot   = prot;
	m->buf_id = buf_id;
	m->pa     = idev->buf_pa[buf_id];
	m->va     = idev->buf_va[buf_id];
	list_add_tail(&m->node, &idev->mappings);
	idev->nr_mappings++;
	atomic64_inc(&idev->map_cnt);
	mutex_unlock(&idev->lock);
	return count;

out_unlock:
	mutex_unlock(&idev->lock);
	kfree(m);
	return ret;
}

static const struct file_operations map_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.write	= map_write,
};

static ssize_t unmap_write(struct file *file, const char __user *ubuf,
			   size_t count, loff_t *ppos)
{
	struct smmu_test_pcie_dev *idev = file->private_data;
	char buf[48];
	unsigned long vals[2];
	unsigned long iova, size;
	struct smmu_test_mapping *m;
	size_t unmapped, n;
	int n_vals, ret;

	n = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, n))
		return -EFAULT;
	buf[n] = '\0';

	n_vals = parse_ulongs(strim(buf), vals, ARRAY_SIZE(vals));
	if (n_vals != 2)
		return n_vals < 0 ? n_vals : -EINVAL;
	iova = vals[0];
	size = vals[1];

	mutex_lock(&idev->lock);
	if (!idev->domain) {
		ret = -ENOTCONN;
		goto out;
	}
	m = mapping_find_locked(idev, iova);
	if (!m || m->size != size) {
		ret = -ENOENT;
		goto out;
	}
	unmapped = iommu_unmap(idev->domain, iova, size);
	if (unmapped != size) {
		dev_warn(idev->dev,
			 "iommu_unmap returned %zu (expected %lu)\n",
			 unmapped, size);
		ret = -EIO;
		goto out;
	}
	list_del(&m->node);
	kfree(m);
	idev->nr_mappings--;
	atomic64_inc(&idev->unmap_cnt);
	ret = 0;
out:
	mutex_unlock(&idev->lock);
	return ret ? ret : count;
}

static const struct file_operations unmap_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.write	= unmap_write,
};

/* ===== file ops: iova2phys ===== */

static ssize_t iova2phys_write(struct file *file, const char __user *ubuf,
			       size_t count, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct smmu_test_pcie_dev *idev = s->private;
	u64 iova;
	int ret;

	ret = kstrtou64_from_user(ubuf, count, 0, &iova);
	if (ret)
		return ret;

	mutex_lock(&idev->lock);
	if (!idev->domain) {
		mutex_unlock(&idev->lock);
		return -ENOTCONN;
	}
	idev->last_iova  = iova;
	idev->last_phys  = iommu_iova_to_phys(idev->domain, iova);
	idev->last_valid = true;
	mutex_unlock(&idev->lock);

	return count;
}

static int iova2phys_show(struct seq_file *s, void *unused)
{
	struct smmu_test_pcie_dev *idev = s->private;

	mutex_lock(&idev->lock);
	if (!idev->last_valid)
		seq_puts(s, "no query yet (write IOVA hex to this file)\n");
	else
		seq_printf(s, "iova:0x%llx phys:0x%llx\n",
			   idev->last_iova, (u64)idev->last_phys);
	mutex_unlock(&idev->lock);
	return 0;
}

static int iova2phys_open(struct inode *inode, struct file *file)
{
	return single_open(file, iova2phys_show, inode->i_private);
}

static const struct file_operations iova2phys_fops = {
	.owner		= THIS_MODULE,
	.open		= iova2phys_open,
	.read		= seq_read,
	.write		= iova2phys_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* ===== file ops: dma_memcpy ===== */

static void dma_complete_cb(void *param)
{
	complete(param);
}

/*
 * caller holds idev->lock; transitions ->dma_chan from NULL to a
 * memcpy-capable channel that shares this idev's IOMMU group.
 */
static int acquire_dma_chan_locked(struct smmu_test_pcie_dev *idev)
{
	dma_cap_mask_t mask;
	struct dma_chan *chan;

	if (idev->dma_chan)
		return 0;

	dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY, mask);

	chan = dma_request_channel(mask, match_chan_iommu_group, idev);
	if (!chan)
		return -ENODEV;

	idev->dma_chan = chan;
	dev_info(idev->dev, "dma_memcpy channel acquired: %s\n",
		 dma_chan_name(chan));
	return 0;
}

/*
 * Run a single memcpy via the acquired dmaengine channel. The src/dst
 * arguments are caller-supplied IOVAs; the SMMU translates them via
 * our UNMANAGED domain. Verification:
 *   1. fill src kernel-VA with a deterministic byte sequence
 *   2. clear dst kernel-VA
 *   3. submit memcpy, wait for completion (DMA_TIMEOUT_MS)
 *   4. memcmp src vs dst kernel-VA
 *
 * No explicit CPU-side cache management: arm64 + arm-smmu-v3 systems
 * are typically configured coherent (the SMMU driver propagates
 * IDR0.COHACC to the io-pgtable). If a memcmp mismatch is ever seen
 * here on a non-coherent setup, plumb in the appropriate cache ops.
 *
 * Caller holds idev->lock.
 */
static int do_dma_memcpy_locked(struct smmu_test_pcie_dev *idev,
				u64 src_iova, u64 dst_iova, size_t size,
				struct smmu_test_mapping *src_m,
				struct smmu_test_mapping *dst_m)
{
	struct dma_async_tx_descriptor *tx;
	dma_cookie_t cookie;
	DECLARE_COMPLETION_ONSTACK(done);
	unsigned long left;
	enum dma_status status;
	size_t i;
	int ret;

	ret = acquire_dma_chan_locked(idev);
	if (ret)
		return ret;

	/* Fill src with a deterministic pattern; clear dst. */
	if (src_m) {
		u8 *p = src_m->va;

		for (i = 0; i < size; i++)
			p[i] = (u8)(i & 0xff);
	}
	if (dst_m)
		memset(dst_m->va, 0, size);

	tx = dmaengine_prep_dma_memcpy(idev->dma_chan,
				       (dma_addr_t)dst_iova,
				       (dma_addr_t)src_iova,
				       size,
				       DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!tx)
		return -EIO;

	tx->callback = dma_complete_cb;
	tx->callback_param = &done;

	cookie = dmaengine_submit(tx);
	if (dma_submit_error(cookie))
		return -EIO;

	dma_async_issue_pending(idev->dma_chan);

	left = wait_for_completion_timeout(&done,
					   msecs_to_jiffies(DMA_TIMEOUT_MS));
	if (left == 0) {
		dmaengine_terminate_sync(idev->dma_chan);
		return -ETIMEDOUT;
	}

	status = dma_async_is_tx_complete(idev->dma_chan, cookie, NULL, NULL);
	if (status != DMA_COMPLETE)
		return -EIO;

	if (!src_m || !dst_m)
		return 0;	/* no kernel view to verify against */

	if (memcmp(src_m->va, dst_m->va, size) != 0)
		return -EIO;
	return 0;
}

static ssize_t dma_memcpy_write(struct file *file, const char __user *ubuf,
				size_t count, loff_t *ppos)
{
	struct smmu_test_pcie_dev *idev = file->private_data;
	char buf[64];
	unsigned long vals[3];
	struct smmu_test_mapping *src_m, *dst_m;
	u64 src_iova, dst_iova;
	size_t size, n;
	int n_vals, ret;

	n = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, n))
		return -EFAULT;
	buf[n] = '\0';

	n_vals = parse_ulongs(strim(buf), vals, ARRAY_SIZE(vals));
	if (n_vals != 3)
		return n_vals < 0 ? n_vals : -EINVAL;
	src_iova = vals[0];
	dst_iova = vals[1];
	size     = vals[2];

	if (size == 0 || size > TEST_BUF_SIZE)
		return -EINVAL;

	mutex_lock(&idev->lock);
	if (!idev->domain) {
		ret = -ENOTCONN;
		goto record;
	}

	src_m = mapping_find_locked(idev, src_iova);
	dst_m = mapping_find_locked(idev, dst_iova);
	/*
	 * Missing or undersized mappings are not rejected here — fault
	 * injection is a valid use. do_dma_memcpy_locked will see what
	 * the SMMU does (typically translation fault → DMA timeout).
	 */
	if (src_m && size > src_m->size) {
		ret = -EINVAL;
		goto record;
	}
	if (dst_m && size > dst_m->size) {
		ret = -EINVAL;
		goto record;
	}

	ret = do_dma_memcpy_locked(idev, src_iova, dst_iova, size,
				   src_m, dst_m);
record:
	idev->dma_has_result = true;
	idev->dma_pass       = (ret == 0);
	idev->dma_xfer_size  = size;
	switch (ret) {
	case 0:
		strscpy(idev->dma_status, "ok", sizeof(idev->dma_status));
		break;
	case -ETIMEDOUT:
		strscpy(idev->dma_status, "timeout (likely SMMU fault)",
			sizeof(idev->dma_status));
		break;
	case -ENOTCONN:
		strscpy(idev->dma_status, "no domain attached",
			sizeof(idev->dma_status));
		break;
	case -ENODEV:
		strscpy(idev->dma_status, "no matching dma channel",
			sizeof(idev->dma_status));
		break;
	default:
		scnprintf(idev->dma_status, sizeof(idev->dma_status),
			  "error %d", ret);
		break;
	}
	atomic64_inc(&idev->dma_cnt);
	mutex_unlock(&idev->lock);

	return ret ? ret : count;
}

static const struct file_operations dma_memcpy_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.write	= dma_memcpy_write,
};

/* ===== file ops: info ===== */

static int info_show(struct seq_file *s, void *unused)
{
	struct smmu_test_pcie_dev *idev = s->private;
	struct iommu_group *grp = iommu_group_get(idev->dev);
	int gid = grp ? iommu_group_id(grp) : -1;
	unsigned int i;

	seq_puts(s,   "type:        PCIe SMMU test\n");
	seq_printf(s, "device:      %s\n", dev_name(idev->dev));
	seq_printf(s, "pci_bdf:     %04x:%02x:%02x.%d\n",
		   pci_domain_nr(idev->pdev->bus),
		   idev->pdev->bus->number,
		   PCI_SLOT(idev->pdev->devfn),
		   PCI_FUNC(idev->pdev->devfn));
	seq_printf(s, "vendor:dev:  %04x:%04x\n",
		   idev->pdev->vendor, idev->pdev->device);
	seq_printf(s, "iommu_group: %d\n", gid);
	for (i = 0; i < TEST_NUM_BUFS; i++)
		seq_printf(s, "buffer%u_pa:  0x%llx\n",
			   i, (u64)idev->buf_pa[i]);
	seq_printf(s, "buffer_size: %zu bytes\n", (size_t)TEST_BUF_SIZE);

	mutex_lock(&idev->lock);
	seq_printf(s, "attached:    %s\n", idev->domain ? "yes" : "no");
	if (idev->domain) {
		seq_printf(s, "domain_type: 0x%x\n", idev->domain->type);
		seq_printf(s, "pgsize_bmap: 0x%lx\n",
			   idev->domain->pgsize_bitmap);
	}
	seq_printf(s, "mappings:    %u\n", idev->nr_mappings);
	if (idev->dma_chan)
		seq_printf(s, "dma_chan:    %s\n",
			   dma_chan_name(idev->dma_chan));
	if (idev->dma_has_result) {
		seq_printf(s, "dma_last:    %s, size=%zu, status=%s\n",
			   idev->dma_pass ? "PASS" : "FAIL",
			   idev->dma_xfer_size, idev->dma_status);
	}
	mutex_unlock(&idev->lock);

	seq_printf(s, "attach_cnt:  %lld\n",
		   (long long)atomic64_read(&idev->attach_cnt));
	seq_printf(s, "detach_cnt:  %lld\n",
		   (long long)atomic64_read(&idev->detach_cnt));
	seq_printf(s, "map_cnt:     %lld\n",
		   (long long)atomic64_read(&idev->map_cnt));
	seq_printf(s, "unmap_cnt:   %lld\n",
		   (long long)atomic64_read(&idev->unmap_cnt));
	seq_printf(s, "dma_cnt:     %lld\n",
		   (long long)atomic64_read(&idev->dma_cnt));

	if (grp)
		iommu_group_put(grp);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(info);

/* ===== probe / remove ===== */

static void debugfs_register(struct smmu_test_pcie_dev *idev)
{
	struct dentry *parent = smmu_debug_pcie_dir();
	char name[32];

	if (IS_ERR_OR_NULL(parent))
		return;

	snprintf(name, sizeof(name), "pcie-test%d",
		 atomic_fetch_inc(&smmu_test_pcie_index));

	idev->dir = debugfs_create_dir(name, parent);
	debugfs_create_file("info",       0444, idev->dir, idev, &info_fops);
	debugfs_create_file("attach",     0200, idev->dir, idev, &attach_fops);
	debugfs_create_file("detach",     0200, idev->dir, idev, &detach_fops);
	debugfs_create_file("map",        0200, idev->dir, idev, &map_fops);
	debugfs_create_file("unmap",      0200, idev->dir, idev, &unmap_fops);
	debugfs_create_file("iova2phys",  0644, idev->dir, idev, &iova2phys_fops);
	debugfs_create_file("dma_memcpy", 0200, idev->dir, idev, &dma_memcpy_fops);
}

static int smmu_test_pcie_probe(struct pci_dev *pdev,
				const struct pci_device_id *id)
{
	struct smmu_test_pcie_dev *idev;
	int ret;

	if (!device_iommu_mapped(&pdev->dev))
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "device has no IOMMU; refusing to bind\n");

	idev = devm_kzalloc(&pdev->dev, sizeof(*idev), GFP_KERNEL);
	if (!idev)
		return -ENOMEM;

	idev->pdev = pdev;
	idev->dev  = &pdev->dev;
	mutex_init(&idev->lock);
	INIT_LIST_HEAD(&idev->mappings);
	pci_set_drvdata(pdev, idev);

	ret = test_bufs_alloc(idev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to allocate test buffers\n");

	debugfs_register(idev);

	pci_info(pdev,
		 "smmu-test-pcie ready: buf0_pa=0x%llx buf1_pa=0x%llx size=%zu\n",
		 (u64)idev->buf_pa[0], (u64)idev->buf_pa[1],
		 (size_t)TEST_BUF_SIZE);
	return 0;
}

static void smmu_test_pcie_remove(struct pci_dev *pdev)
{
	struct smmu_test_pcie_dev *idev = pci_get_drvdata(pdev);

	debugfs_remove_recursive(idev->dir);

	mutex_lock(&idev->lock);
	if (idev->domain)
		do_detach_locked(idev);
	mutex_unlock(&idev->lock);

	test_bufs_free(idev);
}

/*
 * id_table is intentionally NULL (no static IDs, no MODULE_DEVICE_TABLE).
 * A sentinel-only id_table {{}} would be rejected by the new_id sysfs
 * handler in drivers/pci/pci-driver.c (the loop bailing out with
 * -EINVAL when no entry matches). Following the vfio-pci pattern, we
 * leave id_table NULL so new_id can register dynamic IDs freely.
 */
static struct pci_driver smmu_test_pcie_driver = {
	.name		= DRIVER_NAME,
	.probe		= smmu_test_pcie_probe,
	.remove		= smmu_test_pcie_remove,
};
module_pci_driver(smmu_test_pcie_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Steve Jeong <steve@how2flow.net>");
MODULE_DESCRIPTION("ARM SMMUv3 PCIe runtime test via IOMMU API + dmaengine");
