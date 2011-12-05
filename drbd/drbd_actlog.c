/*
   drbd_actlog.c

   This file is part of DRBD by Philipp Reisner and Lars Ellenberg.

   Copyright (C) 2003-2008, LINBIT Information Technologies GmbH.
   Copyright (C) 2003-2008, Philipp Reisner <philipp.reisner@linbit.com>.
   Copyright (C) 2003-2008, Lars Ellenberg <lars.ellenberg@linbit.com>.

   drbd is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   drbd is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with drbd; see the file COPYING.  If not, write to
   the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.

 */

#include <linux/slab.h>
#include <linux/crc32c.h>
#include <linux/drbd.h>
#include <linux/drbd_limits.h>
#include <linux/dynamic_debug.h>
#include "drbd_int.h"
#include "drbd_wrappers.h"


enum al_transaction_types {
	AL_TR_UPDATE = 0,
	AL_TR_INITIALIZED = 0xffff
};
/* all fields on disc in big endian */
struct __packed al_transaction_on_disk {
	/* don't we all like magic */
	__be32	magic;

	/* to identify the most recent transaction block
	 * in the on disk ring buffer */
	__be32	tr_number;

	/* checksum on the full 4k block, with this field set to 0. */
	__be32	crc32c;

	/* type of transaction, special transaction types like:
	 * purge-all, set-all-idle, set-all-active, ... to-be-defined
	 * see also enum al_transaction_types */
	__be16	transaction_type;

	/* we currently allow only a few thousand extents,
	 * so 16bit will be enough for the slot number. */

	/* how many updates in this transaction */
	__be16	n_updates;

	/* maximum slot number, "al-extents" in drbd.conf speak.
	 * Having this in each transaction should make reconfiguration
	 * of that parameter easier. */
	__be16	context_size;

	/* slot number the context starts with */
	__be16	context_start_slot_nr;

	/* Some reserved bytes.  Expected usage is a 64bit counter of
	 * sectors-written since device creation, and other data generation tag
	 * supporting usage */
	__be32	__reserved[4];

	/* --- 36 byte used --- */

	/* Reserve space for up to AL_UPDATES_PER_TRANSACTION changes
	 * in one transaction, then use the remaining byte in the 4k block for
	 * context information.  "Flexible" number of updates per transaction
	 * does not help, as we have to account for the case when all update
	 * slots are used anyways, so it would only complicate code without
	 * additional benefit.
	 */
	__be16	update_slot_nr[AL_UPDATES_PER_TRANSACTION];

	/* but the extent number is 32bit, which at an extent size of 4 MiB
	 * allows to cover device sizes of up to 2**54 Byte (16 PiB) */
	__be32	update_extent_nr[AL_UPDATES_PER_TRANSACTION];

	/* --- 420 bytes used (36 + 64*6) --- */

	/* 4096 - 420 = 3676 = 919 * 4 */
	__be32	context[AL_CONTEXT_PER_TRANSACTION];
};

struct update_odbm_work {
	struct drbd_work w;
	struct drbd_device *device;
	unsigned int enr;
};

struct update_al_work {
	struct drbd_work w;
	struct drbd_device *device;
	struct completion event;
	int err;
};


static int al_write_transaction(struct drbd_device *device, bool delegate);

void *drbd_md_get_buffer(struct drbd_device *device)
{
	int r;

	wait_event(device->misc_wait,
		   (r = atomic_cmpxchg(&device->md_io_in_use, 0, 1)) == 0 ||
		   device->disk_state <= D_FAILED);

	return r ? NULL : page_address(device->md_io_page);
}

void drbd_md_put_buffer(struct drbd_device *device)
{
	if (atomic_dec_and_test(&device->md_io_in_use))
		wake_up(&device->misc_wait);
}

static bool md_io_allowed(struct drbd_device *device)
{
	enum drbd_disk_state ds = device->disk_state;
	return ds >= D_NEGOTIATING || ds == D_ATTACHING;
}

void wait_until_done_or_disk_failure(struct drbd_device *device, unsigned int *done)
{
	wait_event(device->misc_wait, *done || !md_io_allowed(device));
}

STATIC int _drbd_md_sync_page_io(struct drbd_device *device,
				 struct drbd_backing_dev *bdev,
				 struct page *page, sector_t sector,
				 int rw, int size)
{
	struct bio *bio;
	int err;

	if ((rw & WRITE) && !test_bit(MD_NO_BARRIER, &device->flags))
		rw |= DRBD_REQ_FUA | DRBD_REQ_FLUSH;
	rw |= DRBD_REQ_SYNC;

#ifndef REQ_FLUSH
	/* < 2.6.36, "barrier" semantic may fail with EOPNOTSUPP */
 retry:
#endif
	device->md_io.done = 0;
	device->md_io.error = -ENODEV;

	bio = bio_alloc_drbd(GFP_NOIO);
	bio->bi_bdev = bdev->md_bdev;
	bio->bi_sector = sector;
	err = -EIO;
	if (bio_add_page(bio, page, size, 0) != size)
		goto out;
	bio->bi_private = &device->md_io;
	bio->bi_end_io = drbd_md_io_complete;
	bio->bi_rw = rw;

	if (!get_ldev_if_state(device, D_ATTACHING)) {  /* Corresponding put_ldev in drbd_md_io_complete() */
		drbd_err(device, "ASSERT FAILED: get_ldev_if_state() == 1 in _drbd_md_sync_page_io()\n");
		err = -ENODEV;
		goto out;
	}

	bio_get(bio); /* one bio_put() is in the completion handler */
	atomic_inc(&device->md_io_in_use); /* drbd_md_put_buffer() is in the completion handler */
	if (drbd_insert_fault(device, (rw & WRITE) ? DRBD_FAULT_MD_WR : DRBD_FAULT_MD_RD))
		bio_endio(bio, -EIO);
	else
		submit_bio(rw, bio);
	wait_until_done_or_disk_failure(device, &device->md_io.done);
	if (bio_flagged(bio, BIO_UPTODATE))
		err = device->md_io.error;

#ifndef REQ_FLUSH
	/* check for unsupported barrier op.
	 * would rather check on EOPNOTSUPP, but that is not reliable.
	 * don't try again for ANY return value != 0 */
	if (err && device->md_io.done && (bio->bi_rw & DRBD_REQ_HARDBARRIER)) {
		/* Try again with no barrier */
		drbd_warn(device, "Barriers not supported on meta data device - disabling\n");
		set_bit(MD_NO_BARRIER, &device->flags);
		rw &= ~DRBD_REQ_HARDBARRIER;
		bio_put(bio);
		goto retry;
	}
#endif
 out:
	bio_put(bio);
	return err;
}

