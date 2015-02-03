/* Copyright (c) 2014, Sourish Mazumder (sourish.mazumder@gmail.com) */

#include <sys/param.h>
#include <sys/malloc.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/module.h>
#include <sys/ioccom.h>
#include <sys/kernel.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/linker.h>
#include <sys/uio.h>
#include <sys/bio.h>
#include <sys/kthread.h>
#include <sys/sched.h>
#include <sys/devicestat.h>
#include <sys/disk.h>
#include <geom/geom.h>
#include <vm/vm.h>
#include <vm/pmap.h>
#include <machine/pc/bios.h>
#include <machine/metadata.h>

#include <sys/nvram_ioctl.h>

#define SMAP_TYPE_ARXCIS        12

#define NVRAM_BLK_DEV_NAME	"nvram_blk_dev"
#define NVRAM_SMAP_TYPE		SMAP_TYPE_ARXCIS
#define NVRAM_GEOM_CLASS	"nvram::geom"

#ifdef MALLOC_DECLARE
MALLOC_DECLARE(M_NVRAM);
#endif /* MALLOC_DECLARE */
MALLOC_DEFINE(M_NVRAM, "m_nvram", "nvram memory");

static struct cdev		*nvram_dev, *nvram_blk_dev;
static struct bios_smap		*nvram_smap;
static void			*nvram_start_addr;
static struct g_geom		*gp;
static struct g_provider	*pp;
static struct devstat		*nvram_devstat;
static struct bio_queue_head	nvram_bioq;
static struct mtx		nvram_bioq_mtx;
#define BIO_Q_READY		0
#define BIO_Q_FINISH_PROCESSING	1
#define BIO_Q_PROCESSING_DONE	2
static int			nvram_bioq_state;
#define DISK_TYPE_NONE		0
#define DISK_TYPE_GEOM		1
#define DISK_TYPE_DEVFS		2
static int			disk_type;
#define DEVICE_NOT_INIT		0
#define DEVICE_INIT		1
static int			device_initialized;

#define SECTOR_SIZE		DEV_BSIZE
#define BLOCK_SIZE		4096

struct proc			*nvram_proc;

typedef struct timespec		timespec_t;

static g_start_t		nvram_geom_blk_dev_start;
static g_access_t		nvram_geom_blk_dev_access;

static struct g_class nvram_geom_class = {
	.name			= NVRAM_GEOM_CLASS,
	.start			= nvram_geom_blk_dev_start,
	.access			= nvram_geom_blk_dev_access,
	.version		= G_VERSION
};
DECLARE_GEOM_CLASS(nvram_geom_class, nvram_geom);

static inline void
nvram_op(void *dst, void *src, size_t len)
{
	memcpy(dst, src, len);
}

static int
nvram_blk_dev_open(struct cdev *dev, int oflags, int devtype, struct thread *td)
{
	return (0);
}

static int
nvram_blk_dev_close(struct cdev *dev, int oflags, int devtype, struct thread *td)
{
	return (0);
}

static int
nvram_blk_dev_read(struct cdev *dev, struct uio *uio, int ioflag)
{
	int error	= 0;

	error = uiomove(nvram_start_addr + uio->uio_offset, uio->uio_resid, uio);

	return (error);
}

static int
nvram_blk_dev_write(struct cdev *dev, struct uio *uio, int ioflag)
{
	int error	= 0;

	error = uiomove(nvram_start_addr + uio->uio_offset, uio->uio_resid, uio);

	return (error);
}

static int
nvram_blk_dev_ioctl(struct cdev *dev, u_long cmd, caddr_t addr, int flag,
		struct thread *td)
{
	int error = 0;
	off_t offset, length;

