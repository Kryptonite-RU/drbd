/*
-*- Linux-c -*-
   drbd.c
   Kernel module for 2.2.x/2.4.x Kernels

   This file is part of drbd by Philipp Reisner.

   Copyright (C) 1999-2001, Philipp Reisner <philipp.reisner@gmx.at>.
        main author.

   Copyright (C) 2000, Marcelo Tosatti <marcelo@conectiva.com.br>.
        Early 2.3.x work.

   Copyright (C) 2001, Lelik P.Korchagin <lelik@price.ru>.
        Initial devfs support.

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


/*
  By introducing a "Shared" state beside "Primary" and "Secondary" for
  use with GFS at least the following items need to be done.
  *) transfer_log and epoch_set reside in the same memory now.
  *) writes on the receiver side must be done with a temporary
     buffer_head directly to the lower level device. 
     Otherwise we would get in an endless loop sending the same 
     block over all the time.
  *) All occurences of "Primary" or "Secondary" must be reviewed.
*/

#ifdef HAVE_AUTOCONF
#include <linux/autoconf.h>
#endif
#ifdef CONFIG_MODVERSIONS
#include <linux/modversions.h>
#endif

#include <asm/uaccess.h>
#include <asm/bitops.h> 
#include <net/sock.h>
#include <linux/module.h>
#include <linux/smp_lock.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/proc_fs.h>
#include <linux/init.h>
#include <linux/slab.h>
#define __KERNEL_SYSCALLS__
#include <linux/unistd.h>

#include "drbd.h"
#include "drbd_int.h"

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,4,0)
#ifdef CONFIG_DEVFS_FS
#include <linux/devfs_fs_kernel.h>
static devfs_handle_t devfs_handle;
#endif
#endif

static int errno;

/* #define ES_SIZE_STATS 50 */

int drbdd_init(struct Drbd_thread*);
int drbd_syncer(struct Drbd_thread*);
int drbd_asender(struct Drbd_thread*);

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,3,0)
/*static */ int drbd_proc_get_info(char *, char **, off_t, int, int *,
				   void *);
/*static */ void drbd_do_request(request_queue_t *);
#else
/*static */ int drbd_proc_get_info(char *, char **, off_t, int, int);
/*static */ void drbd_do_request(void);
#endif
/*static */ void drbd_dio_end(struct buffer_head *bh, int uptodate);

int drbd_init(void);
/*static */ int drbd_open(struct inode *inode, struct file *file);
/*static */ int drbd_close(struct inode *inode, struct file *file);
/*static */ int drbd_ioctl(struct inode *inode, struct file *file,
			   unsigned int cmd, unsigned long arg);
/*static */ int drbd_fsync(struct file *file, struct dentry *dentry);

int drbd_send(struct Drbd_Conf *, Drbd_Packet*, size_t , void* , size_t,int );

#ifdef DEVICE_REQUEST
#undef DEVICE_REQUEST
#endif
#define DEVICE_REQUEST drbd_do_request

MODULE_AUTHOR("Philipp Reisner <philipp.reisner@gmx.at>");
MODULE_DESCRIPTION("drbd - Distributed Replicated Block Device");
MODULE_LICENSE("GPL");
MODULE_PARM(minor_count,"i");
MODULE_PARM_DESC(minor_count, "Maximum number of drbd devices (1-255)");

/*static */ int *drbd_blocksizes;
/*static */ int *drbd_sizes;
struct Drbd_Conf *drbd_conf;
int minor_count=2;

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,3,40)
/*static */ struct block_device_operations drbd_ops = {
	open:		drbd_open,
	release:	drbd_close,
	ioctl:          drbd_ioctl
};
#else
/*static */ struct file_operations drbd_ops =
{
	read:           block_read,
	write:          block_write,
	ioctl:          drbd_ioctl,
	open:           drbd_open,
	release:        drbd_close,
	fsync:          block_fsync
};
#endif

#define ARRY_SIZE(A) (sizeof(A)/sizeof(A[0]))


int drbd_log2(int i)
{
	int bits = 0;
	int add_one=0; /* In case there is not a whole-numbered solution,
			  round up */
	while (i != 1) {
		bits++;
		if ( (i & 1) == 1) add_one=1;
		i >>= 1;
	}
	return bits+add_one;
}



/************************* The transfer log start */
#define TL_BARRIER    0
#define TL_FINISHED   ((drbd_request_t *)(-1))
/* spinlock readme:
   tl_dependence() only needs a read-lock and is called from interrupt time.
   See Documentation/spinlocks.txt why this is valid.
*/

#if 0
void print_tl(struct Drbd_Conf *mdev)
{
	struct tl_entry* p=mdev->tl_begin;

	printk(KERN_ERR "TransferLog (oldest entry first):\n");

	while( p != mdev->tl_end ) {
		if(p->req == TL_BARRIER)
			printk(KERN_ERR "BARRIER \n");
		else
			printk(KERN_ERR "Sector %ld.\n",p->sector);
		
		p++;
		if (p == mdev->transfer_log + mdev->conf.tl_size) 
			p = mdev->transfer_log;
	}
	
	printk(KERN_ERR "TransferLog (end)\n");
}
#endif

inline void tl_add(struct Drbd_Conf *mdev, drbd_request_t * new_item)
{
	unsigned long flags;
	int cur_size;

	write_lock_irqsave(&mdev->tl_lock,flags);


	  /*printk(KERN_ERR DEVICE_NAME "%d: tl_add(%ld)\n",
	    (int)(mdev-drbd_conf),new_item->sector);*/

	mdev->tl_end->req = new_item;
	mdev->tl_end->sector = GET_SECTOR(new_item);

	mdev->tl_end++;

	if (mdev->tl_end == mdev->transfer_log + mdev->conf.tl_size)
		mdev->tl_end = mdev->transfer_log;

	// Issue a barrier if tl fills up:
	cur_size = mdev->tl_end - mdev->tl_begin;
	if(cur_size < 0) {
		cur_size = mdev->conf.tl_size + cur_size;
	}
	if(cur_size*4 == mdev->conf.tl_size*3) {
		set_bit(ISSUE_BARRIER,&mdev->flags);
	}       

	if (mdev->tl_end == mdev->tl_begin)
		printk(KERN_CRIT DEVICE_NAME "%d: transferlog too small!! \n",
		 (int)(mdev-drbd_conf));

	/* DEBUG CODE */
	/* look if this block is alrady in this epoch */
#if 0
	/* print_tl(mdev); */


	{
		struct tl_entry* p=mdev->tl_end;
		while( p != mdev->tl_begin ) {

			if(p->req==TL_BARRIER || p->req==TL_FINISHED) break;
			if(p->sector == GET_SECTOR(new_item)) {
				printk(KERN_CRIT DEVICE_NAME 
				       "%d: Sector %ld is already in !!\n",
				       (int)(mdev-drbd_conf),
				       GET_SECTOR(new_item));
			}

			p--;
			if (p == mdev->transfer_log) 
				p = mdev->transfer_log + mdev->conf.tl_size;
		}
	}
#endif


	write_unlock_irqrestore(&mdev->tl_lock,flags);
}