int drbd_md_sync_page_io(struct drbd_device *device, struct drbd_backing_dev *bdev,
			 sector_t sector, int rw)
{
	int err;
	struct page *iop = device->md_io_page;

	D_ASSERT(device, atomic_read(&device->md_io_in_use) == 1);

	if (!bdev->md_bdev) {
		if (drbd_ratelimit()) {
			drbd_err(device, "bdev->md_bdev==NULL\n");
			dump_stack();
		}
		return -EIO;
	}

	drbd_dbg(device, "meta_data io: %s [%d]:%s(,%llus,%s)\n",
	     current->comm, current->pid, __func__,
	     (unsigned long long)sector, (rw & WRITE) ? "WRITE" : "READ");

	if (sector < drbd_md_first_sector(bdev) ||
	    sector + 7 > drbd_md_last_sector(bdev))
		drbd_alert(device, "%s [%d]:%s(,%llus,%s) out of range md access!\n",
		     current->comm, current->pid, __func__,
		     (unsigned long long)sector, (rw & WRITE) ? "WRITE" : "READ");

	err = _drbd_md_sync_page_io(device, bdev, iop, sector, rw, MD_BLOCK_SIZE);
	if (err) {
		drbd_err(device, "drbd_md_sync_page_io(,%llus,%s) failed with error %d\n",
		    (unsigned long long)sector, (rw & WRITE) ? "WRITE" : "READ", err);
	}
	return err;
}

static
struct lc_element *_al_get(struct drbd_device *device, unsigned int enr)
{
	struct drbd_peer_device *peer_device;
	struct lc_element *al_ext;
	struct lc_element *tmp;
	int wake;

	spin_lock_irq(&device->al_lock);
	for_each_peer_device(peer_device, device) {
		tmp = lc_find(peer_device->resync_lru, enr/AL_EXT_PER_BM_SECT);
		if (unlikely(tmp != NULL)) {
			struct bm_extent  *bm_ext = lc_entry(tmp, struct bm_extent, lce);
			if (test_bit(BME_NO_WRITES, &bm_ext->flags)) {
				wake = !test_and_set_bit(BME_PRIORITY, &bm_ext->flags);
				spin_unlock_irq(&device->al_lock);
				if (wake)
					wake_up(&device->al_wait);
				return NULL;
			}
		}
	}
	al_ext = lc_get(device->act_log, enr);
	spin_unlock_irq(&device->al_lock);
	return al_ext;
}

/*
 * @delegate:	delegate activity log I/O to the worker thread
 */
void drbd_al_begin_io(struct drbd_device *device, struct drbd_interval *i, bool delegate)
{
	/* for bios crossing activity log extent boundaries,
	 * we may need to activate two extents in one go */
	unsigned first = i->sector >> (AL_EXTENT_SHIFT-9);
	unsigned last = (i->sector + (i->size >> 9) - 1) >> (AL_EXTENT_SHIFT-9);
	unsigned enr;
	bool locked = false;


	D_ASSERT(device, atomic_read(&device->local_cnt) > 0);

	for (enr = first; enr <= last; enr++)
		wait_event(device->al_wait, _al_get(device, enr) != NULL);

	/* Serialize multiple transactions.
	 * This uses test_and_set_bit, memory barrier is implicit.
	 */
	wait_event(device->al_wait,
			device->act_log->pending_changes == 0 ||
			(locked = lc_try_lock_for_transaction(device->act_log)));

	if (locked) {
		/* Double check: it may have been committed by someone else
		 * while we were waiting for the lock. */
		if (device->act_log->pending_changes) {
			al_write_transaction(device, delegate);
			device->al_writ_cnt++;

			spin_lock_irq(&device->al_lock);
			/* FIXME
			if (err)
				we need an "lc_cancel" here;
			*/
			lc_committed(device->act_log);
			spin_unlock_irq(&device->al_lock);
		}
		lc_unlock(device->act_log);
		wake_up(&device->al_wait);
	}
}

void drbd_al_complete_io(struct drbd_device *device, struct drbd_interval *i)
{
	/* for bios crossing activity log extent boundaries,
	 * we may need to activate two extents in one go */
	unsigned first = i->sector >> (AL_EXTENT_SHIFT-9);
	unsigned last = (i->sector + (i->size >> 9) - 1) >> (AL_EXTENT_SHIFT-9);
	unsigned enr;
	struct lc_element *extent;
	unsigned long flags;
	bool wake = false;

	spin_lock_irqsave(&device->al_lock, flags);

	for (enr = first; enr <= last; enr++) {
		extent = lc_find(device->act_log, enr);
		if (!extent) {
			drbd_err(device, "al_complete_io() called on inactive extent %u\n", enr);
			continue;
		}
		if (lc_put(device->act_log, extent) == 0)
			wake = true;
	}
	spin_unlock_irqrestore(&device->al_lock, flags);
	if (wake)
		wake_up(&device->al_wait);
}