	switch (cmd) {
		case DIOCGSECTORSIZE:
			*(u_int *)addr = SECTOR_SIZE;
			break;
		case DIOCGMEDIASIZE:
			*(off_t *)addr = nvram_smap->length;
			break;
		case DIOCGFLUSH:
			break;
		case DIOCGDELETE:
			offset = ((off_t *)addr)[0];
			length = ((off_t *)addr)[1];
			if ((offset % SECTOR_SIZE != 0) || (length % SECTOR_SIZE != 0))
				error = EINVAL;
			break;
		case DIOCGSTRIPESIZE:
			*(off_t *)addr = BLOCK_SIZE;
			break;
		case DIOCGSTRIPEOFFSET:
			*(off_t *)addr = 0;
			break;
		default:
			error = ENOIOCTL;
	}

	return (error);
}

void
nvram_blk_dev_strategy(struct bio *bp)
{
	return;
}

static struct cdevsw nvram_blk_dev_cdevsw = {
	.d_version				= D_VERSION,
	.d_open					= nvram_blk_dev_open,
	.d_close				= nvram_blk_dev_close,
	.d_read					= nvram_blk_dev_read,
	.d_write				= nvram_blk_dev_write,
	.d_ioctl				= nvram_blk_dev_ioctl,
	.d_strategy				= nvram_blk_dev_strategy,
	.d_name					= NVRAM_BLK_DEV_NAME,
	.d_flags				= D_DISK | D_TRACKCLOSE
};

static int
nvram_geom_blk_dev_access(struct g_provider *pp, int acr, int acw, int ace)
{
	g_topology_assert();
	return (0);
}

static void
nvram_geom_blk_dev_start(struct bio *bp)
{
	bool first;

	switch (bp->bio_cmd) {
		case BIO_READ:
		case BIO_WRITE:
		case BIO_FLUSH:
		case BIO_DELETE:
			mtx_lock(&nvram_bioq_mtx);
			first = (bioq_first(&nvram_bioq) == NULL);
			if (nvram_bioq_state == BIO_Q_READY) {
				if ((bp->bio_cmd == BIO_READ) ||
						(bp->bio_cmd == BIO_WRITE))
					devstat_start_transaction_bio(nvram_devstat, bp);
				bioq_insert_tail(&nvram_bioq, bp);
			}
			mtx_unlock(&nvram_bioq_mtx);
			if (first)
				wakeup_one(&nvram_bioq);
			break;
		case BIO_GETATTR:
		default:
			g_io_deliver(bp, EOPNOTSUPP);
			break;
	}
}

static void
nvram_geom_blk_dev_worker(void *arg)
{
	struct bio *bp;
	int error;

	thread_lock(curthread);
	sched_prio(curthread, PRIBIO);
	thread_unlock(curthread);

	for (;;) {
		error = 0;
		mtx_lock(&nvram_bioq_mtx);
		bp = bioq_takefirst(&nvram_bioq);
		if (bp == NULL) {
			if (nvram_bioq_state == BIO_Q_FINISH_PROCESSING) {
				nvram_bioq_state = BIO_Q_PROCESSING_DONE;
				wakeup(&nvram_bioq_state);
				mtx_unlock(&nvram_bioq_mtx);
				/* exit the worker thread */
				kthread_exit();
			}
			msleep(&nvram_bioq, &nvram_bioq_mtx, PRIBIO | PDROP,
					"nvram:bioq", 0);
			continue;
		}
		mtx_unlock(&nvram_bioq_mtx);
		switch (bp->bio_cmd) {
			case BIO_FLUSH:
				break;
			case BIO_READ:
				nvram_op(bp->bio_data, nvram_start_addr + bp->bio_offset,
						bp->bio_length);
				break;
			case BIO_WRITE:
				nvram_op(nvram_start_addr + bp->bio_offset, bp->bio_data,
						bp->bio_length);
				break;
			case BIO_DELETE:
				break;
			default:
				error = EOPNOTSUPP;
				break;
		}
		bp->bio_completed = bp->bio_length;
		bp->bio_resid = 0;
		if ((bp->bio_cmd == BIO_READ) ||
				(bp->bio_cmd == BIO_WRITE))
			devstat_end_transaction_bio(nvram_devstat, bp);
		g_io_deliver(bp, error);
	}
}