inline unsigned int tl_add_barrier(struct Drbd_Conf *mdev)
{
	unsigned long flags;
	unsigned int bnr;
	static int barrier_nr_issue=1;

	write_lock_irqsave(&mdev->tl_lock,flags);

	/*printk(KERN_DEBUG DEVICE_NAME "%d: tl_add(TL_BARRIER)\n",
	  (int)(mdev-drbd_conf));*/

	bnr=barrier_nr_issue++; 
	mdev->tl_end->req = TL_BARRIER;
	mdev->tl_end->sector = bnr;

	mdev->tl_end++;

	if (mdev->tl_end == mdev->transfer_log + mdev->conf.tl_size)
		mdev->tl_end = mdev->transfer_log;

	if (mdev->tl_end == mdev->tl_begin)
		printk(KERN_CRIT DEVICE_NAME "%d: transferlog too small!!\n",
		       (int)(mdev-drbd_conf));

	write_unlock_irqrestore(&mdev->tl_lock,flags);

	return bnr;
}

void tl_release(struct Drbd_Conf *mdev,unsigned int barrier_nr,
		       unsigned int set_size)
{
        int epoch_size=0; 
	unsigned long flags;
	write_lock_irqsave(&mdev->tl_lock,flags);

	/* printk(KERN_DEBUG DEVICE_NAME ": tl_release(%u)\n",barrier_nr); */

	if (mdev->tl_begin->req == TL_BARRIER) epoch_size--;

	do {
		mdev->tl_begin++;

		if (mdev->tl_begin == mdev->transfer_log + mdev->conf.tl_size)
			mdev->tl_begin = mdev->transfer_log;

		if (mdev->tl_begin == mdev->tl_end)
			printk(KERN_ERR DEVICE_NAME "%d: tl messed up!\n",
			       (int)(mdev-drbd_conf));
		epoch_size++;
	} while (mdev->tl_begin->req != TL_BARRIER);

	if(mdev->tl_begin->sector != barrier_nr) //CHK
		printk(KERN_ERR DEVICE_NAME "%d: invalid barrier number!!"
		       "found=%u, reported=%u\n",(int)(mdev-drbd_conf),
		       (unsigned int)mdev->tl_begin->sector,barrier_nr);

	if(epoch_size != set_size) /* CHK */
		printk(KERN_ERR DEVICE_NAME "%d: Epoch set size wrong!!"
		       "found=%d reported=%d \n",(int)(mdev-drbd_conf),
		       epoch_size,set_size);

	write_unlock_irqrestore(&mdev->tl_lock,flags);

#ifdef ES_SIZE_STATS
	mdev->essss[set_size]++;
#endif  

}

/* tl_dependence reports if this sector was present in the current
   epoch. 
   As side effect it clears also the pointer to the request if it
   was present in the transfert log. (Since tl_dependence indicates
   that IO is complete and that drbd_end_req() should not be called
   in case tl_clear has to be called due to interruption of the 
   communication) 
*/
int tl_dependence(struct Drbd_Conf *mdev, drbd_request_t * item)
{
	struct tl_entry* p;
	int r=TRUE;

	read_lock(&mdev->tl_lock);

	p = mdev->tl_end;
	while( TRUE ) {
		if ( p==mdev->tl_begin ) {r=FALSE; break;}
	        if ( p==mdev->transfer_log) {
			p = p + mdev->conf.tl_size;
			if ( p==mdev->tl_begin ) {r=FALSE; break;}
		}
		p--;
		if ( p->req == TL_BARRIER) r=FALSE;
		if ( p->req == item) {
			p->req=TL_FINISHED;
			break;
		}
	}

	read_unlock(&mdev->tl_lock);
	return r;
}

int tl_check_sector(struct Drbd_Conf *mdev, unsigned long sector)
{
	struct tl_entry* p;
	int r=TRUE;

	if((mdev->send_block<<(mdev->blk_size_b-9)) == sector) return TRUE;

	read_lock(&mdev->tl_lock);

	p = mdev->tl_end;
	while( TRUE ) {
		if ( p==mdev->tl_begin ) {r=FALSE; break;}
	        if ( p==mdev->transfer_log) {
			p = p + mdev->conf.tl_size;
			if ( p==mdev->tl_begin ) {r=FALSE; break;}
		}
		p--;
		if ( p->sector == sector) {
			drbd_request_t *req = p->req;
			if( req == TL_BARRIER) continue;
			if( req == TL_FINISHED) { r=FALSE; }
			else {
				if((req->rq_status&0xfffe)==RQ_DRBD_WRITTEN) {
					r=FALSE;
				}
			}
			break;
		}
	}

	read_unlock(&mdev->tl_lock);
	return r;
}

void tl_clear(struct Drbd_Conf *mdev)
{
	struct tl_entry* p = mdev->tl_begin;
	unsigned long flags;
	write_lock_irqsave(&mdev->tl_lock,flags);

	while(p != mdev->tl_end) {
	        if(p->req != TL_BARRIER) { 
			if(p->req != TL_FINISHED && 
			   ((p->req->rq_status & 0xfffe) != RQ_DRBD_SENT)) {
				drbd_end_req(p->req,RQ_DRBD_SENT,1);
				//dec_pending(mdev);
				goto mark;
			}
			
			if(mdev->conf.wire_protocol != DRBD_PROT_C ) {
			mark:
				bm_set_bit(mdev->mbds_id, 
					   p->sector >> (mdev->blk_size_b-9),
					   mdev->blk_size_b, SS_OUT_OF_SYNC);
			}
		} //else dec_pending(mdev);
		p++;
		if (p == mdev->transfer_log + mdev->conf.tl_size)
		        p = mdev->transfer_log;	    
	}
	tl_init(mdev);
	write_unlock_irqrestore(&mdev->tl_lock,flags);
}     

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,4,14) 
// Check when daemonize was introduced.
void daemonize(void)
{
        struct fs_struct *fs;

        exit_mm(current);

        current->session = 1;
        current->pgrp = 1;
        current->tty = NULL;

        exit_fs(current);       /* current->fs->count--; */
        fs = init_task.fs;
        current->fs = fs;
        atomic_inc(&fs->count);
        exit_files(current);
        current->files = init_task.files;
        atomic_inc(&current->files->count);
}
#endif