#if (PAGE_SHIFT + 3) < (AL_EXTENT_SHIFT - BM_BLOCK_SHIFT)
/* Currently BM_BLOCK_SHIFT, BM_EXT_SHIFT and AL_EXTENT_SHIFT
 * are still coupled, or assume too much about their relation.
 * Code below will not work if this is violated.
 * Will be cleaned up with some followup patch.
 */
# error FIXME
#endif

static unsigned int al_extent_to_bm_page(unsigned int al_enr)
{
	return al_enr >>
		/* bit to page */
		((PAGE_SHIFT + 3) -
		/* al extent number to bit */
		 (AL_EXTENT_SHIFT - BM_BLOCK_SHIFT));
}

static unsigned int rs_extent_to_bm_page(unsigned int rs_enr)
{
	return rs_enr >>
		/* bit to page */
		((PAGE_SHIFT + 3) -
		/* resync extent number to bit */
		 (BM_EXT_SHIFT - BM_BLOCK_SHIFT));
}

static int
_al_write_transaction(struct drbd_device *device)
{
	struct al_transaction_on_disk *buffer;
	struct lc_element *e;
	sector_t sector;
	int i, mx;
	unsigned extent_nr;
	unsigned crc = 0;
	int err = 0;

	if (!get_ldev(device)) {
		drbd_err(device, "disk is %s, cannot start al transaction\n",
			drbd_disk_str(device->disk_state));
		return -EIO;
	}

	/* The bitmap write may have failed, causing a state change. */
	if (device->disk_state < D_INCONSISTENT) {
		drbd_err(device,
			"disk is %s, cannot write al transaction\n",
			drbd_disk_str(device->disk_state));
		put_ldev(device);
		return -EIO;
	}

	buffer = drbd_md_get_buffer(device); /* protects md_io_buffer, al_tr_cycle, ... */
	if (!buffer) {
		drbd_err(device, "disk failed while waiting for md_io buffer\n");
		put_ldev(device);
		return -ENODEV;
	}

	memset(buffer, 0, sizeof(*buffer));
	buffer->magic = cpu_to_be32(DRBD_AL_MAGIC);
	buffer->tr_number = cpu_to_be32(device->al_tr_number);

	i = 0;

	/* Even though no one can start to change this list
	 * once we set the LC_LOCKED -- from drbd_al_begin_io(),
	 * lc_try_lock_for_transaction() --, someone may still
	 * be in the process of changing it. */
	spin_lock_irq(&device->al_lock);
	list_for_each_entry(e, &device->act_log->to_be_changed, list) {
		if (i == AL_UPDATES_PER_TRANSACTION) {
			i++;
			break;
		}
		buffer->update_slot_nr[i] = cpu_to_be16(e->lc_index);
		buffer->update_extent_nr[i] = cpu_to_be32(e->lc_new_number);
		if (e->lc_number != LC_FREE)
			drbd_bm_mark_for_writeout(device,
					al_extent_to_bm_page(e->lc_number));
		i++;
	}
	spin_unlock_irq(&device->al_lock);
	BUG_ON(i > AL_UPDATES_PER_TRANSACTION);

	buffer->n_updates = cpu_to_be16(i);
	for ( ; i < AL_UPDATES_PER_TRANSACTION; i++) {
		buffer->update_slot_nr[i] = cpu_to_be16(-1);
		buffer->update_extent_nr[i] = cpu_to_be32(LC_FREE);
	}

	buffer->context_size = cpu_to_be16(device->act_log->nr_elements);
	buffer->context_start_slot_nr = cpu_to_be16(device->al_tr_cycle);

	mx = min_t(int, AL_CONTEXT_PER_TRANSACTION,
		   device->act_log->nr_elements - device->al_tr_cycle);
	for (i = 0; i < mx; i++) {
		unsigned idx = device->al_tr_cycle + i;
		extent_nr = lc_element_by_index(device->act_log, idx)->lc_number;
		buffer->context[i] = cpu_to_be32(extent_nr);
	}
	for (; i < AL_CONTEXT_PER_TRANSACTION; i++)
		buffer->context[i] = cpu_to_be32(LC_FREE);

	device->al_tr_cycle += AL_CONTEXT_PER_TRANSACTION;
	if (device->al_tr_cycle >= device->act_log->nr_elements)
		device->al_tr_cycle = 0;

	sector =  device->ldev->md.md_offset
		+ device->ldev->md.al_offset
		+ device->al_tr_pos * (MD_BLOCK_SIZE>>9);

	crc = crc32c(0, buffer, 4096);
	buffer->crc32c = cpu_to_be32(crc);

	if (drbd_bm_write_hinted(device))
		err = -EIO;
		/* drbd_chk_io_error done already */
	else if (drbd_md_sync_page_io(device, device->ldev, sector, WRITE)) {
		err = -EIO;
		drbd_chk_io_error(device, 1, true);
	} else {
		/* advance ringbuffer position and transaction counter */
		device->al_tr_pos = (device->al_tr_pos + 1) % (MD_AL_SECTORS*512/MD_BLOCK_SIZE);
		device->al_tr_number++;
	}

	drbd_md_put_buffer(device);
	put_ldev(device);

	return err;
}