static void
nvram_init()
{
	caddr_t	kmdp;
	struct bios_smap *smapbase, *smap, *smapend;
	uint32_t smapsize;

	kmdp = preload_search_by_type("elf kernel");
	if (kmdp == NULL)
		kmdp = preload_search_by_type("elf64 kernel");
	if (kmdp == NULL)
		panic("No elf kernel!");

	/* get memory map from INT 15:E820 */
	smapbase = (struct bios_smap *)preload_search_info(kmdp,
			MODINFO_METADATA | MODINFOMD_SMAP);
	if (smapbase == NULL)
		panic("No BIOS smap info from loader!");

	smapsize = *((u_int32_t *)smapbase - 1);
	smapend = (struct bios_smap *)((uintptr_t)smapbase + smapsize);

	nvram_smap = (struct bios_smap *) malloc(sizeof(struct bios_smap), M_NVRAM, M_WAITOK | M_ZERO);

	for (smap = smapbase; smap < smapend; smap++) {
		printf("smap type = %d, smap base = %lu, smap length = %lu\n", smap->type, smap->base, smap->length);
		/* nvram memory is assumed to be contiguous */
		if (smap->type == NVRAM_SMAP_TYPE) {
			nvram_smap->base = smap->base;
			nvram_smap->type = smap->type;
			nvram_smap->length = smap->length;
			printf("nvram starting address is = %lu\n", nvram_smap->base);
			printf("nvram total memory is = %lu\n", nvram_smap->length);
			break;
		}
	}

	if (nvram_smap->length == 0) {
		printf("nvram memory not found\n");
		goto out;
	}

	/* add the nvram memory to the physical address map */
	nvram_start_addr = pmap_mapdev((vm_paddr_t)nvram_smap->base, (vm_size_t)nvram_smap->length);
	if (nvram_start_addr == NULL) {
		printf("Could not add the nvram memory to the physical address map\n");
		goto out;
	}

	if (disk_type == DISK_TYPE_DEVFS) {
		if (make_dev_p(MAKEDEV_CHECKNAME | MAKEDEV_WAITOK, &nvram_blk_dev, &nvram_blk_dev_cdevsw,
					NULL, UID_ROOT, GID_OPERATOR, 0640, "%s", NVRAM_BLK_DEV_NAME) != 0) {
			printf("Could not create nvram block device\n");
			goto out;
		}
		printf("nvram devfs block device created\n");
	}

	if (disk_type == DISK_TYPE_GEOM) {
		/* make a geom device */
		g_topology_lock();
		gp = g_new_geomf(&nvram_geom_class, NVRAM_GEOM_CLASS"%d", 0);
		pp = g_new_providerf(gp, NVRAM_BLK_DEV_NAME"%d", 0);
		pp->sectorsize = SECTOR_SIZE;
		pp->mediasize = nvram_smap->length;
		g_error_provider(pp, 0);
		g_topology_unlock();
		/* init the bio queue */
		bioq_init(&nvram_bioq);
		/* init the bio queue mutex */
		mtx_init(&nvram_bioq_mtx, "nvram_bioq_mtx", NULL, MTX_DEF);
		/* create the IO worker thread */
		kproc_kthread_add(nvram_geom_blk_dev_worker, NULL, &nvram_proc, NULL, 0, 0,
				"nvram_kern_worker", NVRAM_BLK_DEV_NAME);
		/* add a new devstat entry */
		nvram_devstat = devstat_new_entry(NVRAM_BLK_DEV_NAME, 0, SECTOR_SIZE,
				DEVSTAT_ALL_SUPPORTED, DEVSTAT_TYPE_DIRECT, DEVSTAT_PRIORITY_MAX);
		printf("nvram geom block device created\n");
	}

	device_initialized = DEVICE_INIT;

out:
	return;
}

