/* via_dmablit.c -- PCI DMA BitBlt support for the VIA Unichrome/Pro
 *
 * Copyright (C) 2005 Thomas Hellstrom, All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Thomas Hellstrom.
 *    Partially based on code obtained from Digeo Inc.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

/*
 * Unmaps the DMA mappings.
 * FIXME: Is this a NoOp on x86? Also
 * FIXME: What happens if this one is called and a pending blit has previously done
 * the same DMA mappings?
 */

#include "dev/drm/drmP.h"
#include "dev/drm/via_drm.h"
#include "dev/drm/via_drv.h"
#include "dev/drm/via_dmablit.h"

#define VIA_PGDN(x)	(((unsigned long)(x)) & ~PAGE_MASK)
#define VIA_PGOFF(x)	(((unsigned long)(x)) & PAGE_MASK)
#define VIA_PFN(x)	((unsigned long)(x) >> PAGE_SHIFT)

typedef struct _drm_via_descriptor {
	uint32_t mem_addr;
	uint32_t dev_addr;
	uint32_t size;
	uint32_t next;
} drm_via_descriptor_t;

static void via_dmablit_timer(void *arg);

/*
 * Unmap a DMA mapping.
 */
static void
via_unmap_blit_from_device(drm_via_sg_info_t *vsg)
{
	int num_desc = vsg->num_desc;
	unsigned cur_descriptor_page = num_desc / vsg->descriptors_per_page;
	unsigned descriptor_this_page = num_desc % vsg->descriptors_per_page;
	drm_via_descriptor_t *desc_ptr = vsg->desc_pages[cur_descriptor_page] +
		descriptor_this_page;
	dma_addr_t next = vsg->chain_start;

	while(num_desc--) {
		if (descriptor_this_page-- == 0) {
			cur_descriptor_page--;
			descriptor_this_page = vsg->descriptors_per_page - 1;
			desc_ptr = vsg->desc_pages[cur_descriptor_page] +
				descriptor_this_page;
		}
		next = (dma_addr_t) desc_ptr->next;
		desc_ptr--;
	}
}


/*
 * If mode = 0, count how many descriptors are needed.
 * If mode = 1, Map the DMA pages for the device, put together and map also the descriptors.
 * Descriptors are run in reverse order by the hardware because we are not allowed to update the
 * 'next' field without syncing calls when the descriptor is already mapped.
 */
static void
via_map_blit_for_device(const drm_via_dmablit_t *xfer,
		   drm_via_sg_info_t *vsg, int mode)
{
	unsigned cur_descriptor_page = 0;
	unsigned num_descriptors_this_page = 0;
	unsigned char *mem_addr = xfer->mem_addr;
	unsigned char *cur_mem;
	unsigned char *first_addr = (unsigned char *)VIA_PGDN(mem_addr);
	uint32_t fb_addr = xfer->fb_addr;
	uint32_t cur_fb;
	unsigned long line_len;
	unsigned remaining_len;
	int num_desc = 0;
	int cur_line;
	dma_addr_t next = 0 | VIA_DMA_DPR_EC;
	drm_via_descriptor_t *desc_ptr = NULL;

	if (mode == 1)
		desc_ptr = vsg->desc_pages[cur_descriptor_page];

	for (cur_line = 0; cur_line < xfer->num_lines; ++cur_line) {

		line_len = xfer->line_length;
		cur_fb = fb_addr;
		cur_mem = mem_addr;

		while (line_len > 0) {

			remaining_len = min(PAGE_SIZE - VIA_PGOFF(cur_mem),
			    line_len);
			line_len -= remaining_len;

			if (mode == 1) {
				desc_ptr->mem_addr =
				    VM_PAGE_TO_PHYS(
				    vsg->pages[VIA_PFN(cur_mem) -
				    VIA_PFN(first_addr)]) + VIA_PGOFF(cur_mem);
				desc_ptr->dev_addr = cur_fb;

				desc_ptr->size = remaining_len;
				desc_ptr->next = (uint32_t) next;

				next = vtophys(desc_ptr);

				desc_ptr++;
				if (++num_descriptors_this_page >= vsg->descriptors_per_page) {
					num_descriptors_this_page = 0;
					desc_ptr = vsg->desc_pages[++cur_descriptor_page];
				}
			}

			num_desc++;
			cur_mem += remaining_len;
			cur_fb += remaining_len;
		}

		mem_addr += xfer->mem_stride;
		fb_addr += xfer->fb_stride;
	}

	if (mode == 1) {
		vsg->chain_start = next;
		vsg->state = dr_via_device_mapped;
	}
	vsg->num_desc = num_desc;
}