static int w_al_write_transaction(struct drbd_work *w, int unused)
{
	struct update_al_work *aw = container_of(w, struct update_al_work, w);
	struct drbd_device *device = aw->device;
	int err;

	err = _al_write_transaction(device);
	aw->err = err;
	complete(&aw->event);

	return err != -EIO ? err : 0;
}

/*
 * @delegate:	delegate activity log I/O to the worker thread
 */
static int al_write_transaction(struct drbd_device *device, bool delegate)
{
	if (delegate) {
		struct update_al_work al_work;

		/* When called through generic_make_request(), we must delegate
		 * activity log I/O to the worker thread: a further request
		 * submitted via generic_make_request() within the same task
		 * would be queued on current->bio_list, and would only start
		 * after this function returns (see generic_make_request()).
		 */

		BUG_ON(current == device->resource->worker.task);

		init_completion(&al_work.event);
		al_work.w.cb = w_al_write_transaction;
		al_work.device = device;
		drbd_queue_work(&device->resource->work, &al_work.w);
		wait_for_completion(&al_work.event);
		return al_work.err;
	} else
		return _al_write_transaction(device);
}

static int _try_lc_del(struct drbd_device *device, struct lc_element *al_ext)
{
	int rv;

	spin_lock_irq(&device->al_lock);
	rv = (al_ext->refcnt == 0);
	if (likely(rv))
		lc_del(device->act_log, al_ext);
	spin_unlock_irq(&device->al_lock);

	return rv;
}

/**
 * drbd_al_shrink() - Removes all active extents form the activity log
 * @device:	DRBD device.
 *
 * Removes all active extents form the activity log, waiting until
 * the reference count of each entry dropped to 0 first, of course.
 *
 * You need to lock device->act_log with lc_try_lock() / lc_unlock()
 */
void drbd_al_shrink(struct drbd_device *device)
{
	struct lc_element *al_ext;
	int i;

	D_ASSERT(device, test_bit(__LC_LOCKED, &device->act_log->flags));

	for (i = 0; i < device->act_log->nr_elements; i++) {
		al_ext = lc_element_by_index(device->act_log, i);
		if (al_ext->lc_number == LC_FREE)
			continue;
		wait_event(device->al_wait, _try_lc_del(device, al_ext));
	}

	wake_up(&device->al_wait);
}

STATIC int w_update_odbm(struct drbd_work *w, int unused)
{
	struct update_odbm_work *udw = container_of(w, struct update_odbm_work, w);
	struct drbd_device *device = udw->device;
	struct sib_info sib = { .sib_reason = SIB_SYNC_PROGRESS, };

	if (!get_ldev(device)) {
		if (drbd_ratelimit())
			drbd_warn(device, "Can not update on disk bitmap, local IO disabled.\n");
		kfree(udw);
		return 0;
	}

	drbd_bm_write_page(device, rs_extent_to_bm_page(udw->enr));
	put_ldev(device);

	kfree(udw);

	if (drbd_bm_total_weight(device) <= first_peer_device(device)->rs_failed) {
		switch (first_peer_device(device)->repl_state) {
		case L_SYNC_SOURCE:  case L_SYNC_TARGET:
		case L_PAUSED_SYNC_S: case L_PAUSED_SYNC_T:
			drbd_resync_finished(first_peer_device(device));
		default:
			/* nothing to do */
			break;
		}
	}
	drbd_bcast_event(device, &sib);

	return 0;
}


/* ATTENTION. The AL's extents are 4MB each, while the extents in the
 * resync LRU-cache are 16MB each.
 * The caller of this function has to hold an get_ldev() reference.
 *
 * TODO will be obsoleted once we have a caching lru of the on disk bitmap
 */
static void drbd_try_clear_on_disk_bm(struct drbd_peer_device *peer_device, sector_t sector,
				      int count, int success)
{
	struct drbd_device *device = peer_device->device;
	struct lc_element *e;
	struct update_odbm_work *udw;

	unsigned int enr;

	D_ASSERT(device, atomic_read(&device->local_cnt));

	/* I simply assume that a sector/size pair never crosses
	 * a 16 MB extent border. (Currently this is true...) */
	enr = BM_SECT_TO_EXT(sector);

	e = lc_get(first_peer_device(device)->resync_lru, enr);
	if (e) {
		struct bm_extent *ext = lc_entry(e, struct bm_extent, lce);
		if (ext->lce.lc_number == enr) {
			if (success)
				ext->rs_left -= count;
			else
				ext->rs_failed += count;
			if (ext->rs_left < ext->rs_failed) {
				struct drbd_peer_device *pd = first_peer_device(device);
				unsigned s = combined_conn_state(pd);
				drbd_warn(device, "BAD! sector=%llus enr=%u rs_left=%d "
				    "rs_failed=%d count=%d cstate=%s\n",
				     (unsigned long long)sector,
				     ext->lce.lc_number, ext->rs_left,
				     ext->rs_failed, count,
				     drbd_conn_str(s));

				/* We don't expect to be able to clear more bits
				 * than have been set when we originally counted
				 * the set bits to cache that value in ext->rs_left.
				 * Whatever the reason (disconnect during resync,
				 * delayed local completion of an application write),
				 * try to fix it up by recounting here. */
				ext->rs_left = drbd_bm_e_weight(device, enr);
			}
		} else {
			/* Normally this element should be in the cache,
			 * since drbd_rs_begin_io() pulled it already in.
			 *
			 * But maybe an application write finished, and we set
			 * something outside the resync lru_cache in sync.
			 */
			int rs_left = drbd_bm_e_weight(device, enr);
			if (ext->flags != 0) {
				drbd_warn(device, "changing resync lce: %d[%u;%02lx]"
				     " -> %d[%u;00]\n",
				     ext->lce.lc_number, ext->rs_left,
				     ext->flags, enr, rs_left);
				ext->flags = 0;
			}
			if (ext->rs_failed) {
				drbd_warn(device, "Kicking resync_lru element enr=%u "
				     "out with rs_failed=%d\n",
				     ext->lce.lc_number, ext->rs_failed);
			}
			ext->rs_left = rs_left;
			ext->rs_failed = success ? 0 : count;
			/* we don't keep a persistent log of the resync lru,
			 * we can commit any change right away. */
			lc_committed(first_peer_device(device)->resync_lru);
		}
		lc_put(first_peer_device(device)->resync_lru, &ext->lce);
		/* no race, we are within the al_lock! */

		if (ext->rs_left == ext->rs_failed) {
			ext->rs_failed = 0;

			udw = kmalloc(sizeof(*udw), GFP_ATOMIC);
			if (udw) {
				udw->enr = ext->lce.lc_number;
				udw->w.cb = w_update_odbm;
				udw->device = device;
				drbd_queue_work(&device->resource->work, &udw->w);
			} else {
				drbd_warn(device, "Could not kmalloc an udw\n");
			}
		}
	} else {
		drbd_err(device, "lc_get() failed! locked=%d/%d flags=%lu\n",
		    first_peer_device(device)->resync_locked,
		    first_peer_device(device)->resync_lru->nr_elements,
		    first_peer_device(device)->resync_lru->flags);
	}
}