static void
nvram_fini()
{
	if (disk_type == DISK_TYPE_DEVFS) {
		if (nvram_blk_dev)
			(void) destroy_dev(nvram_blk_dev);
	}
	if (disk_type == DISK_TYPE_GEOM) {
		g_topology_lock();
		g_topology_assert();
		mtx_lock(&nvram_bioq_mtx);
		nvram_bioq_state = BIO_Q_FINISH_PROCESSING;
		wakeup_one(&nvram_bioq);
		while (nvram_bioq_state != BIO_Q_PROCESSING_DONE)
			msleep(&nvram_bioq_state, &nvram_bioq_mtx, PRIBIO | PDROP,
					"nvram:bioq_state", 0);
		mtx_destroy(&nvram_bioq_mtx);
		/* destroy the geom */
		if (pp)
			g_wither_geom(pp->geom, ENXIO);
		g_topology_unlock();
		/* remove the devstat entry */
		if (nvram_devstat)
			devstat_remove_entry(nvram_devstat);
	}
	if (nvram_start_addr)
		(void) pmap_unmapdev((vm_paddr_t)nvram_smap->base, (vm_size_t)nvram_smap->length);
	if (nvram_smap)
		free(nvram_smap, M_NVRAM);

	device_initialized = DEVICE_NOT_INIT;

	printf("nvram block device destroyed\n");

	return;
}

static int
nvram_dev_open(struct cdev *dev, int oflags, int devtype, struct thread *td)
{
	return (0);
}

static int
nvram_dev_close(struct cdev *dev, int oflags, int devtype, struct thread *td)
{
	return (0);
}

static int
nvram_dev_ioctl(struct cdev *dev, u_long cmd, caddr_t addr, int flag,
		struct thread *td)
{
	int error = 0;

	switch (cmd) {
		case NVRAM_GEOM_CREATE:
			if (!device_initialized) {
				disk_type = DISK_TYPE_GEOM;
				nvram_init();
			} else {
				printf("nvram_dev_ioctl : nvram device already created\n");
			}
			break;
		case NVRAM_DEVFS_CREATE:
			if (!device_initialized) {
				disk_type = DISK_TYPE_DEVFS;
				nvram_init();
			} else {
				printf("nvram_dev_ioctl : nvram device already created\n");
			}
			break;
		case NVRAM_DESTROY:
			nvram_fini();
			disk_type = DISK_TYPE_NONE;
			break;
		default:
			error = ENOTTY;
	}

	return error;
}

static struct cdevsw nvram_cdevsw = {
	.d_version				= D_VERSION,
	.d_open					= nvram_dev_open,
	.d_close				= nvram_dev_close,
	.d_ioctl				= nvram_dev_ioctl,
	.d_name					= NVRAM_DEV_NAME
};

static void
nvram_ctl_init()
{
	nvram_dev = make_dev(&nvram_cdevsw, 0, UID_ROOT, GID_WHEEL, 0600, NVRAM_DEV_NAME);
	KASSERT(nvram_dev != NULL, "nvram device ctl make failed");
}

static void
nvram_ctl_fini()
{
	if (device_initialized)
		nvram_fini();
	if (nvram_dev)
		destroy_dev(nvram_dev);
}

static int
nvram_ctl_modevent(module_t mod __unused, int type, void *unused __unused)
{
	int error = 0;

	switch (type) {
		case MOD_LOAD:
			nvram_ctl_init();
			break;
		case MOD_UNLOAD:
			nvram_ctl_fini();
			break;
		case MOD_SHUTDOWN:
			break;
		default:
			error = EOPNOTSUPP;
			break;
	}

	return error;
}

DEV_MODULE(nvram_ctl, nvram_ctl_modevent, NULL);
MODULE_VERSION(nvram_ctl, 1);
