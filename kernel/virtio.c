/**
 * Implements virtio device drivers, particularly mmio ones.
 *
 * Reference:
 *
 * http://docs.oasis-open.org/virtio/virtio/v1.0/cs04/virtio-v1.0-cs04.html
 */

#include "kernel.h"
#include "slab.h"
#include "virtio.h"

#define VIRTIO_MAGIC 0x74726976
#define VIRTIO_VERSION 0x2
#define VIRTIO_DEV_BLK 0x2

uint8_t buffer[512];


struct virtio_cap indp_caps[] = {
	{"VIRTIO_F_RING_INDIRECT_DESC", 1<<28, false,
		"Negotiating this feature indicates that the driver can use"
		" descriptors with the VIRTQ_DESC_F_INDIRECT flag set, as"
		" described in 2.4.5.3 Indirect Descriptors."},
	{"VIRTIO_F_RING_EVENT_IDX", 1<<29, false,
		"This feature enables the used_event and the avail_event fields"
		" as described in 2.4.7 and 2.4.8."},
	/*{"VIRTIO_F_VERSION_1", 1<<32, false,
		"This indicates compliance with this specification, giving a"
		" simple way to detect legacy devices or drivers."},*/
};

struct virtio_cap blk_caps[] = {
	{"VIRTIO_BLK_F_SIZE_MAX", 1<<1, false,
		"Maximum size of any single segment is in size_max."},
	{"VIRTIO_BLK_F_SEG_MAX", 1<<2, false,
		"Maximum number of segments in a request is in seg_max."},
	{"VIRTIO_BLK_F_GEOMETRY", 1<<4, false,
		"Disk-style geometry specified in geometry."},
	{"VIRTIO_BLK_F_RO", 1<<5, false,
		"Device is read-only."},
	{"VIRTIO_BLK_F_BLK_SIZE", 1<<6, false,
		"Block size of disk is in blk_size."},
	{"VIRTIO_BLK_F_FLUSH", 1<<9, false,
		"Cache flush command support."},
	{"VIRTIO_BLK_F_TOPOLOGY", 1<<10, false,
		"Device exports information on optimal I/O alignment."},
	{"VIRTIO_BLK_F_CONFIG_WCE", 1<<11, false,
		"Device can toggle its cache between writeback and "
		"writethrough modes."},
};

struct slab *blkreq_slab;

struct virtio_blk {
	virtio_regs *regs;
	struct virtqueue *virtq;
	uint32_t intid;
} blkdev;

static void maybe_init_blkreq_slab(void)
{
	if (!blkreq_slab)
		blkreq_slab = slab_new(sizeof(struct virtio_blk_req), kmem_get_page, kmem_free_page);
}

struct virtqueue *virtq_create(uint32_t len)
{
	uint32_t page_phys;
	uint32_t page_virt;
	struct virtqueue *virtq;

	/* compute offsets */
	uint32_t off_desc = ALIGN(sizeof(struct virtqueue), 16);
	uint32_t off_avail = ALIGN(off_desc + len * sizeof(struct virtqueue_desc), 2);
	uint32_t off_used_event = (
			off_avail + sizeof(struct virtqueue_avail)
			+ len * sizeof(uint16_t));
	uint32_t off_used = ALIGN(off_used_event + sizeof(uint16_t), 4);
	uint32_t off_avail_event = (
			off_used + sizeof(struct virtqueue_used)
			+ len * sizeof(struct virtqueue_used_elem));
	uint32_t memsize = off_avail_event + sizeof(uint16_t);

	printf("virtq_create: len=%u memsize=0x%x\n", len, memsize);
	if (memsize > PAGE_SIZE) {
		printf("virtq_create: error, too big for a page\n");
		return NULL;
	}
	page_phys = alloc_pages(phys_allocator, PAGE_SIZE, 0);
	page_virt = alloc_pages(kern_virt_allocator, PAGE_SIZE, 0);
	kmem_map_pages(page_virt, page_phys, PAGE_SIZE, PRW_UNA | EXECUTE_NEVER);

	virtq = (struct virtqueue *)page_virt;
	virtq->phys = page_phys;
	virtq->len = len;

	virtq->desc = (struct virtqueue_desc *)(page_virt + off_desc);
	virtq->avail = (struct virtqueue_avail *) (page_virt + off_avail);
	virtq->used_event = (uint16_t *) (page_virt + off_used_event);
	virtq->used = (struct virtqueue_used *) (page_virt + off_used);
	virtq->avail_event = (uint16_t *) (page_virt + off_avail_event);