void drbd_advance_rs_marks(struct drbd_peer_device *peer_device, unsigned long still_to_go)
{
	unsigned long now = jiffies;
	unsigned long last = peer_device->rs_mark_time[peer_device->rs_last_mark];
	int next = (peer_device->rs_last_mark + 1) % DRBD_SYNC_MARKS;
	if (time_after_eq(now, last + DRBD_SYNC_MARK_STEP)) {
		if (peer_device->rs_mark_left[peer_device->rs_last_mark] != still_to_go &&
		    peer_device->repl_state != L_PAUSED_SYNC_T &&
		    peer_device->repl_state != L_PAUSED_SYNC_S) {
			peer_device->rs_mark_time[next] = now;
			peer_device->rs_mark_left[next] = still_to_go;
			peer_device->rs_last_mark = next;
		}
	}
}

/* clear the bit corresponding to the piece of storage in question:
 * size byte of data starting from sector.  Only clear a bits of the affected
 * one ore more _aligned_ BM_BLOCK_SIZE blocks.
 *
 * called by worker on L_SYNC_TARGET and receiver on SyncSource.
 *
 */
void drbd_set_in_sync(struct drbd_device *device, sector_t sector, int size)
{
	/* Is called from worker and receiver context _only_ */
	unsigned long sbnr, ebnr, lbnr;
	unsigned long count = 0;
	sector_t esector, nr_sectors;
	int wake_up = 0;
	unsigned long flags;

	if (size <= 0 || !IS_ALIGNED(size, 512) || size > DRBD_MAX_BIO_SIZE) {
		drbd_err(device, "drbd_set_in_sync: sector=%llus size=%d nonsense!\n",
				(unsigned long long)sector, size);
		return;
	}
	nr_sectors = drbd_get_capacity(device->this_bdev);
	esector = sector + (size >> 9) - 1;

	if (!expect(device, sector < nr_sectors))
		return;
	if (!expect(device, esector < nr_sectors))
		esector = nr_sectors - 1;

	lbnr = BM_SECT_TO_BIT(nr_sectors-1);

	/* we clear it (in sync).
	 * round up start sector, round down end sector.  we make sure we only
	 * clear full, aligned, BM_BLOCK_SIZE (4K) blocks */
	if (unlikely(esector < BM_SECT_PER_BIT-1))
		return;
	if (unlikely(esector == (nr_sectors-1)))
		ebnr = lbnr;
	else
		ebnr = BM_SECT_TO_BIT(esector - (BM_SECT_PER_BIT-1));
	sbnr = BM_SECT_TO_BIT(sector + BM_SECT_PER_BIT-1);

	if (sbnr > ebnr)
		return;

	/*
	 * ok, (capacity & 7) != 0 sometimes, but who cares...
	 * we count rs_{total,left} in bits, not sectors.
	 */
	count = drbd_bm_clear_bits(device, sbnr, ebnr);
	if (count && get_ldev(device)) {
		struct drbd_peer_device *peer_device;

		drbd_advance_rs_marks(first_peer_device(device), drbd_bm_total_weight(device));
		spin_lock_irqsave(&device->al_lock, flags);
		/* FIXME: Make sure the list of peer devices cannot change here. */
		for_each_peer_device_rcu(peer_device, device)
			drbd_try_clear_on_disk_bm(peer_device, sector, count, true);
		spin_unlock_irqrestore(&device->al_lock, flags);

		/* just wake_up unconditional now, various lc_chaged(),
		 * lc_put() in drbd_try_clear_on_disk_bm(). */
		wake_up = 1;
		put_ldev(device);
	}
	if (wake_up)
		wake_up(&device->al_wait);
}

/*
 * this is intended to set one request worth of data out of sync.
 * affects at least 1 bit,
 * and at most 1+DRBD_MAX_BIO_SIZE/BM_BLOCK_SIZE bits.
 *
 * called by tl_clear and drbd_send_dblock (==drbd_make_request).
 * so this can be _any_ process.
 */