int drbd_thread_setup(void* arg)
{
	struct Drbd_thread *thi = (struct Drbd_thread *) arg;
	int retval;

	daemonize();

	down(&thi->mutex); //ensures that thi->task is set.

	retval = thi->function(thi);

	thi->task = 0;
	set_bit(COLLECT_ZOMBIES,&drbd_conf[thi->minor].flags);
	up(&thi->mutex); //allow thread_stop to proceed

	return retval;
}

void drbd_thread_init(int minor, struct Drbd_thread *thi,
		      int (*func) (struct Drbd_thread *))
{
	thi->task = NULL;
	init_MUTEX(&thi->mutex);
	thi->function = func;
	thi->minor = minor;
}

void drbd_thread_start(struct Drbd_thread *thi)
{
	int pid;

	if (thi->task == NULL) {
		thi->t_state = Running;

		down(&thi->mutex);
		pid = kernel_thread(drbd_thread_setup, (void *) thi, CLONE_FS);

		if (pid < 0) {
			printk(KERN_ERR DEVICE_NAME
			       "%d: Couldn't start thread (%d)\n", thi->minor,
			       pid);
			return;
		}
		/* printk(KERN_DEBUG DEVICE_NAME ": pid = %d\n", pid); */
		read_lock(&tasklist_lock);
		thi->task = find_task_by_pid(pid);
		read_unlock(&tasklist_lock);
		up(&thi->mutex);
	}
}


void _drbd_thread_stop(struct Drbd_thread *thi, int restart,int wait)
{
	if (!thi->task) return;

	if (restart)
		thi->t_state = Restarting;
	else
		thi->t_state = Exiting;

	drbd_queue_signal(SIGTERM,thi->task);

	if(wait) {
		down(&thi->mutex); // wait until thread has exited
		up(&thi->mutex);

		current->state = TASK_INTERRUPTIBLE;
		schedule_timeout(HZ / 10);
	}
}

int drbd_send_cmd(int minor,Drbd_Packet_Cmd cmd, int via_msock)
{
	int err;
	Drbd_Packet head;

	head.command = cpu_to_be16(cmd);
	if(!via_msock) down(&drbd_conf[minor].send_mutex);
	err = drbd_send(&drbd_conf[minor], &head,sizeof(head),0,0,via_msock);
	if(!via_msock) up(&drbd_conf[minor].send_mutex);

	return err;  
}

int drbd_send_param(int minor)
{
	Drbd_Parameter_Packet param;
	int err,i;
	kdev_t ll_dev = drbd_conf[minor].lo_device;

	
	param.h.size = cpu_to_be64( drbd_conf[minor].lo_usize ? 
				    drbd_conf[minor].lo_usize : 
				    blk_size[MAJOR(ll_dev)][MINOR(ll_dev)] );

	param.p.command = cpu_to_be16(ReportParams);
	param.h.blksize = cpu_to_be32(1 << drbd_conf[minor].blk_size_b);
	param.h.state = cpu_to_be32(drbd_conf[minor].state);
	param.h.protocol = cpu_to_be32(drbd_conf[minor].conf.wire_protocol);
	param.h.version = cpu_to_be32(PRO_VERSION);

	for(i=0;i<=PrimaryInd;i++) {
		param.h.gen_cnt[i]=cpu_to_be32(drbd_conf[minor].gen_cnt[i]);
		param.h.bit_map_gen[i]=
			cpu_to_be32(drbd_conf[minor].bit_map_gen[i]);
	}

	down(&drbd_conf[minor].send_mutex);
	err = drbd_send(&drbd_conf[minor], (Drbd_Packet*)&param, 
			sizeof(param),0,0,0);
	up(&drbd_conf[minor].send_mutex);
	
	if(err < sizeof(Drbd_Parameter_Packet))
		printk(KERN_ERR DEVICE_NAME
		       "%d: Sending of parameter block failed!!\n",minor);  

	return err;
}

int drbd_send_cstate(struct Drbd_Conf *mdev)
{
	Drbd_CState_Packet head;
	int ret;
	
	head.p.command = cpu_to_be16(CStateChanged);
	head.h.cstate = cpu_to_be32(mdev->cstate);

	down(&mdev->send_mutex);
	ret=drbd_send(mdev,(Drbd_Packet*)&head,sizeof(head),0,0,0);
	up(&mdev->send_mutex);
	return ret;

}

int _drbd_send_barrier(struct Drbd_Conf *mdev)
{
	int r;
        Drbd_Barrier_Packet head;

	/* tl_add_barrier() must be called with the send_mutex aquired */
	head.p.command = cpu_to_be16(Barrier);
	head.h.barrier=tl_add_barrier(mdev); 

	/* printk(KERN_DEBUG DEVICE_NAME": issuing a barrier\n"); */
       
	r=drbd_send(mdev,(Drbd_Packet*)&head,sizeof(head),0,0,0);

	inc_pending(mdev);

	return r;
}

int drbd_send_b_ack(struct Drbd_Conf *mdev, u32 barrier_nr,u32 set_size)
{
        Drbd_BarrierAck_Packet head;
	int ret;
       
	head.p.command = cpu_to_be16(BarrierAck);
        head.h.barrier = barrier_nr;
	head.h.set_size = cpu_to_be32(set_size);
	down(&mdev->send_mutex);
	ret=drbd_send(mdev,(Drbd_Packet*)&head,sizeof(head),0,0,0);
	up(&mdev->send_mutex);
	return ret;
}


int drbd_send_ack(struct Drbd_Conf *mdev, int cmd, unsigned long block_nr,
		  u64 block_id)
{
        Drbd_BlockAck_Packet head;
	int ret;

	head.p.command = cpu_to_be16(cmd);
	head.h.block_nr = cpu_to_be64(block_nr);
        head.h.block_id = block_id;
	down(&mdev->send_mutex);
	ret=drbd_send(mdev,(Drbd_Packet*)&head,sizeof(head),0,0,0);
	up(&mdev->send_mutex);
	return ret;
}