	virtq->avail->idx = 0;
	virtq->used->idx = 0;

	return virtq;
}

void virtq_add_to_device(volatile virtio_regs *regs, struct virtqueue *virtq, uint32_t queue_sel)
{
	WRITE32(regs->QueueSel, queue_sel);
	mb();
	WRITE32(regs->QueueNum, virtq->len);
	WRITE32(regs->QueueDescLow, virtq->phys + ((void*)virtq->desc - (void*)virtq));
	WRITE32(regs->QueueDescHigh, 0);
	WRITE32(regs->QueueAvailLow, virtq->phys + ((void*)virtq->avail - (void*)virtq));
	WRITE32(regs->QueueAvailHigh, 0);
	WRITE32(regs->QueueUsedLow, virtq->phys + ((void*)virtq->used - (void*)virtq));
	WRITE32(regs->QueueUsedHigh, 0);
	mb();
	WRITE32(regs->QueueReady, 1);
}

static int virtio_check_capabilities(uint32_t *device, uint32_t *request, struct virtio_cap *caps, uint32_t n)
{
	uint32_t i;
	for (i = 0; i < n; i++) {
		if (*device & caps[i].bit) {
			if (caps[i].support) {
				*request |= caps[i].bit;
			} else {
				printf("virtio supports unsupported option %s (%s)\n",
						caps[i].name, caps[i].help);
			}
		}
		*device &= ~caps[i].bit;
	}
}

#define HI32(u64) ((uint32_t)((0xFFFFFFFF00000000ULL & (u64)) >> 32))
#define LO32(u64) ((uint32_t)(0x00000000FFFFFFFFULL & (u64)))

static int virtio_blk_init(virtio_regs *regs, uint32_t intid)
{
	volatile struct virtio_blk_config *conf = (struct virtio_blk_config*)regs->Config;
	struct virtqueue *virtq;
	uint32_t request_features = 0;
	uint32_t DeviceFeatures;
	uint32_t i;


	WRITE32(regs->DeviceFeaturesSel, 0);
	WRITE32(regs->DriverFeaturesSel, 0);
	mb();
	DeviceFeatures = regs->DeviceFeatures;
	virtio_check_capabilities(&DeviceFeatures, &request_features, blk_caps, nelem(blk_caps));
	virtio_check_capabilities(&DeviceFeatures, &request_features, indp_caps, nelem(indp_caps));

	if (DeviceFeatures) {
		printf("virtio supports undocumented options 0x%x!\n", DeviceFeatures);
	}

	WRITE32(regs->DriverFeatures, request_features);
	WRITE32(regs->Status, READ32(regs->Status) | VIRTIO_STATUS_FEATURES_OK);
	mb();
	if (!(regs->Status & VIRTIO_STATUS_FEATURES_OK)) {
		puts("error: virtio-blk did not accept our features\n");
		return -1;
	}

	printf("virtio-blk has 0x%x %x sectors\n", HI32(conf->capacity), LO32(conf->capacity));
	printf("virtio-blk queuenummax %u\n", READ32(regs->QueueNumMax));
	printf("virtio-blk Status %x\n", READ32(regs->Status));
	printf("virtio-blk InterruptStatus %x\n", regs->InterruptStatus);

	virtq = virtq_create(128);
	virtq_add_to_device(regs, virtq, 0);

	printf("    avail.idx = %u\n", virtq->avail->idx);
	printf("    used.idx = %u\n", virtq->used->idx);

	gic_enable_interrupt(intid);
	blkdev.regs = regs;
	blkdev.virtq = virtq;
	blkdev.intid = intid;

	WRITE32(regs->Status, READ32(regs->Status) | VIRTIO_STATUS_DRIVER_OK);
	mb();
	printf("virtio-blk Status %x\n", *(volatile uint32_t *)&regs->Status);


	maybe_init_blkreq_slab();
	printf("virtio-blk 0x%x (intid %u): ready!\n", kmem_lookup_phys((void*)regs), intid);
}