int drbd_set_out_of_sync(struct drbd_device *device, sector_t sector, int size)
{
	unsigned long sbnr, ebnr, flags;
	sector_t esector, nr_sectors;
	unsigned int enr, count = 0;
	struct lc_element *e;

	if (size <= 0 || !IS_ALIGNED(size, 512) || size > DRBD_MAX_BIO_SIZE) {
		drbd_err(device, "sector: %llus, size: %d\n",
			(unsigned long long)sector, size);
		return 0;
	}

	if (!get_ldev(device))
		return 0; /* no disk, no metadata, no bitmap to set bits in */

	nr_sectors = drbd_get_capacity(device->this_bdev);
	esector = sector + (size >> 9) - 1;

	if (!expect(device, sector < nr_sectors))
		goto out;
	if (!expect(device, esector < nr_sectors))
		esector = nr_sectors - 1;

	/* we set it out of sync,
	 * we do not need to round anything here */
	sbnr = BM_SECT_TO_BIT(sector);
	ebnr = BM_SECT_TO_BIT(esector);

	/* ok, (capacity & 7) != 0 sometimes, but who cares...
	 * we count rs_{total,left} in bits, not sectors.  */
	spin_lock_irqsave(&device->al_lock, flags);
	count = drbd_bm_set_bits(device, sbnr, ebnr);

	enr = BM_SECT_TO_EXT(sector);
	e = lc_find(first_peer_device(device)->resync_lru, enr);
	if (e)
		lc_entry(e, struct bm_extent, lce)->rs_left += count;
	spin_unlock_irqrestore(&device->al_lock, flags);

out:
	put_ldev(device);

	return count;
}

static
struct bm_extent *_bme_get(struct drbd_peer_device *peer_device, unsigned int enr)
{
	struct drbd_device *device = peer_device->device;
	struct lc_element *e;
	struct bm_extent *bm_ext;
	int wakeup = 0;
	unsigned long rs_flags;

	spin_lock_irq(&device->al_lock);
	if (peer_device->resync_locked > peer_device->resync_lru->nr_elements/2) {
		spin_unlock_irq(&device->al_lock);
		return NULL;
	}
	e = lc_get(peer_device->resync_lru, enr);
	bm_ext = e ? lc_entry(e, struct bm_extent, lce) : NULL;
	if (bm_ext) {
		if (bm_ext->lce.lc_number != enr) {
			bm_ext->rs_left = drbd_bm_e_weight(device, enr);
			bm_ext->rs_failed = 0;
			lc_committed(peer_device->resync_lru);
			wakeup = 1;
		}
		if (bm_ext->lce.refcnt == 1)
			peer_device->resync_locked++;
		set_bit(BME_NO_WRITES, &bm_ext->flags);
	}
	rs_flags = peer_device->resync_lru->flags;
	spin_unlock_irq(&device->al_lock);
	if (wakeup)
		wake_up(&device->al_wait);

	if (!bm_ext) {
		if (rs_flags & LC_STARVING)
			drbd_warn(peer_device, "Have to wait for element"
			     " (resync LRU too small?)\n");
		BUG_ON(rs_flags & LC_LOCKED);
	}

	return bm_ext;
}

static int _is_in_al(struct drbd_device *device, unsigned int enr)
{
	int rv;

	spin_lock_irq(&device->al_lock);
	rv = lc_is_used(device->act_log, enr);
	spin_unlock_irq(&device->al_lock);

	return rv;
}

/**
 * drbd_rs_begin_io() - Gets an extent in the resync LRU cache and sets it to BME_LOCKED
 *
 * This functions sleeps on al_wait. Returns 0 on success, -EINTR if interrupted.
 */
int drbd_rs_begin_io(struct drbd_peer_device *peer_device, sector_t sector)
{
	struct drbd_device *device = peer_device->device;
	unsigned int enr = BM_SECT_TO_EXT(sector);
	struct bm_extent *bm_ext;
	int i, sig;
	int sa = 200; /* Step aside 200 times, then grab the extent and let app-IO wait.
			 200 times -> 20 seconds. */

retry:
	sig = wait_event_interruptible(device->al_wait,
			(bm_ext = _bme_get(peer_device, enr)));
	if (sig)
		return -EINTR;

	if (test_bit(BME_LOCKED, &bm_ext->flags))
		return 0;

	for (i = 0; i < AL_EXT_PER_BM_SECT; i++) {
		sig = wait_event_interruptible(device->al_wait,
					       !_is_in_al(device, enr * AL_EXT_PER_BM_SECT + i) ||
					       test_bit(BME_PRIORITY, &bm_ext->flags));

		if (sig || (test_bit(BME_PRIORITY, &bm_ext->flags) && sa)) {
			spin_lock_irq(&device->al_lock);
			if (lc_put(peer_device->resync_lru, &bm_ext->lce) == 0) {
				bm_ext->flags = 0; /* clears BME_NO_WRITES and eventually BME_PRIORITY */
				peer_device->resync_locked--;
				wake_up(&device->al_wait);
			}
			spin_unlock_irq(&device->al_lock);
			if (sig)
				return -EINTR;
			if (schedule_timeout_interruptible(HZ/10))
				return -EINTR;
			if (sa && --sa == 0)
				drbd_warn(device,"drbd_rs_begin_io() stepped aside for 20sec."
					 "Resync stalled?\n");
			goto retry;
		}
	}
	set_bit(BME_LOCKED, &bm_ext->flags);
	return 0;
}