/*
 * Function that frees up all resources for a blit. It is usable even if the
 * blit info has only been partially built as long as the status enum is consistent
 * with the actual status of the used resources.
 */
static void
via_free_sg_info(drm_via_sg_info_t *vsg)
{
	vm_page_t page;
	int i;

	switch(vsg->state) {
	case dr_via_device_mapped:
		via_unmap_blit_from_device(vsg);
	case dr_via_desc_pages_alloc:
		for (i=0; i<vsg->num_desc_pages; ++i) {
			if (vsg->desc_pages[i] != NULL)
			    free(vsg->desc_pages[i], DRM_MEM_PAGES);
		}
		free(vsg->desc_pages, DRM_MEM_DRIVER);
	case dr_via_pages_locked:
		for (i=0; i < vsg->num_pages; ++i) {
			page = vsg->pages[i];
			vm_page_lock(page);
			vm_page_unwire(page, PQ_INACTIVE);
			vm_page_unlock(page);
		}
	case dr_via_pages_alloc:
		free(vsg->pages, DRM_MEM_DRIVER);
	default:
		vsg->state = dr_via_sg_init;
	}
	free(vsg->bounce_buffer, DRM_MEM_DRIVER);
	vsg->bounce_buffer = NULL;
	vsg->free_on_sequence = 0;
}


/*
 * Fire a blit engine.
 */
static void
via_fire_dmablit(struct drm_device *dev, drm_via_sg_info_t *vsg, int engine)
{
	drm_via_private_t *dev_priv = (drm_via_private_t *)dev->dev_private;

	VIA_WRITE(VIA_PCI_DMA_MAR0 + engine*0x10, 0);
	VIA_WRITE(VIA_PCI_DMA_DAR0 + engine*0x10, 0);
	VIA_WRITE(VIA_PCI_DMA_CSR0 + engine*0x04, VIA_DMA_CSR_DD | VIA_DMA_CSR_TD |
		  VIA_DMA_CSR_DE);
	VIA_WRITE(VIA_PCI_DMA_MR0  + engine*0x04, VIA_DMA_MR_CM | VIA_DMA_MR_TDIE);
	VIA_WRITE(VIA_PCI_DMA_BCR0 + engine*0x10, 0);
	VIA_WRITE(VIA_PCI_DMA_DPR0 + engine*0x10, vsg->chain_start);
	DRM_WRITEMEMORYBARRIER();
	VIA_WRITE(VIA_PCI_DMA_CSR0 + engine*0x04, VIA_DMA_CSR_DE | VIA_DMA_CSR_TS);
	(void)VIA_READ(VIA_PCI_DMA_CSR0 + engine*0x04);
}


/*
 * Obtain a page pointer array and lock all pages into system memory. A segmentation violation will
 * occur here if the calling user does not have access to the submitted address.
 */
static int
via_lock_all_dma_pages(drm_via_sg_info_t *vsg,  drm_via_dmablit_t *xfer)
{
	unsigned long first_pfn = VIA_PFN(xfer->mem_addr);
#if __FreeBSD_version < 1300035
	vm_page_t m;
	int i;
#endif

	vsg->num_pages = VIA_PFN(xfer->mem_addr +
	    (xfer->num_lines * xfer->mem_stride -1)) - first_pfn + 1;

	if (NULL == (vsg->pages = malloc(sizeof(vm_page_t) * vsg->num_pages,
	    DRM_MEM_DRIVER, M_NOWAIT)))
		return -ENOMEM;

	vsg->state = dr_via_pages_alloc;

	if (vm_fault_quick_hold_pages(&curproc->p_vmspace->vm_map,
	    (vm_offset_t)xfer->mem_addr, vsg->num_pages * PAGE_SIZE,
	    VM_PROT_READ | VM_PROT_WRITE, vsg->pages, vsg->num_pages) < 0)
		return -EACCES;

#if __FreeBSD_version < 1300035
	for (i = 0; i < vsg->num_pages; i++) {
		m = vsg->pages[i];
		vm_page_lock(m);
		vm_page_wire(m);
		vm_page_unhold(m);
		vm_page_unlock(m);
	}
#endif
	vsg->state = dr_via_pages_locked;

	DRM_DEBUG("DMA pages locked\n");

	return 0;
}