static int virtio_blk_read(struct virtio_blk *blk, uint32_t sector, uint8_t *data)
{
	volatile struct virtio_blk_req *hdr = slab_alloc(blkreq_slab);
	uint32_t hdr_phys = kmem_lookup_phys((void*)hdr);
	uint32_t data_phys = kmem_lookup_phys((void*)data);

	hdr->type = VIRTIO_BLK_T_IN;
	hdr->sector = sector;

	blk->virtq->desc[0].addr = hdr_phys;
	blk->virtq->desc[0].len = VIRTIO_BLK_REQ_HEADER_SIZE;
	blk->virtq->desc[0].flags = VIRTQ_DESC_F_NEXT;
	blk->virtq->desc[0].next = 1;

	blk->virtq->desc[1].addr = data_phys + 16;
	blk->virtq->desc[1].len = VIRTIO_BLK_SECTOR_SIZE;
	blk->virtq->desc[1].flags = VIRTQ_DESC_F_WRITE | VIRTQ_DESC_F_NEXT;
	blk->virtq->desc[1].next = 2;

	blk->virtq->desc[2].addr = hdr_phys + VIRTIO_BLK_REQ_HEADER_SIZE;
	blk->virtq->desc[2].len = VIRTIO_BLK_REQ_FOOTER_SIZE;
	blk->virtq->desc[2].flags = VIRTQ_DESC_F_WRITE;
	blk->virtq->desc[2].next = 0;

	blk->virtq->avail->ring[blk->virtq->avail->idx] = 0;
	mb();
	blk->virtq->avail->idx += 1;
	mb();
	WRITE32(blk->regs->QueueNotify, 0);
}

static int virtio_dev_init(uint32_t virt, uint32_t intid)
{
	virtio_regs *regs = (virtio_regs *) virt;

	if (READ32(regs->MagicValue) != VIRTIO_MAGIC) {
		printf("error: virtio at 0x%x had wrong magic value 0x%x, expected 0x%x\n",
				virt, regs->MagicValue, VIRTIO_MAGIC);
		return -1;
	}
	if (READ32(regs->Version) != VIRTIO_VERSION) {
		printf("error: virtio at 0x%x had wrong version 0x%x, expected 0x%x\n",
				virt, regs->Version, VIRTIO_VERSION);
		return -1;
	}
	if (READ32(regs->DeviceID) == 0) {
		/*On QEMU, this is pretty common, don't print a message */
		/*printf("warn: virtio at 0x%x has DeviceID=0, skipping\n", virt);*/
		return -1;
	}

	/* First step of initialization: reset */
	WRITE32(regs->Status, 0);
	mb();
	/* Hello there, I see you */
	WRITE32(regs->Status, READ32(regs->Status) | VIRTIO_STATUS_ACKNOWLEDGE);
	mb();

	/* Hello, I am a driver for you */
	WRITE32(regs->Status, READ32(regs->Status) | VIRTIO_STATUS_DRIVER);
	mb();

	switch (READ32(regs->DeviceID)) {
	case VIRTIO_DEV_BLK:
		return virtio_blk_init(regs, intid);
	default:
		printf("unsupported virtio device ID 0x%x\n", READ32(regs->DeviceID));
	}
}

int virtio_blk_cmd_status(int argc, char **argv)
{
	printf("virtio_blk_dev at 0x%x\n", kmem_lookup_phys((void*)blkdev.regs));
	printf("    Status=0x%x\n", READ32(blkdev.regs->Status));
	printf("    DeviceID=0x%x\n", READ32(blkdev.regs->DeviceID));
	printf("    VendorID=0x%x\n", READ32(blkdev.regs->VendorID));
	printf("    InterruptStatus=0x%x\n", READ32(blkdev.regs->InterruptStatus));
	printf("    MagicValue=0x%x\n", READ32(blkdev.regs->MagicValue));
	printf("  Queue 0:\n");
	printf("    avail.idx = %u\n", blkdev.virtq->avail->idx);
	printf("    used.idx = %u\n", blkdev.virtq->used->idx);
	WRITE32(blkdev.regs->QueueSel, 0);
	mb();
	printf("    ready = 0x%x\n", READ32(blkdev.regs->QueueReady));
}

int virtio_blk_cmd_read(int argc, char **argv)
{
	uint32_t sector;

	if (argc != 2) {
		puts("usage: read SECTOR\n");
		return 1;
	}

	sector = atoi(argv[1]);
	virtio_blk_read(&blkdev, 0, buffer);
	return 0;
}

void virtio_init(void)
{
	/* TODO: we know these addresses due to manually reading device tree,
	 * but we should automate that */
	uint32_t page_virt = alloc_pages(kern_virt_allocator, 0x4000, 0);
	kmem_map_pages(page_virt, 0x0a000000U, 0x4000, PRW_UNA | EXECUTE_NEVER);

	for (int i = 0; i < 32; i++)
		virtio_dev_init(page_virt + 0x200 * i, 32 + 0x10 + i);

	virtio_blk_read(&blkdev, 0, buffer);
}