int drbd_send_block(struct Drbd_Conf *mdev, struct buffer_head *bh, 
			  u64 block_id)
{
        Drbd_Data_Packet head;
	int ret,ok;

	head.p.command = cpu_to_be16(Data);
	head.h.block_nr = cpu_to_be64(bh->b_blocknr);
	head.h.block_id = block_id;

	down(&mdev->send_mutex);
	
	if(test_and_clear_bit(ISSUE_BARRIER,&mdev->flags)) {
	        _drbd_send_barrier(mdev);
	}
	
	ret=drbd_send(mdev,(Drbd_Packet*)&head,sizeof(head),bh_kmap(bh),
		      bh->b_size,0);
	bh_kunmap(bh);
	ok=(ret == bh->b_size + sizeof(head));

	if( ok ) {
		if( mdev->conf.wire_protocol != DRBD_PROT_A || 
		    block_id == ID_SYNCER )  {
			inc_pending(mdev);
		}
		mdev->send_cnt+=bh->b_size>>10;
	}

	if(block_id != ID_SYNCER) {
		if( ok ) {
			/* This must be within the semaphore */
			tl_add(mdev,(drbd_request_t*)(unsigned long)block_id);
		} else {
			bm_set_bit(mdev->mbds_id,bh->b_blocknr,
				   mdev->blk_size_b,SS_OUT_OF_SYNC);
			ret=0;
		}
	}

	up(&mdev->send_mutex);

	return ret;
}

static void drbd_timeout(unsigned long arg)
{
	struct send_timer_info *ti = (struct send_timer_info *) arg;
	//	int i;

	if(ti->via_msock) {
		printk(KERN_ERR DEVICE_NAME"%d: sock_sendmsg time expired"
		       " on msock\n",
		       (int)(ti->mdev-drbd_conf));

		ti->timeout_happened=1;
		drbd_queue_signal(DRBD_SIG, ti->task);
		spin_lock(&ti->mdev->send_proc_lock);		
		if((ti=ti->mdev->send_proc)) {
			ti->timeout_happened=1;
			drbd_queue_signal(DRBD_SIG, ti->task);
		}
		spin_unlock(&ti->mdev->send_proc_lock);		
	} else {
		/*
		printk(KERN_ERR DEVICE_NAME"%d: sock_sendmsg time expired"
		       " (pid=%d) requesting ping\n",
		       (int)(ti->mdev-drbd_conf),ti->task->pid);
		*/
		set_bit(SEND_PING,&ti->mdev->flags);
		drbd_queue_signal(DRBD_SIG, ti->mdev->asender.task);

		if(ti->restart) {
			ti->s_timeout.expires = jiffies +
				(ti->mdev->conf.timeout * HZ / 10);
			add_timer(&ti->s_timeout);
		}
	}
}

void drbd_a_timeout(unsigned long arg)
{
	struct Drbd_Conf *mdev = (struct Drbd_Conf *) arg;

	/*
	printk(KERN_ERR DEVICE_NAME "%d: ack timeout detected (pc=%d)"
	       " requesting ping\n",
	       (int)(mdev-drbd_conf),atomic_read(&mdev->pending_cnt));
	*/
	set_bit(SEND_PING,&mdev->flags);
	drbd_queue_signal(DRBD_SIG, mdev->asender.task);
}

/*
  drbd_send distinqushes two cases:

  Packets sent via the data socket "sock"
  and packets sent via the meta data socket "msock"

                    sock                      msock
  -----------------+-------------------------+------------------------------
  timeout           conf.timeout              avg round trip time(artt) x 4
  timeout action    send a ping via msock     Abort communication
                                              and close all sockets
*/
int drbd_send(struct Drbd_Conf *mdev, Drbd_Packet* header, size_t header_size,
	      void* data, size_t data_size, int via_msock)
{
	mm_segment_t oldfs;
	sigset_t oldset;
	struct msghdr msg;
	struct iovec iov[2];
	unsigned long flags;
	int rv,sent=0;
	int app_got_sig=0;
	struct send_timer_info ti;
	struct socket *sock = via_msock ? mdev->msock : mdev->sock;
	
	if (!sock) return -1000;
	if (mdev->cstate < WFReportParams) return -1001;

	header->magic  =  cpu_to_be32(DRBD_MAGIC);
	header->length  = cpu_to_be16(data_size);

	sock->sk->allocation = GFP_DRBD;

	iov[0].iov_base = header;
	iov[0].iov_len = header_size;
	iov[1].iov_base = data;
	iov[1].iov_len = data_size;

	msg.msg_iov = iov;
	msg.msg_iovlen = data_size > 0 ? 2 : 1;
	msg.msg_control = NULL;
	msg.msg_controllen = 0;
	msg.msg_name = 0;
	msg.msg_namelen = 0;
	msg.msg_flags = MSG_NOSIGNAL;

	ti.mdev=mdev;
	ti.timeout_happened=0;
	ti.via_msock=via_msock;
	ti.task=current;
	ti.restart=1;
	if(!via_msock) {
		spin_lock(&mdev->send_proc_lock); 
		mdev->send_proc=&ti; 
		spin_unlock(&mdev->send_proc_lock);		
	}

	if (mdev->conf.timeout) {
		init_timer(&ti.s_timeout);
		ti.s_timeout.function = drbd_timeout;
		ti.s_timeout.data = (unsigned long) &ti;
		ti.s_timeout.expires = jiffies + mdev->conf.timeout*HZ/20;
		add_timer(&ti.s_timeout);
	}

	lock_kernel();  //  check if this is still necessary
	oldfs = get_fs();
	set_fs(KERNEL_DS);

	spin_lock_irqsave(&current->sigmask_lock, flags);
	oldset = current->blocked;
	sigfillset(&current->blocked);
	sigdelset(&current->blocked,DRBD_SIG); 
	recalc_sigpending(current);
	spin_unlock_irqrestore(&current->sigmask_lock, flags);

	while(1) {
		rv = sock_sendmsg(sock, &msg, header_size+data_size);
		if ( rv == -ERESTARTSYS) {
			spin_lock_irqsave(&current->sigmask_lock,flags);
			if (sigismember(SIGSET_OF(current), DRBD_SIG)) {
				sigdelset(SIGSET_OF(current), DRBD_SIG);
				recalc_sigpending(current);
				spin_unlock_irqrestore(&current->sigmask_lock,
						       flags);
				if(ti.timeout_happened) {
					break;
				} else {
					app_got_sig=1;
					continue;
				}
			}
			spin_unlock_irqrestore(&current->sigmask_lock,flags);
		}
		if (rv <= 0) break;
		sent += rv;
		if (sent == header_size+data_size) break;

		/*printk(KERN_ERR DEVICE_NAME
		       "%d: calling sock_sendmsg again\n",
		       (int)(mdev-drbd_conf));*/

		if( rv < header_size ) {
			iov[0].iov_base += rv;
			iov[0].iov_len  -= rv;
			header_size -= rv;
		} else /* rv >= header_size */ {
			if (header_size) {
				iov[0].iov_base = iov[1].iov_base;
				iov[0].iov_len = iov[1].iov_len;
				msg.msg_iovlen = 1;
				rv -= header_size;
				header_size = 0;
			}
			iov[0].iov_base += rv;
			iov[0].iov_len  -= rv;
			data_size -= rv;
		}
	}

	set_fs(oldfs);
	unlock_kernel();

	ti.restart=0;

	if (mdev->conf.timeout) {
		del_timer_sync(&ti.s_timeout);
	}

	if(!via_msock) {
		spin_lock(&mdev->send_proc_lock);		
		mdev->send_proc=NULL;
		spin_unlock(&mdev->send_proc_lock);		
	}


	spin_lock_irqsave(&current->sigmask_lock, flags);
	current->blocked = oldset;
	if(app_got_sig) {
		sigaddset(SIGSET_OF(current), DRBD_SIG);
	} else {
		sigdelset(SIGSET_OF(current), DRBD_SIG);
	}
	recalc_sigpending(current);
	spin_unlock_irqrestore(&current->sigmask_lock, flags);

	if (/*rv == -ERESTARTSYS &&*/ ti.timeout_happened) {
		printk(KERN_DEBUG DEVICE_NAME
		       "%d: send timed out!! (pid=%d)\n",
		       (int)(mdev-drbd_conf),current->pid);

		set_cstate(mdev,Timeout);
		
		drbd_thread_restart_nowait(&mdev->receiver);
		
		return -1002;
	}

	if (rv <= 0) {
		printk(KERN_ERR DEVICE_NAME "%d: sock_sendmsg returned %d\n",
		       (int)(mdev-drbd_conf),rv);

		set_cstate(mdev,BrokenPipe);
		drbd_thread_restart_nowait(&mdev->receiver);	  
	}

	return sent;
}