/*
 * Allocate DMA capable memory for the blit descriptor chain, and an array that keeps track of the
 * pages we allocate. We don't want to use kmalloc for the descriptor chain because it may be
 * quite large for some blits, and pages don't need to be contingous.
 */
static int
via_alloc_desc_pages(drm_via_sg_info_t *vsg)
{
	int i;

	vsg->descriptors_per_page = PAGE_SIZE / sizeof(drm_via_descriptor_t);
	vsg->num_desc_pages = (vsg->num_desc + vsg->descriptors_per_page - 1) /
	    vsg->descriptors_per_page;

	if (NULL ==  (vsg->desc_pages = malloc(vsg->num_desc_pages *
	    sizeof(void *), DRM_MEM_DRIVER, M_NOWAIT | M_ZERO)))
		return -ENOMEM;

	vsg->state = dr_via_desc_pages_alloc;
	for (i = 0; i < vsg->num_desc_pages; ++i) {
		if (NULL == (vsg->desc_pages[i] =
		    (drm_via_descriptor_t *)malloc(PAGE_SIZE, DRM_MEM_PAGES,
		    M_NOWAIT | M_ZERO)))
			return -ENOMEM;
	}
	DRM_DEBUG("Allocated %d pages for %d descriptors.\n",
	    vsg->num_desc_pages, vsg->num_desc);

	return 0;
}


static void
via_abort_dmablit(struct drm_device *dev, int engine)
{
	drm_via_private_t *dev_priv = (drm_via_private_t *)dev->dev_private;

	VIA_WRITE(VIA_PCI_DMA_CSR0 + engine*0x04, VIA_DMA_CSR_TA);
}


static void
via_dmablit_engine_off(struct drm_device *dev, int engine)
{
	drm_via_private_t *dev_priv = (drm_via_private_t *)dev->dev_private;

	VIA_WRITE(VIA_PCI_DMA_CSR0 + engine*0x04, VIA_DMA_CSR_TD | VIA_DMA_CSR_DD);
}


/*
 * The dmablit part of the IRQ handler. Trying to do only reasonably fast things here.
 * The rest, like unmapping and freeing memory for done blits is done in a separate workqueue
 * task. Basically the task of the interrupt handler is to submit a new blit to the engine, while
 * the workqueue task takes care of processing associated with the old blit.
 */
void
via_dmablit_handler(struct drm_device *dev, int engine, int from_irq)
{
	drm_via_private_t *dev_priv = (drm_via_private_t *)dev->dev_private;
	drm_via_blitq_t *blitq = dev_priv->blit_queues + engine;
	int cur;
	int done_transfer;
	uint32_t status = 0;

	DRM_DEBUG("DMA blit handler called. engine = %d, from_irq = %d, blitq = 0x%lx\n",
		  engine, from_irq, (unsigned long) blitq);

	mtx_lock(&blitq->blit_lock);

	done_transfer = blitq->is_active &&
	  (( status = VIA_READ(VIA_PCI_DMA_CSR0 + engine*0x04)) & VIA_DMA_CSR_TD);
	done_transfer = done_transfer || ( blitq->aborting && !(status & VIA_DMA_CSR_DE));

	cur = blitq->cur;
	if (done_transfer) {

		blitq->blits[cur]->aborted = blitq->aborting;
		blitq->done_blit_handle++;
		DRM_WAKEUP(&blitq->blit_queue[cur]);

		cur++;
		if (cur >= VIA_NUM_BLIT_SLOTS)
			cur = 0;
		blitq->cur = cur;

		/*
		 * Clear transfer done flag.
		 */

		VIA_WRITE(VIA_PCI_DMA_CSR0 + engine*0x04,  VIA_DMA_CSR_TD);

		blitq->is_active = 0;
		blitq->aborting = 0;

		taskqueue_enqueue(taskqueue_swi, &blitq->wq);

	} else if (blitq->is_active && (ticks >= blitq->end)) {

		/*
		 * Abort transfer after one second.
		 */

		via_abort_dmablit(dev, engine);
		blitq->aborting = 1;
		blitq->end = ticks + DRM_HZ;
	}

	if (!blitq->is_active) {
		if (blitq->num_outstanding) {
			via_fire_dmablit(dev, blitq->blits[cur], engine);
			blitq->is_active = 1;
			blitq->cur = cur;
			blitq->num_outstanding--;
			blitq->end = ticks + DRM_HZ;

			if (!callout_pending(&blitq->poll_timer))
				callout_reset(&blitq->poll_timer,
				    1, (timeout_t *)via_dmablit_timer,
				    (void *)blitq);
		} else {
			if (callout_pending(&blitq->poll_timer)) {
				callout_stop(&blitq->poll_timer);
			}
			via_dmablit_engine_off(dev, engine);
		}
	}

	mtx_unlock(&blitq->blit_lock);
}


