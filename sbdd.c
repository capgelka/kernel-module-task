// SPDX-License-Identifier: GPL-2.0

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/bio.h>
#include <linux/bvec.h>
#include <linux/init.h>
#include <linux/wait.h>
#include <linux/stat.h>
#include <linux/slab.h>
#include <linux/numa.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/moduleparam.h>
#include <linux/spinlock_types.h>

#define SBDD_NAME              "sbdd"
#define SBDD_DST_MODE          (FMODE_WRITE | FMODE_READ)

struct sbdd {
	wait_queue_head_t       exitwait;
	atomic_t                deleting;
	atomic_t                refs_cnt;
	sector_t                capacity;
	struct gendisk          *gd;
	struct request_queue    *q;
	struct block_device     *dst_device;
};

static struct sbdd      __sbdd;
static int              __sbdd_major;
static char             *__dst_device_path;
static struct bio_set    __bio_set;


// static void bio_custom_endio(struct bio *bio)
// {
// 	pr_info("Original io finished: %p\n", bio);
// 	bio_endio(bio);
// 	bio_put(bio);
// 	if (atomic_dec_and_test(&__sbdd.refs_cnt))
// 		wake_up(&__sbdd.exitwait);
// }


static int init_dst_device(struct block_device **dst_device, char *path)
{
	struct block_device *bdev;
	int ret = 0;

	pr_info("opening %s device\n", path);
	bdev = blkdev_get_by_path(path, SBDD_DST_MODE, NULL);
	if (IS_ERR(bdev)) {
		ret = PTR_ERR(bdev);
		pr_err("Failed to open block device %s: %d\n", path, ret);
		return ret;
	}
	*dst_device = bdev;
	pr_info("device %s has been openned succesfully\n", path);
	return ret;
}


static blk_qc_t sbdd_make_request(struct request_queue *q, struct bio *bio)
{
	blk_qc_t ret = BLK_STS_OK;
	struct bio *new_bio;
	pr_info("Original bio request: 0x%p\n", bio);
	if (atomic_read(&__sbdd.deleting)) {
		bio_io_error(bio);
		return BLK_STS_IOERR;
	}

	if (!atomic_inc_not_zero(&__sbdd.refs_cnt)) {
		bio_io_error(bio);
		return BLK_STS_IOERR;
	}

	new_bio = bio_clone_fast(bio, GFP_KERNEL, &__bio_set);

	bio_set_dev(new_bio, __sbdd.dst_device);
	bio_chain(new_bio, bio);

	//bio->bi_end_io = bio_custom_endio;
	pr_info("Cloned bio request: 0x%p\n", new_bio);
	ret = submit_bio(new_bio);
	pr_info("Cloned bio request 0x%p has been submitted!\n", new_bio);
	if (ret != BLK_STS_OK && ret != BLK_QC_T_NONE)
		pr_warn("Bio redirection failed %d\n", ret);

	if (atomic_dec_and_test(&__sbdd.refs_cnt))
		wake_up(&__sbdd.exitwait);

	pr_debug("end of make request\n");
	return ret;
}

/*
 * There are no read or write operations. These operations are performed by
 * the request() function associated with the request queue of the disk.
 */
static const struct block_device_operations __sbdd_bdev_ops = {
	.owner = THIS_MODULE,
};