/*static */ int drbd_open(struct inode *inode, struct file *file)
{
	int minor;

	minor = MINOR(inode->i_rdev);
	if(minor >= minor_count) return -ENODEV;

	if (file->f_mode & FMODE_WRITE) {
		if( drbd_conf[minor].state == Secondary) {
			return -EROFS;
		}
		set_bit(WRITER_PRESENT, &drbd_conf[minor].flags);
	}

	drbd_conf[minor].open_cnt++;

	MOD_INC_USE_COUNT;

	return 0;
}

/*static */ int drbd_close(struct inode *inode, struct file *file)
{
	/* do not use *file (May be NULL, in case of a unmount :-) */
	int minor;

	minor = MINOR(inode->i_rdev);
	if(minor >= minor_count) return -ENODEV;

	/*
	printk(KERN_ERR DEVICE_NAME ": close(inode=%p,file=%p)"
	       "current=%p,minor=%d,wc=%d\n", inode, file, current, minor,
	       inode->i_writecount);
	*/

	if (--drbd_conf[minor].open_cnt == 0) {
		clear_bit(WRITER_PRESENT, &drbd_conf[minor].flags);
	}

	MOD_DEC_USE_COUNT;

	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,4,0)
void drbd_send_write_hint(void *data)
{
	struct Drbd_Conf* mdev = (struct Drbd_Conf*)data;
	
	drbd_send_cmd((int)(mdev-drbd_conf),WriteHint,0);
	clear_bit(WRITE_HINT_QUEUED, &mdev->flags);
}
#endif

int __init drbd_init(void)
{

	int i;
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,3,0)
	drbd_proc = create_proc_read_entry("drbd", 0, &proc_root,
					   drbd_proc_get_info, NULL);
	if (!drbd_proc)
#else
	if (proc_register(&proc_root, &drbd_proc_dir))
#endif
	{
		printk(KERN_ERR DEVICE_NAME": unable to register proc file\n");
		return -EIO;
	}

	if (register_blkdev(MAJOR_NR, DEVICE_NAME, &drbd_ops)) {

		printk(KERN_ERR DEVICE_NAME ": Unable to get major %d\n",
		       MAJOR_NR);

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,3,0)
		if (drbd_proc)
			remove_proc_entry("drbd", &proc_root);
#else
		proc_unregister(&proc_root, drbd_proc_dir.low_ino);
#endif

		return -EBUSY;
	}


#if LINUX_VERSION_CODE > KERNEL_VERSION(2,4,0)
#ifdef CONFIG_DEVFS_FS
	devfs_handle = devfs_mk_dir (NULL, "nbd", NULL);
	devfs_register_series(devfs_handle, "%u", minor_count,
			      DEVFS_FL_DEFAULT, MAJOR_NR, 0,
			      S_IFBLK | S_IRUSR | S_IWUSR,
		     	      &drbd_ops, NULL);