/*
 * Check whether this blit is still active, performing necessary locking.
 */
static int
via_dmablit_active(drm_via_blitq_t *blitq, int engine, uint32_t handle, wait_queue_head_t **queue)
{
	uint32_t slot;
	int active;

	mtx_lock(&blitq->blit_lock);

	/*
	 * Allow for handle wraparounds.
	 */
	active = ((blitq->done_blit_handle - handle) > (1 << 23)) &&
		((blitq->cur_blit_handle - handle) <= (1 << 23));

	if (queue && active) {
		slot = handle - blitq->done_blit_handle + blitq->cur -1;
		if (slot >= VIA_NUM_BLIT_SLOTS) {
			slot -= VIA_NUM_BLIT_SLOTS;
		}
		*queue = blitq->blit_queue + slot;
	}

	mtx_unlock(&blitq->blit_lock);

	return active;
}


/*
 * Sync. Wait for at least three seconds for the blit to be performed.
 */
static int
via_dmablit_sync(struct drm_device *dev, uint32_t handle, int engine)
{

	drm_via_private_t *dev_priv = (drm_via_private_t *)dev->dev_private;
	drm_via_blitq_t *blitq = dev_priv->blit_queues + engine;
	wait_queue_head_t *queue;
	int ret = 0;

	if (via_dmablit_active(blitq, engine, handle, &queue)) {
		DRM_WAIT_ON(ret, *queue, 3 * DRM_HZ,
			    !via_dmablit_active(blitq, engine, handle, NULL));
	}
	DRM_DEBUG("DMA blit sync handle 0x%x engine %d returned %d\n",
		  handle, engine, ret);

	return ret;
}


/*
 * A timer that regularly polls the blit engine in cases where we don't have interrupts:
 * a) Broken hardware (typically those that don't have any video capture facility).
 * b) Blit abort. The hardware doesn't send an interrupt when a blit is aborted.
 * The timer and hardware IRQ's can and do work in parallel. If the hardware has
 * irqs, it will shorten the latency somewhat.
 */
static void
via_dmablit_timer(void *arg)
{
	drm_via_blitq_t *blitq = (drm_via_blitq_t *)arg;
	struct drm_device *dev = blitq->dev;
	int engine = (int)
		(blitq - ((drm_via_private_t *)dev->dev_private)->blit_queues);

	DRM_DEBUG("Polling timer called for engine %d, jiffies %lu\n", engine,
		  (unsigned long) jiffies);

	via_dmablit_handler(dev, engine, 0);

	if (!callout_pending(&blitq->poll_timer)) {
		callout_schedule(&blitq->poll_timer, 1);

	       /*
		* Rerun handler to delete timer if engines are off, and
		* to shorten abort latency. This is a little nasty.
		*/

	       via_dmablit_handler(dev, engine, 0);

	}
}


/*
 * Workqueue task that frees data and mappings associated with a blit.
 * Also wakes up waiting processes. Each of these tasks handles one
 * blit engine only and may not be called on each interrupt.
 */
static void
via_dmablit_workqueue(void *arg, int pending)
{
	drm_via_blitq_t *blitq = (drm_via_blitq_t *)arg;
	struct drm_device *dev = blitq->dev;
	drm_via_sg_info_t *cur_sg;
	int cur_released;


	DRM_DEBUG("task called for blit engine %ld\n",(unsigned long)
		  (blitq - ((drm_via_private_t *)dev->dev_private)->blit_queues));

	mtx_lock(&blitq->blit_lock);

	while(blitq->serviced != blitq->cur) {

		cur_released = blitq->serviced++;

		DRM_DEBUG("Releasing blit slot %d\n", cur_released);

		if (blitq->serviced >= VIA_NUM_BLIT_SLOTS)
			blitq->serviced = 0;

		cur_sg = blitq->blits[cur_released];
		blitq->num_free++;

		mtx_unlock(&blitq->blit_lock);

		DRM_WAKEUP(&blitq->busy_queue);

		via_free_sg_info(cur_sg);
		free(cur_sg, DRM_MEM_DRIVER);

		mtx_lock(&blitq->blit_lock);
	}

	mtx_unlock(&blitq->blit_lock);
}


/*
 * Init all blit engines. Currently we use two, but some hardware have 4.
 */