static int sbdd_create(void)
{
	int ret = 0;

	/*
	 * This call is somewhat redundant, but used anyways by tradition.
	 * The number is to be displayed in /proc/devices (0 for auto).
	 */
	pr_info("registering blkdev\n");
	__sbdd_major = register_blkdev(0, SBDD_NAME);
	if (__sbdd_major < 0) {
		pr_err("call register_blkdev() failed with %d\n", __sbdd_major);
		return -EBUSY;
	}

	memset(&__sbdd, 0, sizeof(struct sbdd));

	ret = bioset_init(&__bio_set, BIO_POOL_SIZE, 0, 0);
	if (ret)
		return ret;

	ret = init_dst_device(&__sbdd.dst_device, __dst_device_path);
	if (ret)
		return ret;

	__sbdd.capacity = get_capacity(__sbdd.dst_device->bd_disk);

	init_waitqueue_head(&__sbdd.exitwait);

	pr_info("allocating queue\n");
	__sbdd.q = blk_alloc_queue(GFP_KERNEL);
	if (!__sbdd.q) {
		pr_err("call blk_alloc_queue() failed\n");
		return -EINVAL;
	}
	blk_queue_make_request(__sbdd.q, sbdd_make_request);

	/* Configure queue */
	blk_queue_logical_block_size(
		__sbdd.q, bdev_logical_block_size(__sbdd.dst_device)
	);

	/* A disk must have at least one minor */
	pr_info("allocating disk\n");
	__sbdd.gd = alloc_disk(1);

	/* Configure gendisk */
	__sbdd.gd->queue = __sbdd.q;
	__sbdd.gd->major = __sbdd_major;
	__sbdd.gd->first_minor = 0;
	__sbdd.gd->fops = &__sbdd_bdev_ops;
	/* Represents name in /proc/partitions and /sys/block */
	scnprintf(__sbdd.gd->disk_name, DISK_NAME_LEN, SBDD_NAME);
	set_capacity(__sbdd.gd, __sbdd.capacity);
	atomic_set(&__sbdd.refs_cnt, 1);

	/*
	 * Allocating gd does not make it available, add_disk() required.
	 * After this call, gd methods can be called at any time. Should not be
	 * called before the driver is fully initialized and ready to process reqs.
	 */
	pr_info("adding disk\n");
	add_disk(__sbdd.gd);
	return ret;
}

static void sbdd_delete(void)
{
	atomic_set(&__sbdd.deleting, 1);
	atomic_dec_if_positive(&__sbdd.refs_cnt);
	wait_event(__sbdd.exitwait, !atomic_read(&__sbdd.refs_cnt));

	/* gd will be removed only after the last reference put */
	if (__sbdd.gd) {
		pr_info("deleting disk\n");
		del_gendisk(__sbdd.gd);
	}

	if (__sbdd.q) {
		pr_info("cleaning up queue\n");
		blk_cleanup_queue(__sbdd.q);
	}

	if (__sbdd.gd)
		put_disk(__sbdd.gd);

	if (__sbdd.dst_device) {
		pr_info("releasing %s device\n", __dst_device_path);
		blkdev_put(__sbdd.dst_device, SBDD_DST_MODE);
	}

	
	bioset_exit(&__bio_set);


	memset(&__sbdd, 0, sizeof(struct sbdd));

	if (__sbdd_major > 0) {
		pr_info("unregistering blkdev\n");
		unregister_blkdev(__sbdd_major, SBDD_NAME);
		__sbdd_major = 0;
	}
}

/*
 * Note __init is for the kernel to drop this function after
 * initialization complete making its memory available for other uses.
 * There is also __initdata note, same but used for variables.
 */
static int __init sbdd_init(void)
{
	int ret = 0;

	pr_info("starting initialization...\n");
	pr_info("device to work with: %s\n", __dst_device_path);
	ret = sbdd_create();

	if (ret) {
		pr_warn("initialization failed\n");
		sbdd_delete();
	} else {
		pr_info("initialization complete\n");
	}

	return ret;
}

/*
 * Note __exit is for the compiler to place this code in a special ELF section.
 * Sometimes such functions are simply discarded (e.g. when module is built
 * directly into the kernel). There is also __exitdata note.
 */
static void __exit sbdd_exit(void)
{
	pr_info("exiting...\n");
	sbdd_delete();
	pr_info("exiting complete\n");
}

/* Called on module loading. Is mandatory. */
module_init(sbdd_init);

/* Called on module unloading. Unloading module is not allowed without it. */
module_exit(sbdd_exit);

/* Set desired proxy targer device with insmod */
module_param_named(device, __dst_device_path, charp, 0444);
MODULE_PARM_DESC(__dst_device_path, "Path to device file for bio redirection");

/* Note for the kernel: a free license module. A warning will be outputted without it. */
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Simple Block Device Driver");