/**
 * drbd_try_rs_begin_io() - Gets an extent in the resync LRU cache, does not sleep
 *
 * Gets an extent in the resync LRU cache, sets it to BME_NO_WRITES, then
 * tries to set it to BME_LOCKED. Returns 0 upon success, and -EAGAIN
 * if there is still application IO going on in this area.
 */
int drbd_try_rs_begin_io(struct drbd_peer_device *peer_device, sector_t sector)
{
	struct drbd_device *device = peer_device->device;
	unsigned int enr = BM_SECT_TO_EXT(sector);
	const unsigned int al_enr = enr*AL_EXT_PER_BM_SECT;
	struct lc_element *e;
	struct bm_extent *bm_ext;
	int i;

	spin_lock_irq(&device->al_lock);
	if (peer_device->resync_wenr != LC_FREE && peer_device->resync_wenr != enr) {
		/* in case you have very heavy scattered io, it may
		 * stall the syncer undefined if we give up the ref count
		 * when we try again and requeue.
		 *
		 * if we don't give up the refcount, but the next time
		 * we are scheduled this extent has been "synced" by new
		 * application writes, we'd miss the lc_put on the
		 * extent we keep the refcount on.
		 * so we remembered which extent we had to try again, and
		 * if the next requested one is something else, we do
		 * the lc_put here...
		 * we also have to wake_up
		 */

		e = lc_find(peer_device->resync_lru, peer_device->resync_wenr);
		bm_ext = e ? lc_entry(e, struct bm_extent, lce) : NULL;
		if (bm_ext) {
			D_ASSERT(device, !test_bit(BME_LOCKED, &bm_ext->flags));
			D_ASSERT(device, test_bit(BME_NO_WRITES, &bm_ext->flags));
			clear_bit(BME_NO_WRITES, &bm_ext->flags);
			peer_device->resync_wenr = LC_FREE;
			if (lc_put(peer_device->resync_lru, &bm_ext->lce) == 0)
				peer_device->resync_locked--;
			wake_up(&device->al_wait);
		} else {
			drbd_alert(device, "LOGIC BUG\n");
		}
	}
	/* TRY. */
	e = lc_try_get(peer_device->resync_lru, enr);
	bm_ext = e ? lc_entry(e, struct bm_extent, lce) : NULL;
	if (bm_ext) {
		if (test_bit(BME_LOCKED, &bm_ext->flags))
			goto proceed;
		if (!test_and_set_bit(BME_NO_WRITES, &bm_ext->flags)) {
			peer_device->resync_locked++;
		} else {
			/* we did set the BME_NO_WRITES,
			 * but then could not set BME_LOCKED,
			 * so we tried again.
			 * drop the extra reference. */
			bm_ext->lce.refcnt--;
			D_ASSERT(device, bm_ext->lce.refcnt > 0);
		}
		goto check_al;
	} else {
		/* do we rather want to try later? */
		if (peer_device->resync_locked > peer_device->resync_lru->nr_elements-3)
			goto try_again;
		/* Do or do not. There is no try. -- Yoda */
		e = lc_get(peer_device->resync_lru, enr);
		bm_ext = e ? lc_entry(e, struct bm_extent, lce) : NULL;
		if (!bm_ext) {
			const unsigned long rs_flags = peer_device->resync_lru->flags;
			if (rs_flags & LC_STARVING)
				drbd_warn(device, "Have to wait for element"
				     " (resync LRU too small?)\n");
			BUG_ON(rs_flags & LC_LOCKED);
			goto try_again;
		}
		if (bm_ext->lce.lc_number != enr) {
			bm_ext->rs_left = drbd_bm_e_weight(device, enr);
			bm_ext->rs_failed = 0;
			lc_committed(peer_device->resync_lru);
			wake_up(&device->al_wait);
			D_ASSERT(device, test_bit(BME_LOCKED, &bm_ext->flags) == 0);
		}
		set_bit(BME_NO_WRITES, &bm_ext->flags);
		D_ASSERT(device, bm_ext->lce.refcnt == 1);
		peer_device->resync_locked++;
		goto check_al;
	}
check_al:
	for (i = 0; i < AL_EXT_PER_BM_SECT; i++) {
		if (lc_is_used(device->act_log, al_enr+i))
			goto try_again;
	}
	set_bit(BME_LOCKED, &bm_ext->flags);
proceed:
	peer_device->resync_wenr = LC_FREE;
	spin_unlock_irq(&device->al_lock);
	return 0;

try_again:
	if (bm_ext)
		peer_device->resync_wenr = enr;
	spin_unlock_irq(&device->al_lock);
	return -EAGAIN;
}

void drbd_rs_complete_io(struct drbd_peer_device *peer_device, sector_t sector)
{
	struct drbd_device *device = peer_device->device;
	unsigned int enr = BM_SECT_TO_EXT(sector);
	struct lc_element *e;
	struct bm_extent *bm_ext;
	unsigned long flags;

	spin_lock_irqsave(&device->al_lock, flags);
	e = lc_find(peer_device->resync_lru, enr);
	bm_ext = e ? lc_entry(e, struct bm_extent, lce) : NULL;
	if (!bm_ext) {
		spin_unlock_irqrestore(&device->al_lock, flags);
		if (drbd_ratelimit())
			drbd_err(device, "drbd_rs_complete_io() called, but extent not found\n");
		return;
	}

	if (bm_ext->lce.refcnt == 0) {
		spin_unlock_irqrestore(&device->al_lock, flags);
		drbd_err(device, "drbd_rs_complete_io(,%llu [=%u]) called, "
		    "but refcnt is 0!?\n",
		    (unsigned long long)sector, enr);
		return;
	}

	if (lc_put(peer_device->resync_lru, &bm_ext->lce) == 0) {
		bm_ext->flags = 0; /* clear BME_LOCKED, BME_NO_WRITES and BME_PRIORITY */
		peer_device->resync_locked--;
		wake_up(&device->al_wait);
	}

	spin_unlock_irqrestore(&device->al_lock, flags);
}