# endif
#endif

	drbd_blocksizes = kmalloc(sizeof(int)*minor_count,GFP_KERNEL);
	drbd_sizes = kmalloc(sizeof(int)*minor_count,GFP_KERNEL);
	drbd_conf = kmalloc(sizeof(struct Drbd_Conf)*minor_count,GFP_KERNEL);

	/* Initialize size arrays. */

	for (i = 0; i < minor_count; i++) {
		drbd_blocksizes[i] = INITIAL_BLOCK_SIZE;
		drbd_conf[i].blk_size_b = drbd_log2(INITIAL_BLOCK_SIZE);
		drbd_sizes[i] = 0;
		set_device_ro(MKDEV(MAJOR_NR, i), FALSE /*TRUE */ );
		drbd_conf[i].do_panic = 0;
		drbd_conf[i].sock = 0;
		drbd_conf[i].msock = 0;
		drbd_conf[i].lo_file = 0;
		drbd_conf[i].lo_device = 0;
		drbd_conf[i].state = Secondary;
		init_waitqueue_head(&drbd_conf[i].state_wait);
		drbd_conf[i].o_state = Unknown;
		drbd_conf[i].cstate = Unconfigured;
		drbd_conf[i].send_cnt = 0;
		drbd_conf[i].recv_cnt = 0;
		drbd_conf[i].writ_cnt = 0;
		drbd_conf[i].read_cnt = 0;
		atomic_set(&drbd_conf[i].pending_cnt,0);
		atomic_set(&drbd_conf[i].unacked_cnt,0);
		drbd_conf[i].transfer_log = 0;
		drbd_conf[i].mbds_id = 0;
		drbd_conf[i].flags=0;
		tl_init(&drbd_conf[i]);
		drbd_conf[i].a_timeout.function = drbd_a_timeout;
		drbd_conf[i].a_timeout.data = (unsigned long)(drbd_conf+i);
		init_timer(&drbd_conf[i].a_timeout);
		drbd_conf[i].synced_to=0;
		init_MUTEX(&drbd_conf[i].send_mutex);
		drbd_conf[i].send_proc=NULL;
		drbd_conf[i].send_proc_lock = SPIN_LOCK_UNLOCKED;
		drbd_thread_init(i, &drbd_conf[i].receiver, drbdd_init);
		drbd_thread_init(i, &drbd_conf[i].syncer, drbd_syncer);
		drbd_thread_init(i, &drbd_conf[i].asender, drbd_asender);
		drbd_conf[i].tl_lock = RW_LOCK_UNLOCKED;
		drbd_conf[i].ee_lock = SPIN_LOCK_UNLOCKED;
		drbd_conf[i].req_lock = SPIN_LOCK_UNLOCKED;
		drbd_conf[i].syncer_b = 0;
		drbd_conf[i].bb_lock = SPIN_LOCK_UNLOCKED;
		init_waitqueue_head(&drbd_conf[i].cstate_wait);
		drbd_conf[i].open_cnt = 0;
		drbd_conf[i].epoch_size=0;
		drbd_conf[i].send_block=-1;
		INIT_LIST_HEAD(&drbd_conf[i].free_ee);
		INIT_LIST_HEAD(&drbd_conf[i].active_ee);
		INIT_LIST_HEAD(&drbd_conf[i].sync_ee);
		INIT_LIST_HEAD(&drbd_conf[i].done_ee);
		INIT_LIST_HEAD(&drbd_conf[i].busy_blocks);
		drbd_conf[i].ee_vacant=0;
		drbd_conf[i].ee_in_use=0;
		drbd_init_ee(drbd_conf+i);
		init_waitqueue_head(&drbd_conf[i].ee_wait);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,4,0)
		drbd_conf[i].write_hint_tq.sync	= 0;
		drbd_conf[i].write_hint_tq.routine = &drbd_send_write_hint;
		drbd_conf[i].write_hint_tq.data = drbd_conf+i;
#endif

		{
			int j;
			for(j=0;j<=PrimaryInd;j++) drbd_conf[i].gen_cnt[j]=0;
			for(j=0;j<=PrimaryInd;j++) 
				drbd_conf[i].bit_map_gen[j]=0;
#ifdef ES_SIZE_STATS
			for(j=0;j<ES_SIZE_STATS;j++) drbd_conf[i].essss[j]=0;
#endif  
		}
	}
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,4,0)
	blk_queue_make_request(BLK_DEFAULT_QUEUE(MAJOR_NR),drbd_make_request);
	/*   blk_init_queue(BLK_DEFAULT_QUEUE(MAJOR_NR), NULL); */
#else
	blk_dev[MAJOR_NR].request_fn = DEVICE_REQUEST;
#endif
	blksize_size[MAJOR_NR] = drbd_blocksizes;
	blk_size[MAJOR_NR] = drbd_sizes;	/* Size in Kb */

	return 0;
}

int __init init_module()
{
	printk(KERN_INFO DEVICE_NAME ": initialised. "
	       "Version: " REL_VERSION " (api:%d/proto:%d)\n",
	       API_VERSION,PRO_VERSION);

	return drbd_init();

}

void cleanup_module()
{
	int i;

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,4,0)
#ifdef CONFIG_DEVFS_FS
	devfs_unregister(devfs_handle);
#endif
#endif
	for (i = 0; i < minor_count; i++) {
		drbd_set_state(i,Secondary);
		fsync_dev(MKDEV(MAJOR_NR, i));
		set_bit(DO_NOT_INC_CONCNT,&drbd_conf[i].flags);
		drbd_thread_stop(&drbd_conf[i].syncer);
		drbd_thread_stop(&drbd_conf[i].receiver);
		drbd_thread_stop(&drbd_conf[i].asender);
		drbd_free_resources(i);
		if (drbd_conf[i].transfer_log)
			kfree(drbd_conf[i].transfer_log);		    
		if (drbd_conf[i].mbds_id) bm_cleanup(drbd_conf[i].mbds_id);
		// free the receiver's stuff

		drbd_release_ee(drbd_conf+i,&drbd_conf[i].free_ee);
		if(drbd_release_ee(drbd_conf+i,&drbd_conf[i].active_ee) || 
		   drbd_release_ee(drbd_conf+i,&drbd_conf[i].sync_ee)   ||
		   drbd_release_ee(drbd_conf+i,&drbd_conf[i].done_ee) ) {
			printk(KERN_ERR DEVICE_NAME
			       "%d: EEs in active/sync/done list found!\n",i);
		}
	}

	if (unregister_blkdev(MAJOR_NR, DEVICE_NAME) != 0)
		printk(KERN_ERR DEVICE_NAME": unregister of device failed\n");


#if LINUX_VERSION_CODE < KERNEL_VERSION(2,4,0)
	blk_dev[MAJOR_NR].request_fn = NULL;
	/*
#else
	blk_cleanup_queue(BLK_DEFAULT_QUEUE(MAJOR_NR));
	*/
#endif

	blksize_size[MAJOR_NR] = NULL;
	blk_size[MAJOR_NR] = NULL;
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,3,0)
	if (drbd_proc)
		remove_proc_entry("drbd", &proc_root);
#else
	proc_unregister(&proc_root, drbd_proc_dir.low_ino);
#endif

	kfree(drbd_blocksizes);
	kfree(drbd_sizes);
	kfree(drbd_conf);
}