void
via_init_dmablit(struct drm_device *dev)
{
	int i,j;
	drm_via_private_t *dev_priv = (drm_via_private_t *)dev->dev_private;
	drm_via_blitq_t *blitq;

	for (i=0; i< VIA_NUM_BLIT_ENGINES; ++i) {
		blitq = dev_priv->blit_queues + i;
		blitq->dev = dev;
		blitq->cur_blit_handle = 0;
		blitq->done_blit_handle = 0;
		blitq->head = 0;
		blitq->cur = 0;
		blitq->serviced = 0;
		blitq->num_free = VIA_NUM_BLIT_SLOTS - 1;
		blitq->num_outstanding = 0;
		blitq->is_active = 0;
		blitq->aborting = 0;
		mtx_init(&blitq->blit_lock, "via_blit_lk", NULL, MTX_DEF);
		for (j=0; j<VIA_NUM_BLIT_SLOTS; ++j) {
			DRM_INIT_WAITQUEUE(blitq->blit_queue + j);
		}
		DRM_INIT_WAITQUEUE(&blitq->busy_queue);
		TASK_INIT(&blitq->wq, 0, via_dmablit_workqueue, blitq);
		callout_init(&blitq->poll_timer, 0);
	}
}


/*
 * Build all info and do all mappings required for a blit.
 */
static int
via_build_sg_info(struct drm_device *dev, drm_via_sg_info_t *vsg,
    drm_via_dmablit_t *xfer)
{
	int ret = 0;

	vsg->bounce_buffer = NULL;

	vsg->state = dr_via_sg_init;

	if (xfer->num_lines <= 0 || xfer->line_length <= 0) {
		DRM_ERROR("Zero size bitblt.\n");
		return -EINVAL;
	}

	/*
	 * Below check is a driver limitation, not a hardware one. We
	 * don't want to lock unused pages, and don't want to incoporate the
	 * extra logic of avoiding them. Make sure there are no.
	 * (Not a big limitation anyway.)
	 */
	if ((xfer->mem_stride - xfer->line_length) > 2 * PAGE_SIZE) {
		DRM_ERROR("Too large system memory stride. Stride: %d, "
			  "Length: %d\n", xfer->mem_stride, xfer->line_length);
		return -EINVAL;
	}

	if ((xfer->mem_stride == xfer->line_length) &&
	    (xfer->fb_stride == xfer->line_length)) {
		xfer->mem_stride *= xfer->num_lines;
		xfer->line_length = xfer->mem_stride;
		xfer->fb_stride = xfer->mem_stride;
		xfer->num_lines = 1;
	}

	/*
	 * Don't lock an arbitrary large number of pages, since that causes a
	 * DOS security hole.
	 */
	if (xfer->num_lines > 2048 ||
	    (xfer->num_lines*xfer->mem_stride > (2048*2048*4))) {
		DRM_ERROR("Too large PCI DMA bitblt.\n");
		return -EINVAL;
	}

	/*
	 * we allow a negative fb stride to allow flipping of images in
	 * transfer.
	 */
	if (xfer->mem_stride < xfer->line_length ||
		abs(xfer->fb_stride) < xfer->line_length) {
		DRM_ERROR("Invalid frame-buffer / memory stride.\n");
		return -EINVAL;
	}

	/*
	 * A hardware bug seems to be worked around if system memory addresses
	 * start on 16 byte boundaries. This seems a bit restrictive however.
	 * VIA is contacted about this. Meanwhile, impose the following
	 * restrictions:
	 */
#ifdef VIA_BUGFREE
	if ((((unsigned long)xfer->mem_addr & 3) !=
	    ((unsigned long)xfer->fb_addr & 3)) ||
	    ((xfer->num_lines > 1) && ((xfer->mem_stride & 3) !=
	    (xfer->fb_stride & 3)))) {
		DRM_ERROR("Invalid DRM bitblt alignment.\n");
		return -EINVAL;
	}
#else
	if ((((unsigned long)xfer->mem_addr & 15) ||
	    ((unsigned long)xfer->fb_addr & 3)) ||
	    ((xfer->num_lines > 1) &&
	    ((xfer->mem_stride & 15) || (xfer->fb_stride & 3)))) {
		DRM_ERROR("Invalid DRM bitblt alignment.\n");
		return -EINVAL;
	}
#endif

	if (0 != (ret = via_lock_all_dma_pages(vsg, xfer))) {
		DRM_ERROR("Could not lock DMA pages.\n");
		via_free_sg_info(vsg);
		return ret;
	}

	via_map_blit_for_device(xfer, vsg, 0);
	if (0 != (ret = via_alloc_desc_pages(vsg))) {
		DRM_ERROR("Could not allocate DMA descriptor pages.\n");
		via_free_sg_info(vsg);
		return ret;
	}
	via_map_blit_for_device(xfer, vsg, 1);

	return 0;
}