/**
 * drbd_rs_cancel_all() - Removes all extents from the resync LRU (even BME_LOCKED)
 */
void drbd_rs_cancel_all(struct drbd_peer_device *peer_device)
{
	struct drbd_device *device = peer_device->device;
	spin_lock_irq(&device->al_lock);

	if (get_ldev_if_state(device, D_FAILED)) { /* Makes sure ->resync is there. */
		lc_reset(peer_device->resync_lru);
		put_ldev(device);
	}
	peer_device->resync_locked = 0;
	peer_device->resync_wenr = LC_FREE;
	spin_unlock_irq(&device->al_lock);
	wake_up(&device->al_wait);
}

/**
 * drbd_rs_del_all() - Gracefully remove all extents from the resync LRU
 *
 * Returns 0 upon success, -EAGAIN if at least one reference count was
 * not zero.
 */
int drbd_rs_del_all(struct drbd_peer_device *peer_device)
{
	struct drbd_device *device = peer_device->device;
	struct lc_element *e;
	struct bm_extent *bm_ext;
	int i;

	spin_lock_irq(&device->al_lock);

	if (get_ldev_if_state(device, D_FAILED)) {
		/* ok, ->resync is there. */
		for (i = 0; i < peer_device->resync_lru->nr_elements; i++) {
			e = lc_element_by_index(peer_device->resync_lru, i);
			bm_ext = lc_entry(e, struct bm_extent, lce);
			if (bm_ext->lce.lc_number == LC_FREE)
				continue;
			if (bm_ext->lce.lc_number == peer_device->resync_wenr) {
				drbd_info(device, "dropping %u in drbd_rs_del_all, apparently"
				     " got 'synced' by application io\n",
				     peer_device->resync_wenr);
				D_ASSERT(device, !test_bit(BME_LOCKED, &bm_ext->flags));
				D_ASSERT(device, test_bit(BME_NO_WRITES, &bm_ext->flags));
				clear_bit(BME_NO_WRITES, &bm_ext->flags);
				peer_device->resync_wenr = LC_FREE;
				lc_put(peer_device->resync_lru, &bm_ext->lce);
			}
			if (bm_ext->lce.refcnt != 0) {
				drbd_info(device, "Retrying drbd_rs_del_all() later. "
				     "refcnt=%d\n", bm_ext->lce.refcnt);
				put_ldev(device);
				spin_unlock_irq(&device->al_lock);
				return -EAGAIN;
			}
			D_ASSERT(device, !test_bit(BME_LOCKED, &bm_ext->flags));
			D_ASSERT(device, !test_bit(BME_NO_WRITES, &bm_ext->flags));
			lc_del(peer_device->resync_lru, &bm_ext->lce);
		}
		D_ASSERT(device, peer_device->resync_lru->used == 0);
		put_ldev(device);
	}
	spin_unlock_irq(&device->al_lock);

	return 0;
}

/*
 * Record information on a failure to resync the specified blocks
 */
void drbd_rs_failed_io(struct drbd_peer_device *peer_device, sector_t sector, int size)
{
	/* Is called from worker and receiver context _only_ */
	struct drbd_device *device = peer_device->device;
	unsigned long sbnr, ebnr, lbnr;
	unsigned long count;
	sector_t esector, nr_sectors;
	int wake_up = 0;

	if (size <= 0 || !IS_ALIGNED(size, 512) || size > DRBD_MAX_BIO_SIZE) {
		drbd_err(device, "drbd_rs_failed_io: sector=%llus size=%d nonsense!\n",
				(unsigned long long)sector, size);
		return;
	}
	nr_sectors = drbd_get_capacity(device->this_bdev);
	esector = sector + (size >> 9) - 1;

	if (!expect(device, sector < nr_sectors))
		return;
	if (!expect(device, esector < nr_sectors))
		esector = nr_sectors - 1;

	lbnr = BM_SECT_TO_BIT(nr_sectors-1);

	/*
	 * round up start sector, round down end sector.  we make sure we only
	 * handle full, aligned, BM_BLOCK_SIZE (4K) blocks */
	if (unlikely(esector < BM_SECT_PER_BIT-1))
		return;
	if (unlikely(esector == (nr_sectors-1)))
		ebnr = lbnr;
	else
		ebnr = BM_SECT_TO_BIT(esector - (BM_SECT_PER_BIT-1));
	sbnr = BM_SECT_TO_BIT(sector + BM_SECT_PER_BIT-1);

	if (sbnr > ebnr)
		return;

	/*
	 * ok, (capacity & 7) != 0 sometimes, but who cares...
	 * we count rs_{total,left} in bits, not sectors.
	 */
	spin_lock_irq(&device->al_lock);
	count = drbd_bm_count_bits(device, sbnr, ebnr);
	if (count) {
		peer_device->rs_failed += count;

		if (get_ldev(device)) {
			drbd_try_clear_on_disk_bm(peer_device, sector, count, false);
			put_ldev(device);
		}

		/* just wake_up unconditional now, various lc_chaged(),
		 * lc_put() in drbd_try_clear_on_disk_bm(). */
		wake_up = 1;
	}
	spin_unlock_irq(&device->al_lock);
	if (wake_up)
		wake_up(&device->al_wait);
}