void drbd_free_ll_dev(int minor)
{
	if (drbd_conf[minor].lo_file) {
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,3,0)
		blkdev_put(drbd_conf[minor].lo_file->f_dentry->d_inode->i_bdev,
			   BDEV_FILE);
#else
		blkdev_release(drbd_conf[minor].lo_file->f_dentry->
			       d_inode);
#endif
		fput(drbd_conf[minor].lo_file);
		drbd_conf[minor].lo_file = 0;
		drbd_conf[minor].lo_device = 0;
	}
}

void drbd_free_sock(int minor)
{
	if (drbd_conf[minor].sock) {
		sock_release(drbd_conf[minor].sock);
		drbd_conf[minor].sock = 0;
	}
	if (drbd_conf[minor].msock) {
		sock_release(drbd_conf[minor].msock);
		drbd_conf[minor].msock = 0;
	}

}


void drbd_free_resources(int minor)
{
	drbd_free_sock(minor);
	drbd_free_ll_dev(minor);
}

/*********************************/

/*** The bitmap stuff. ***/
/*
  We need to store one bit for a block. 
  Example: 1GB disk @ 4096 byte blocks ==> we need 32 KB bitmap.
  Bit 0 ==> Primary and secondary nodes are in sync.
  Bit 1 ==> secondary node's block must be updated. (')

  A wicked bug was found and pointed out by 
                     Guzovsky, Eduard <EGuzovsky@crossbeamsys.com>
*/

#include <asm/types.h>
#include <linux/vmalloc.h>

#define BM_BLOCK_SIZE_B  12  
#define BM_BLOCK_SIZE    (1<<12)

#define BM_IN_SYNC       0
#define BM_OUT_OF_SYNC   1

#if BITS_PER_LONG == 32
#define LN2_BPL 5
#elif BITS_PER_LONG == 64
#define LN2_BPL 6
#else
#error "LN2 of BITS_PER_LONG unknown!"
#endif

struct BitMap {
	kdev_t dev;
	unsigned long size;
	unsigned long* bm;
	unsigned long sb_bitnr;
	unsigned long sb_mask;
	unsigned long gb_bitnr;
	unsigned long gb_snr;
	spinlock_t bm_lock;
};


// Shift right with round up. :)
#define SR_RU(A,B) ( ((A)>>(B)) + ( ((A) & ((1<<(B))-1)) > 0 ? 1 : 0 ) )

struct BitMap* bm_init(kdev_t dev)
{
        struct BitMap* sbm;
	unsigned long size;

	size = SR_RU(blk_size[MAJOR(dev)][MINOR(dev)],
		     (BM_BLOCK_SIZE_B - (10 - LN2_BPL))) << (LN2_BPL-3);
	/* 10 => blk_size is KB ; 3 -> 2^3=8 Bits per Byte */
	// Calculate the number of long words needed, round it up, and
	// finally convert it to bytes.

	if(size == 0) return 0;

	sbm = vmalloc(size + sizeof(struct BitMap));

	sbm->dev = dev;
	sbm->size = size;
	sbm->bm = (unsigned long*)((char*)sbm + sizeof(struct BitMap));
	sbm->sb_bitnr=0;
	sbm->sb_mask=0;
	sbm->gb_bitnr=0;
	sbm->gb_snr=0;
	sbm->bm_lock = SPIN_LOCK_UNLOCKED;

	memset(sbm->bm,0,size);
	
	/*
	printk(KERN_INFO DEVICE_NAME " : vmallocing %ld B for bitmap."
	       " @%p\n",size,sbm->bm);
	*/
  
	return sbm;
}     

void bm_cleanup(void* bm_id)
{
        vfree(bm_id);
}

/* THINK:
   What happens when the block_size (ln2_block_size) changes between
   calls 
*/

void bm_set_bit(struct BitMap* sbm,unsigned long blocknr,int ln2_block_size, int bit)
{
        unsigned long* bm;
	unsigned long bitnr;
	int cb = (BM_BLOCK_SIZE_B-ln2_block_size);

	if(sbm == NULL) {
		printk(KERN_ERR DEVICE_NAME"X: You need to specify the "
		       "device size!\n");
		return;
	}

	bm = sbm->bm;
	bitnr = blocknr >> cb;

 	spin_lock(&sbm->bm_lock);

	if(!bit && cb) {
		if(sbm->sb_bitnr == bitnr) {
		        sbm->sb_mask |= 1L << (blocknr & ((1L<<cb)-1));
			if(sbm->sb_mask != (1L<<(1<<cb))-1) goto out;
		} else {
	                sbm->sb_bitnr = bitnr;
			sbm->sb_mask = 1L << (blocknr & ((1L<<cb)-1));
			goto out;
		}
	}

	if(bitnr>>3 >= sbm->size) { // 3 -> 2^3=8 Bit per Byte
		printk(KERN_ERR DEVICE_NAME" : BitMap too small!\n");	  
		goto out;
	}

	bm[bitnr>>LN2_BPL] = bit ?
	  bm[bitnr>>LN2_BPL] |  ( 1L << (bitnr & ((1L<<LN2_BPL)-1)) ) :
	  bm[bitnr>>LN2_BPL] & ~( 1L << (bitnr & ((1L<<LN2_BPL)-1)) );

 out:
	spin_unlock(&sbm->bm_lock);
}

static inline int bm_get_bn(unsigned long word,int nr)
{
	if(nr == BITS_PER_LONG-1) return -1;
	word >>= ++nr;
	while (! (word & 1)) {
                word >>= 1;
		if (++nr == BITS_PER_LONG) return -1;
	}
	return nr;
}

unsigned long bm_get_blocknr(struct BitMap* sbm,int ln2_block_size)
{
        unsigned long* bm = sbm->bm;
	unsigned long wnr;
	unsigned long nw = sbm->size/sizeof(unsigned long);
	unsigned long rv;
	int cb = (BM_BLOCK_SIZE_B-ln2_block_size);

 	spin_lock(&sbm->bm_lock);

	if(sbm->gb_snr >= (1L<<cb)) {	  
		for(wnr=sbm->gb_bitnr>>LN2_BPL;wnr<nw;wnr++) {
	                if (bm[wnr]) {
				int bnr;
				if (wnr == sbm->gb_bitnr>>LN2_BPL)
					bnr = sbm->gb_bitnr & ((1<<LN2_BPL)-1);
				else bnr = -1;
				bnr = bm_get_bn(bm[wnr],bnr);
				if (bnr == -1) continue; 
			        sbm->gb_bitnr = (wnr<<LN2_BPL) + bnr;
				sbm->gb_snr = 0;
				goto out;
			}
		}
		rv = MBDS_DONE;
		goto r_out;
	}
 out:
	rv = (sbm->gb_bitnr<<cb) + sbm->gb_snr++;
 r_out:
 	spin_unlock(&sbm->bm_lock);	
	return rv;
}