/*
 * Reserve one free slot in the blit queue. Will wait for one second for one
 * to become available. Otherwise -EBUSY is returned.
 */
static int
via_dmablit_grab_slot(drm_via_blitq_t *blitq, int engine)
{
	struct drm_device *dev = blitq->dev;
	int ret=0;

	DRM_DEBUG("Num free is %d\n", blitq->num_free);
	mtx_lock(&blitq->blit_lock);
	while(blitq->num_free == 0) {
		mtx_unlock(&blitq->blit_lock);

		DRM_WAIT_ON(ret, blitq->busy_queue, DRM_HZ,
		    blitq->num_free > 0);
		if (ret) {
			return (-EINTR == ret) ? -EAGAIN : ret;
		}

		mtx_lock(&blitq->blit_lock);
	}

	blitq->num_free--;
	mtx_unlock(&blitq->blit_lock);

	return 0;
}


/*
 * Hand back a free slot if we changed our mind.
 */
static void
via_dmablit_release_slot(drm_via_blitq_t *blitq)
{

	mtx_lock(&blitq->blit_lock);
	blitq->num_free++;
	mtx_unlock(&blitq->blit_lock);
	DRM_WAKEUP( &blitq->busy_queue );
}


/*
 * Grab a free slot. Build blit info and queue a blit.
 */
static int
via_dmablit(struct drm_device *dev, drm_via_dmablit_t *xfer)
{
	drm_via_private_t *dev_priv = (drm_via_private_t *)dev->dev_private;
	drm_via_sg_info_t *vsg;
	drm_via_blitq_t *blitq;
	int ret;
	int engine;

	if (dev_priv == NULL) {
		DRM_ERROR("Called without initialization.\n");
		return -EINVAL;
	}

	engine = (xfer->to_fb) ? 0 : 1;
	blitq = dev_priv->blit_queues + engine;
	if (0 != (ret = via_dmablit_grab_slot(blitq, engine))) {
		return ret;
	}
	if (NULL == (vsg = malloc(sizeof(*vsg), DRM_MEM_DRIVER,
	    M_NOWAIT | M_ZERO))) {
		via_dmablit_release_slot(blitq);
		return -ENOMEM;
	}
	if (0 != (ret = via_build_sg_info(dev, vsg, xfer))) {
		via_dmablit_release_slot(blitq);
		free(vsg, DRM_MEM_DRIVER);
		return ret;
	}
	mtx_lock(&blitq->blit_lock);

	blitq->blits[blitq->head++] = vsg;
	if (blitq->head >= VIA_NUM_BLIT_SLOTS)
		blitq->head = 0;
	blitq->num_outstanding++;
	xfer->sync.sync_handle = ++blitq->cur_blit_handle;

	mtx_unlock(&blitq->blit_lock);
	xfer->sync.engine = engine;

	via_dmablit_handler(dev, engine, 0);

	return 0;
}


/*
 * Sync on a previously submitted blit. Note that the X server use signals
 * extensively, and that there is a very big probability that this IOCTL will
 * be interrupted by a signal. In that case it returns with -EAGAIN for the
 * signal to be delivered. The caller should then reissue the IOCTL. This is
 * similar to what is being done for drmGetLock().
 */
int
via_dma_blit_sync( struct drm_device *dev, void *data,
    struct drm_file *file_priv )
{
	drm_via_blitsync_t *sync = data;
	int err;

	if (sync->engine >= VIA_NUM_BLIT_ENGINES)
		return -EINVAL;

	err = via_dmablit_sync(dev, sync->sync_handle, sync->engine);

	if (-EINTR == err)
		err = -EAGAIN;

	return err;
}


/*
 * Queue a blit and hand back a handle to be used for sync. This IOCTL may be
 * interrupted by a signal while waiting for a free slot in the blit queue.
 * In that case it returns with -EAGAIN and should be reissued. See the above
 * IOCTL code.
 */
int
via_dma_blit( struct drm_device *dev, void *data, struct drm_file *file_priv )
{
	drm_via_dmablit_t *xfer = data;
	int err;

	err = via_dmablit(dev, xfer);

	return err;
}