void bm_reset(struct BitMap* sbm,int ln2_block_size)
{
 	spin_lock(&sbm->bm_lock);

	sbm->gb_bitnr=0;
	if (sbm->bm[0] & 1) sbm->gb_snr=0;
	else sbm->gb_snr = 1L<<(BM_BLOCK_SIZE_B-ln2_block_size);

 	spin_unlock(&sbm->bm_lock);	
}
/*********************************/
/* meta data management */

void drbd_md_write(int minor)
{
	u32 buffer[6];
	mm_segment_t oldfs;
	struct inode* inode;
	struct file* fp;
	char fname[25];
	int i;		

	drbd_conf[minor].gen_cnt[PrimaryInd]=(drbd_conf[minor].state==Primary);
	
	for(i=0;i<=PrimaryInd;i++) 
		buffer[i]=cpu_to_be32(drbd_conf[minor].gen_cnt[i]);
	buffer[MagicNr]=cpu_to_be32(DRBD_MAGIC);
	
	sprintf(fname,DRBD_MD_FILES,minor);
	fp=filp_open(fname,O_WRONLY|O_CREAT|O_TRUNC|O_SYNC,00600);
	if(IS_ERR(fp)) goto err;
        oldfs = get_fs();
        set_fs(get_ds());
	inode = fp->f_dentry->d_inode;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,4,0)
        down(&inode->i_sem);
#endif
	i=fp->f_op->write(fp,(const char*)&buffer,sizeof(buffer),&fp->f_pos);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,4,0)
	up(&inode->i_sem);
#endif
	set_fs(oldfs);
	filp_close(fp,NULL);
	if (i==sizeof(buffer)) return;
 err:
	printk(KERN_ERR DEVICE_NAME 
	       "%d: Error writing state file\n\"%s\"\n",minor,fname);
	return;
}

void drbd_md_read(int minor)
{
	u32 buffer[6];
	mm_segment_t oldfs;
	struct inode* inode;
	struct file* fp;
	char fname[25];
	int i;		

	sprintf(fname,DRBD_MD_FILES,minor);
	fp=filp_open(fname,O_RDONLY,0);
	if(IS_ERR(fp)) goto err;
        oldfs = get_fs();
        set_fs(get_ds());
	inode = fp->f_dentry->d_inode;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,4,0)
        down(&inode->i_sem); 
#endif
	i=fp->f_op->read(fp,(char*)&buffer,sizeof(buffer),&fp->f_pos);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,4,0)
	up(&inode->i_sem);
#endif
	set_fs(oldfs);
	filp_close(fp,NULL);

	if(i != sizeof(buffer)) goto err;
	if(be32_to_cpu(buffer[MagicNr]) != DRBD_MAGIC) goto err;
	for(i=0;i<=PrimaryInd;i++) 
		drbd_conf[minor].gen_cnt[i]=be32_to_cpu(buffer[i]);

	return;
 err:
	printk(KERN_ERR DEVICE_NAME 
	       "%d: Error reading state file\n\"%s\"\n",minor,fname);
	for(i=0;i<PrimaryInd;i++) drbd_conf[minor].gen_cnt[i]=1;
	drbd_conf[minor].gen_cnt[PrimaryInd]=
		(drbd_conf[minor].state==Primary);
	drbd_md_write(minor);
	return;
}


/* Returns  1 if I have the good bits,
            0 if both are nice
	   -1 if the partner has the good bits.
*/
int drbd_md_compare(int minor,Drbd_Parameter_P* partner)
{
	int i;
	u32 me,other;
	
	for(i=0;i<=PrimaryInd;i++) {
		me=drbd_conf[minor].gen_cnt[i];
		other=be32_to_cpu(partner->gen_cnt[i]);
		if( me > other ) return 1;
		if( me < other ) return -1;
	}
	return 0;
}

/* Returns  1 if SyncingQuick is sufficient
            0 if SyncAll is needed.
*/
int drbd_md_syncq_ok(int minor,Drbd_Parameter_P* partner,int i_am_pri)
{
	int i;
	u32 me,other;

	// crash during sync forces SyncAll:
	if( (i_am_pri && be32_to_cpu(partner->gen_cnt[Consistent])==0) ||
	    (!i_am_pri && drbd_conf[minor].gen_cnt[Consistent]==0)) return 0;

	// primary crash forces SyncAll:
	if( (i_am_pri && be32_to_cpu(partner->gen_cnt[PrimaryInd])==1) ||
	    (!i_am_pri && drbd_conf[minor].gen_cnt[PrimaryInd]==1)) return 0;

	// If partner's GC not equal our bitmap's GC force SyncAll
	if( i_am_pri ) {
		for(i=HumanCnt;i<=ArbitraryCnt;i++) {
			me=drbd_conf[minor].bit_map_gen[i];
			other=be32_to_cpu(partner->gen_cnt[i]);
			if( me != other ) return 0;
		}
	} else { // !i_am_pri 
		for(i=HumanCnt;i<=ArbitraryCnt;i++) {
			me=drbd_conf[minor].gen_cnt[i];
			other=be32_to_cpu(partner->bit_map_gen[i]);
			if( me != other ) return 0;
		}
	}

	// SyncQuick sufficient
	return 1;
}

void drbd_md_inc(int minor, enum MetaDataIndex order)
{
	drbd_conf[minor].gen_cnt[order]++;
}

void drbd_queue_signal(int signal,struct task_struct *task)
{
	unsigned long flags;

  	read_lock(&tasklist_lock);
	if (task) {
		spin_lock_irqsave(&task->sigmask_lock, flags);
		sigaddset(SIGSET_OF(task), signal);
		recalc_sigpending(task);
		spin_unlock_irqrestore(&task->sigmask_lock, flags);
		if (task->state & TASK_INTERRUPTIBLE) wake_up_process(task);
	}
	read_unlock(&tasklist_lock);
}

