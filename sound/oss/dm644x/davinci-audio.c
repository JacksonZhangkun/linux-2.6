/*
 * linux/sound/oss/davinci-audio.c
 *
 * Common audio handling for the Davinci processors
 *
 * Copyright (C) 2006 Texas Instruments, Inc.
 *
 * Copyright (C) 2000, 2001 Nicolas Pitre <nico@cam.org>
 *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * THIS PACKAGE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 * History:
 *
 * 2004/08/12   Nishanth Menon - Modified to integrate Audio requirements on
 * 					1610,1710 platforms
 *
 * 2004-11-01   Nishanth Menon - modified to support 16xx and 17xx
 *                platform multi channel chaining.
 *
 * 2004-11-04   Nishanth Menon - Added support for power management
 *
 * 2004-12-17   Nishanth Menon - Provided proper module handling support
 *
 * 2005-10-01   Rishi Bhattacharya - Adapted to TI Davinci Family of processors
 */

/***************************** INCLUDES ************************************/

#include <linux/config.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/poll.h>
#include <linux/pm.h>
#include <linux/errno.h>
#include <linux/sound.h>
#include <linux/soundcard.h>
#include <linux/sysrq.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/completion.h>

#include <asm/uaccess.h>
#include <asm/io.h>
#include <asm/hardware.h>
#include <asm/semaphore.h>

#include "davinci-audio-dma-intfc.h"
#include "davinci-audio.h"

/***************************** MACROS ************************************/

#undef DEBUG
/* #define DEBUG */
#ifdef DEBUG
#define DPRINTK  printk
#define FN_IN printk(KERN_DEBUG "[davinci_audio.c:[%s] start\n", __FUNCTION__)
#define FN_OUT(n) printk(KERN_DEBUG "[davinci_audio.c:[%s] end(%d)\n", \
			 __FUNCTION__ , n)
#else
#define DPRINTK( x... )
#define FN_IN
#define FN_OUT(x)
#endif

#define DAVINCI_AUDIO_NAME              "davinci-audio"

#define AUDIO_NBFRAGS_DEFAULT   4
#define AUDIO_FRAGSIZE_DEFAULT   3072

/*************/

/* HACK ALERT!: These values will bave to be tuned as this is a trade off b/w
 * Sampling Rate vs buffer size and delay we are prepared to do before giving up
 */
#define MAX_QUEUE_FULL_RETRIES 1000000
#define QUEUE_WAIT_TIME        10

#define AUDIO_ACTIVE(state)     ((state)->rd_ref || (state)->wr_ref)

#define SPIN_ADDR               (dma_addr_t)0
#define SPIN_SIZE               2048

/********************** MODULES SPECIFIC FUNCTION PROTOTYPES ***************/

static int audio_write(struct file *file, const char *buffer,
		       size_t count, loff_t *ppos);

static int audio_read(struct file *file, char *buffer, size_t count,
		      loff_t *ppos);

static int audio_mmap(struct file *file, struct vm_area_struct *vma);

static unsigned int audio_poll(struct file *file,
			       struct poll_table_struct *wait);

static loff_t audio_llseek(struct file *file, loff_t offset, int origin);

static int audio_ioctl(struct inode *inode, struct file *file, uint cmd,
		       ulong arg);

static int audio_open(struct inode *inode, struct file *file);

static int audio_release(struct inode *inode, struct file *file);

static int audio_probe(struct device *dev);

static int audio_remove(struct device *dev);

static void audio_shutdown(struct device *dev);

static int audio_suspend(struct device *dev, u32 state, u32 level);

static int audio_resume(struct device *dev, u32 level);

static void audio_free(struct device *dev);

/***************************** Data Structures ********************************/

/*
 * The function pointer set to be registered by the codec.
 */
static audio_state_t audio_state = { 0 };

/* DMA Call back function */
static dma_callback_t audio_dma_callback;

/* File Ops structure */
static struct file_operations davinci_audio_fops = {
	.open = audio_open,
	.release = audio_release,
	.write = audio_write,
	.read = audio_read,
	.mmap = audio_mmap,
	.poll = audio_poll,
	.ioctl = audio_ioctl,
	.llseek = audio_llseek,
	.owner = THIS_MODULE
};

/* Driver information */
static struct device_driver davinci_audio_driver = {
	.name = DAVINCI_AUDIO_NAME,
	.bus = &platform_bus_type,
	.probe = audio_probe,
	.remove = audio_remove,
	.suspend = audio_suspend,
	.resume = audio_resume,
	.shutdown = audio_shutdown,
};

/* Device Information */
static struct platform_device davinci_audio_device = {
	.name = DAVINCI_AUDIO_NAME,
	.dev = {
		.driver_data = &audio_state,
		.release = audio_free,
		},
	.id = 0,
};

/***************************** GLOBAL FUNCTIONs *******************************/

/* Power Management Functions for Linux Device Model  */
/* DEBUG PUPOSES ONLY! */
#ifdef CONFIG_PM
/* #undef CONFIG_PM */
#endif

#ifdef CONFIG_PM
/*******************************************************************************
 *
 * audio_ldm_suspend(): Suspend operation
 *
 ******************************************************************************/
static int audio_ldm_suspend(void *data)
{
	audio_state_t *state = data;

	FN_IN;

	/* Reject the suspend request if we are already actively transmitting
	   data.
	   Rationale: We dont want to be suspended while in the middle of a
	   call!
	 */

	if (AUDIO_ACTIVE(state) && state->hw_init) {
		DPRINTK("Audio device Active, Cannot Suspend");
		return -EPERM;

		/* NOTE:
		 * This Piece of code is commented out in hope
		 * That one day we would need to suspend the device while
		 * audio operations are in progress and resume the operations
		 * once the resume is done.
		 * This is just a sample implementation of how it could be done.
		 * Currently NOT SUPPORTED
		 */
		/*
		   audio_stream_t *is = state->input_stream;
		   audio_stream_t *os = state->output_stream;
		   int stopstate;

		   if (is && is->buffers) {
		   DPRINTK("IS Suspend\n");
		   stopstate = is->stopped;
		   audio_stop_dma(is);
		   DMA_CLEAR(is);
		   is->dma_spinref = 0;
		   is->stopped = stopstate;
		   }
		   if (os && os->buffers) {
		   DPRINTK("OS Suspend\n");
		   stopstate = os->stopped;
		   audio_stop_dma(os);
		   DMA_CLEAR(os);
		   os->dma_spinref = 0;
		   os->stopped = stopstate;
		   }
		*/
	}

	FN_OUT(0);
	return 0;
}

/*******************************************************************************
 *
 * audio_ldm_resume(): Resume Operations
 *
 ******************************************************************************/
static int audio_ldm_resume(void *data)
{
	audio_state_t *state = data;

	FN_IN;
	if (AUDIO_ACTIVE(state) && state->hw_init) {
		/* Should never occur - since we never suspend with active state
		 */

		BUG();
		return -EPERM;

		/* NOTE:
		 * This Piece of code is commented out in hope
		 * That one day we would need to suspend the device while
		 * audio operations are in progress and resume the operations
		 * once the resume is done.
		 * This is just a sample implementation of how it could be done.
		 * Currently NOT SUPPORTED
		 */
		/*
		  audio_stream_t *is = state->input_stream;
		  audio_stream_t *os = state->output_stream;
		  if (os && os->buffers) {
		  DPRINTK("OS Resume\n");
		  audio_reset(os);
		  audio_process_dma(os);
		  }
		  if (is && is->buffers) {
		  DPRINTK("IS Resume\n");
		  audio_reset(is);
		  audio_process_dma(is);
		  }
		*/
	}
	FN_OUT(0);
	return 0;
}
#endif				/* End of #ifdef CONFIG_PM */

/*******************************************************************************
 *
 * audio_free(): The Audio driver release function
 * This is a dummy function required by the platform driver
 *
 ******************************************************************************/
static void audio_free(struct device *dev)
{
	/* Nothing to Release! */
}

/*******************************************************************************
 *
 * audio_probe(): The Audio driver probe function
 * WARNING!!!!  : It is expected that the codec would have registered with us by
 * now
 *
 ******************************************************************************/
static int audio_probe(struct device *dev)
{
	int ret;
	FN_IN;
	if (!audio_state.hw_probe) {
		DPRINTK("Probe Function Not Registered\n");
		return -ENODEV;
	}
	ret = audio_state.hw_probe();
	FN_OUT(ret);
	return ret;
}

/*******************************************************************************
 *
 * audio_remove() Function to handle removal operations
 *
 ******************************************************************************/
static int audio_remove(struct device *dev)
{
	FN_IN;
	if (audio_state.hw_remove) {
		audio_state.hw_remove();
	}
	FN_OUT(0);
	return 0;
}

/*******************************************************************************
 *
 * audio_shutdown(): Function to handle shutdown operations
 *
 ******************************************************************************/
static void audio_shutdown(struct device *dev)
{
	FN_IN;
	if (audio_state.hw_cleanup) {
		audio_state.hw_cleanup();
	}
	FN_OUT(0);
	return;
}

/*******************************************************************************
 *
 * audio_suspend(): Function to handle suspend operations
 *
 ******************************************************************************/
static int audio_suspend(struct device *dev, u32 state, u32 level)
{
	int ret = 0;

#ifdef CONFIG_PM
	void *data = dev->driver_data;
	FN_IN;
	if (level != 3) {
		return 0;
	}
	if (audio_state.hw_suspend) {
		ret = audio_ldm_suspend(data);
		if (ret == 0)
			ret = audio_state.hw_suspend();
	}
	if (ret) {
		DPRINTK("Audio Suspend Failed \n");
	} else {
		DPRINTK("Audio Suspend Success \n");
	}
#endif				/* CONFIG_PM */

	FN_OUT(ret);
	return ret;
}

/*******************************************************************************
 *
 * audio_resume(): Function to handle resume operations
 *
 ******************************************************************************/
static int audio_resume(struct device *dev, u32 level)
{
	int ret = 0;

#ifdef  CONFIG_PM
	void *data = dev->driver_data;
	FN_IN;
	if (level != 0) {
		return 0;
	}
	if (audio_state.hw_resume) {
		ret = audio_ldm_resume(data);
		if (ret == 0)
			ret = audio_state.hw_resume();
	}
	if (ret) {
		DPRINTK(" Audio Resume Failed \n");
	} else {
		DPRINTK(" Audio Resume Success \n");
	}
#endif				/* CONFIG_PM */

	FN_OUT(ret);
	return ret;
}

/*******************************************************************************
 *
 * audio_get_fops(): Return the fops required to get the function pointers of
 *                   DAVINCI Audio Driver
 *
 ******************************************************************************/
struct file_operations *audio_get_fops(void)
{
	FN_IN;
	FN_OUT(0);
	return &davinci_audio_fops;
}
EXPORT_SYMBOL(audio_get_fops);

/*******************************************************************************
 *
 * audio_register_codec(): Register a Codec fn points using this function
 * WARNING!!!!!          : Codecs should ensure that they do so! no sanity
 * 			checks during runtime is done due to obvious performance
 * 			penalties.
 *
 ******************************************************************************/
int audio_register_codec(audio_state_t *codec_state)
{
	int ret;
	FN_IN;

	/* We dont handle multiple codecs now */
	if (audio_state.hw_init) {
		DPRINTK(" Codec Already registered\n");
		return -EPERM;
	}

	/* Grab the dma Callback */
	audio_dma_callback = audio_get_dma_callback();
	if (!audio_dma_callback) {
		DPRINTK("Unable to get call back function\n");
		return -EPERM;
	}

	/* Sanity checks */
	if (!codec_state) {
		DPRINTK("NULL ARGUMENT!\n");
		return -EPERM;
	}

	if (!codec_state->hw_probe || !codec_state->hw_init
	    || !codec_state->hw_shutdown || !codec_state->client_ioctl) {
		DPRINTK
			("Required Fn Entry point Missing probe = %p\
				init = %p, down = %p, ioctl = %p !\n",
			 codec_state->hw_probe, codec_state->hw_init,
			 codec_state->hw_shutdown, codec_state->client_ioctl);
		return -EPERM;
	}

	memcpy(&audio_state, codec_state, sizeof(audio_state_t));
	sema_init(&audio_state.sem, 1);

	ret = platform_device_register(&davinci_audio_device);
	if (ret != 0) {
		DPRINTK("Platform dev_register failed =%d\n", ret);
		ret = -ENODEV;
		goto register_out;
	}

	ret = driver_register(&davinci_audio_driver);
	if (ret != 0) {
		DPRINTK("Device Register failed =%d\n", ret);
		ret = -ENODEV;
		platform_device_unregister(&davinci_audio_device);
		goto register_out;
	}

	DPRINTK("audio driver register success\n");

register_out:

	FN_OUT(ret);
	return ret;
}
EXPORT_SYMBOL(audio_register_codec);

/*******************************************************************************
 *
 * audio_unregister_codec(): Un-Register a Codec using this function
 *
 ******************************************************************************/
int audio_unregister_codec(audio_state_t *codec_state)
{
	FN_IN;

	/* We dont handle multiple codecs now */
	if (!audio_state.hw_init) {
		DPRINTK(" No Codec registered\n");
		return -EPERM;
	}
	/* Security check */
	if (audio_state.hw_init != codec_state->hw_init) {
		DPRINTK
			("Attempt to unregister codec which was not registered \
				with us\n");
		return -EPERM;
	}

	driver_unregister(&davinci_audio_driver);
	platform_device_unregister(&davinci_audio_device);

	memset(&audio_state, 0, sizeof(audio_state_t));

	FN_OUT(0);
	return 0;
}
EXPORT_SYMBOL(audio_unregister_codec);

/***************************** MODULES SPECIFIC FUNCTION **********************/

/*******************************************************************************
 *
 * audio_write(): Exposed to write() call
 *
 ******************************************************************************/
static int
audio_write(struct file *file, const char *buffer, size_t count, loff_t *ppos)
{
	const char *buffer0 = buffer;
	audio_state_t *state = file->private_data;
	audio_stream_t *s = state->output_stream;
	int chunksize, ret = 0;
	unsigned long flags;

	DPRINTK("audio_write: count=%d\n", count);
	if (*ppos != file->f_pos) {
		DPRINTK("FPOS not ppos ppos=0x%x fpos =0x%x\n", (u32) * ppos,
			(u32) file->f_pos);
		return -ESPIPE;
	}
	if (s->mapped) {
		DPRINTK("s already mapped\n");
		return -ENXIO;
	}
	if (!s->buffers && audio_setup_buf(s)) {
		DPRINTK("NO MEMORY\n");
		return -ENOMEM;
	}

	while (count > 0) {
		audio_buf_t *b = &s->buffers[s->usr_head];

		/* Wait for a buffer to become free */
		if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			if (!s->wfc.done)
				break;
			else {
				local_irq_save(flags);
				s->wfc.done--;
				local_irq_restore(flags);
			}
		} else {
			ret = -ERESTARTSYS;
			if (wait_for_completion_interruptible(&s->wfc))
				break;
		}

		/* Feed the current buffer */
		chunksize = s->fragsize - b->offset;
		if (chunksize > count)
			chunksize = count;
		DPRINTK("write %d to %d\n", chunksize, s->usr_head);
		if (copy_from_user(b->data + b->offset, buffer, chunksize)) {
			DPRINTK("Audio: CopyFrom User failed \n");
			complete(&s->wfc);
			return -EFAULT;
		}

		buffer += chunksize;
		count -= chunksize;
		b->offset += chunksize;

		if (b->offset < s->fragsize) {
			complete(&s->wfc);
			break;
		}

		/* Update pointers and send current fragment to DMA */
		local_irq_save(flags);
		b->offset = 0;
		if (++s->usr_head >= s->nbfrags)
			s->usr_head = 0;
		/* Add the num of frags pending */
		s->pending_frags++;
		s->active = 1;
		local_irq_restore(flags);
		atomic_set(&s->in_write_path, 1);
		audio_process_dma(s);
		atomic_set(&s->in_write_path, 0);
	}

	if ((buffer - buffer0))
		ret = buffer - buffer0;

	DPRINTK("audio_write: return=%d\n", ret);
	return ret;
}

/*******************************************************************************
 *
 * audio_read(): Exposed as read() function
 *
 ******************************************************************************/
static int
audio_read(struct file *file, char *buffer, size_t count, loff_t *ppos)
{
	char *buffer0 = buffer;
	audio_state_t *state = file->private_data;
	audio_stream_t *s = state->input_stream;
	int chunksize, ret = 0;
	unsigned long flags;

	DPRINTK("audio_read: count=%d\n", count);

	if (*ppos != file->f_pos) {
		DPRINTK("AudioRead - FPOS not ppos ppos=0x%x fpos =0x%x\n",
			(u32) * ppos, (u32) file->f_pos);
		return -ESPIPE;
	}
	if (s->mapped) {
		DPRINTK("AudioRead - s already mapped\n");
		return -ENXIO;
	}

	if (!s->active) {
		if (!s->buffers && audio_setup_buf(s)) {
			DPRINTK("AudioRead - No Memory\n");
			return -ENOMEM;
		}
		audio_prime_rx(state);
	}

	while (count > 0) {
		audio_buf_t *b = &s->buffers[s->usr_head];

		/* Wait for a buffer to become full */
		if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			if (!s->wfc.done)
				break;
			else {
				local_irq_save(flags);
				s->wfc.done--;
				local_irq_restore(flags);
			}
		} else {
			ret = -ERESTARTSYS;
			if (wait_for_completion_interruptible(&s->wfc))
				break;
		}

		/* Grab data from the current buffer */
		chunksize = s->fragsize - b->offset;
		if (chunksize > count)
			chunksize = count;

		DPRINTK("read %d from %d\n", chunksize, s->usr_head);
		if (copy_to_user(buffer, b->data + b->offset, chunksize)) {
			complete(&s->wfc);
			return -EFAULT;
		}
		buffer += chunksize;
		count -= chunksize;
		b->offset += chunksize;
		if (b->offset < s->fragsize) {
			complete(&s->wfc);
			break;
		}

		/* Update pointers and return current fragment to DMA */
		local_irq_save(flags);
		b->offset = 0;
		if (++s->usr_head >= s->nbfrags)
			s->usr_head = 0;

		s->pending_frags++;
		local_irq_restore(flags);
		DPRINTK(KERN_INFO
			"calling audio_process_dma from audio_read\n");
		audio_process_dma(s);
	}

	if ((buffer - buffer0))
		ret = buffer - buffer0;
	DPRINTK("audio_read: return=%d\n", ret);
	return ret;
}

/*******************************************************************************
 *
 * audio_mmap(): Exposed as mmap Function
 * !!WARNING: Still under development
 *
 ******************************************************************************/
static int audio_mmap(struct file *file, struct vm_area_struct *vma)
{
	audio_state_t *state = file->private_data;
	audio_stream_t *s;
	unsigned long size, vma_addr;
	int i, ret = 0;

	FN_IN;
	if (vma->vm_pgoff != 0)
		return -EINVAL;

	if (vma->vm_flags & VM_WRITE) {
		if (!state->wr_ref)
			return -EINVAL;
		s = state->output_stream;
	} else if (vma->vm_flags & VM_READ) {
		if (!state->rd_ref)
			return -EINVAL;
		s = state->input_stream;
	} else
		return -EINVAL;

	if (s->mapped)
		return -EINVAL;
	size = vma->vm_end - vma->vm_start;
	if (size != s->fragsize * s->nbfrags)
		return -EINVAL;
	if (!s->buffers && audio_setup_buf(s))
		return -ENOMEM;
	vma_addr = vma->vm_start;
	for (i = 0; i < s->nbfrags; i++) {
		audio_buf_t *buf = &s->buffers[i];
		if (!buf->master)
			continue;
		/*
		  ret =
		  remap_pfn_range(vma, vma_addr, buf->dma_addr >> PAGE_SHIFT,
		  buf->master, vma->vm_page_prot);
		*/

		if (ret)
			return ret;
		vma_addr += buf->master;
	}
	s->mapped = 1;

	FN_OUT(0);
	return 0;
}

/*******************************************************************************
 *
 * audio_poll(): Exposed as poll function
 *
 ******************************************************************************/
static unsigned int
audio_poll(struct file *file, struct poll_table_struct *wait)
{
	audio_state_t *state = file->private_data;
	audio_stream_t *is = state->input_stream;
	audio_stream_t *os = state->output_stream;
	unsigned int mask = 0;

	DPRINTK("audio_poll(): mode=%s%s\n",
		(file->f_mode & FMODE_READ) ? "r" : "",
		(file->f_mode & FMODE_WRITE) ? "w" : "");

	if (file->f_mode & FMODE_READ) {
		/* Start audio input if not already active */
		if (!is->active) {
			if (!is->buffers && audio_setup_buf(is))
				return -ENOMEM;
			audio_prime_rx(state);
		}
		poll_wait(file, &is->wq, wait);
	}

	if (file->f_mode & FMODE_WRITE) {
		if (!os->buffers && audio_setup_buf(os))
			return -ENOMEM;
		poll_wait(file, &os->wq, wait);
	}

	if (file->f_mode & FMODE_READ)
		if ((is->mapped && is->bytecount > 0) ||
		    (!is->mapped && is->wfc.done > 0))
			mask |= POLLIN | POLLRDNORM;

	if (file->f_mode & FMODE_WRITE)
		if ((os->mapped && os->bytecount > 0) ||
		    (!os->mapped && os->wfc.done > 0))
			mask |= POLLOUT | POLLWRNORM;

	DPRINTK("audio_poll() returned mask of %s%s\n",
		(mask & POLLIN) ? "r" : "", (mask & POLLOUT) ? "w" : "");

	FN_OUT(mask);
	return mask;
}

/*******************************************************************************
 *
 * audio_llseek(): Exposed as lseek() function.
 *
 ******************************************************************************/
static loff_t audio_llseek(struct file *file, loff_t offset, int origin)
{
	FN_IN;
	FN_OUT(0);
	return -ESPIPE;
}

/*******************************************************************************
 *
 * audio_ioctl(): Handles generic ioctls. If there is a request for something
 * this fn cannot handle, its then given to client specific ioctl routine, that
 * will take up platform specific requests
 *
 ******************************************************************************/
static int
audio_ioctl(struct inode *inode, struct file *file, uint cmd, ulong arg)
{
	audio_state_t *state = file->private_data;
	audio_stream_t *os = state->output_stream;
	audio_stream_t *is = state->input_stream;
	long val;

	DPRINTK(__FILE__ " audio_ioctl 0x%08x\n", cmd);

	/* dispatch based on command */
	switch (cmd) {
	case OSS_GETVERSION:
		return put_user(SOUND_VERSION, (int *)arg);

	case SNDCTL_DSP_GETBLKSIZE:
		if (file->f_mode & FMODE_WRITE)
			return put_user(os->fragsize, (int *)arg);
		else
			return put_user(is->fragsize, (int *)arg);

	case SNDCTL_DSP_GETCAPS:
		val = DSP_CAP_REALTIME | DSP_CAP_TRIGGER | DSP_CAP_MMAP;
		if (is && os)
			val |= DSP_CAP_DUPLEX;
		FN_OUT(1);
		return put_user(val, (int *)arg);

	case SNDCTL_DSP_SETFRAGMENT:
		if (get_user(val, (long *)arg)) {
			FN_OUT(2);
			return -EFAULT;
		}
		if (file->f_mode & FMODE_READ) {
			int ret = audio_set_fragments(is, val);
			if (ret < 0) {
				FN_OUT(3);
				return ret;
			}
			ret = put_user(ret, (int *)arg);
			if (ret) {
				FN_OUT(4);
				return ret;
			}
		}
		if (file->f_mode & FMODE_WRITE) {
			int ret = audio_set_fragments(os, val);
			if (ret < 0) {
				FN_OUT(5);
				return ret;
			}
			ret = put_user(ret, (int *)arg);
			if (ret) {
				FN_OUT(6);
				return ret;
			}
		}
		FN_OUT(7);
		return 0;

	case SNDCTL_DSP_SYNC:
		FN_OUT(8);
		return audio_sync(file);

	case SNDCTL_DSP_SETDUPLEX:
		FN_OUT(9);
		return 0;

	case SNDCTL_DSP_POST:
		FN_OUT(10);
		return 0;

	case SNDCTL_DSP_GETTRIGGER:
		val = 0;
		if (file->f_mode & FMODE_READ && is->active && !is->stopped)
			val |= PCM_ENABLE_INPUT;
		if (file->f_mode & FMODE_WRITE && os->active && !os->stopped)
			val |= PCM_ENABLE_OUTPUT;
		FN_OUT(11);
		return put_user(val, (int *)arg);

	case SNDCTL_DSP_SETTRIGGER:
		if (get_user(val, (int *)arg)) {
			FN_OUT(12);
			return -EFAULT;
		}
		if (file->f_mode & FMODE_READ) {
			if (val & PCM_ENABLE_INPUT) {
				unsigned long flags;
				if (!is->active) {
					if (!is->buffers &&
					    audio_setup_buf(is)) {
						FN_OUT(13);
						return -ENOMEM;
					}
					audio_prime_rx(state);
				}
				local_irq_save(flags);
				is->stopped = 0;
				local_irq_restore(flags);
				audio_process_dma(is);

			} else {
				is->stopped = 1;
				audio_stop_dma(is);
			}
		}
		if (file->f_mode & FMODE_WRITE) {
			if (val & PCM_ENABLE_OUTPUT) {
				unsigned long flags;
				if (!os->buffers && audio_setup_buf(os)) {
					FN_OUT(14);
					return -ENOMEM;
				}
				local_irq_save(flags);
				if (os->mapped && !os->pending_frags) {
					os->pending_frags = os->nbfrags;
					init_completion(&os->wfc);
					os->wfc.done = 0;
					os->active = 1;
				}
				os->stopped = 0;
				local_irq_restore(flags);
				audio_process_dma(os);
			} else {
				os->stopped = 1;
				audio_stop_dma(os);
			}
		}
		FN_OUT(15);
		return 0;

	case SNDCTL_DSP_GETOPTR:
	case SNDCTL_DSP_GETIPTR:
		{
			count_info inf = { 0, };
			audio_stream_t *s =
			    (cmd == SNDCTL_DSP_GETOPTR) ? os : is;
			int bytecount, offset;
			unsigned long flags;

			if ((s == is && !(file->f_mode & FMODE_READ)) ||
			    (s == os && !(file->f_mode & FMODE_WRITE))) {
				FN_OUT(16);
				return -EINVAL;
			}
			if (s->active) {
				local_irq_save(flags);
				offset = audio_get_dma_pos(s);
				inf.ptr = s->dma_tail * s->fragsize + offset;
				bytecount = s->bytecount + offset;
				s->bytecount = -offset;
				inf.blocks = s->fragcount;
				s->fragcount = 0;
				local_irq_restore(flags);
				if (bytecount < 0)
					bytecount = 0;
				inf.bytes = bytecount;
			}
			FN_OUT(17);
			return copy_to_user((void *)arg, &inf, sizeof(inf));
		}

	case SNDCTL_DSP_GETOSPACE:
	case SNDCTL_DSP_GETISPACE:
		{
			audio_buf_info inf = { 0, };
			audio_stream_t *s =
			    (cmd == SNDCTL_DSP_GETOSPACE) ? os : is;
			audio_buf_t *b = NULL;

			if ((s == is && !(file->f_mode & FMODE_READ)) ||
			    (s == os && !(file->f_mode & FMODE_WRITE))) {
				FN_OUT(18);
				return -EINVAL;
			}
			if (!s->buffers && audio_setup_buf(s)) {
				FN_OUT(19);
				return -ENOMEM;
			}
			b = &s->buffers[s->usr_head];
			inf.bytes = s->wfc.done * s->fragsize;
			inf.bytes -= b->offset;
			if (inf.bytes < 0)
				inf.bytes = 0;

			inf.fragments = inf.bytes / s->fragsize;
			inf.fragsize = s->fragsize;
			inf.fragstotal = s->nbfrags;
			FN_OUT(20);
			return copy_to_user((void *)arg, &inf, sizeof(inf));
		}

	case SNDCTL_DSP_NONBLOCK:
		file->f_flags |= O_NONBLOCK;
		FN_OUT(21);
		return 0;

	case SNDCTL_DSP_RESET:
		if (file->f_mode & FMODE_READ) {
			audio_reset(is);
			if (state->need_tx_for_rx) {
				unsigned long flags;
				local_irq_save(flags);
				os->spin_idle = 0;
				local_irq_restore(flags);
			}
		}
		if (file->f_mode & FMODE_WRITE) {
			audio_reset(os);
		}
		FN_OUT(22);
		return 0;

	default:
		/*
		 * Let the client of this module handle the
		 * non generic ioctls
		 */
		FN_OUT(23);
		return state->client_ioctl(inode, file, cmd, arg);
	}

	FN_OUT(0);
	return 0;
}

/*******************************************************************************
 *
 * audio_open(): Exposed as open() function
 *
 ******************************************************************************/
static int audio_open(struct inode *inode, struct file *file)
{
	audio_state_t *state = (&audio_state);
	audio_stream_t *os = state->output_stream;
	audio_stream_t *is = state->input_stream;
	int err, need_tx_dma;
	static unsigned char aic33_init_flag;

	FN_IN;

	/* Lock the module */
	if (!try_module_get(THIS_MODULE)) {
		DPRINTK("Failed to get module\n");
		return -ESTALE;
	}
	/* Lock the codec module */
	if (!try_module_get(state->owner)) {
		DPRINTK("Failed to get codec module\n");
		module_put(THIS_MODULE);
		return -ESTALE;
	}

	down(&state->sem);

	/* access control */
	err = -ENODEV;
	if ((file->f_mode & FMODE_WRITE) && !os)
		goto out;
	if ((file->f_mode & FMODE_READ) && !is)
		goto out;
	err = -EBUSY;
	if ((file->f_mode & FMODE_WRITE) && state->wr_ref)
		goto out;
	if ((file->f_mode & FMODE_READ) && state->rd_ref)
		goto out;
	err = -EINVAL;
	if ((file->f_mode & FMODE_READ) && state->need_tx_for_rx && !os)
		goto out;

	/* request DMA channels */
	need_tx_dma = ((file->f_mode & FMODE_WRITE) ||
		       ((file->f_mode & FMODE_READ) && state->need_tx_for_rx));
	if (state->wr_ref || (state->rd_ref && state->need_tx_for_rx))
		need_tx_dma = 0;
	if (need_tx_dma) {
		DPRINTK("DMA REQUEST FOR playback\n");
		DMA_REQUEST(err, os, audio_dma_callback);
		if (err < 0)
			goto out;
	}
	if (file->f_mode & FMODE_READ) {
		DPRINTK("DMA REQUEST FOR record\n");
		DMA_REQUEST(err, is, audio_dma_callback);
		if (err < 0) {
			if (need_tx_dma)
				DMA_FREE(os);
			goto out;
		}
	}

	/* now complete initialisation */
	if (!AUDIO_ACTIVE(state)) {
		if (state->hw_init && !aic33_init_flag) {
			state->hw_init(state->data);
			aic33_init_flag = 0;
		}
	}

	if ((file->f_mode & FMODE_WRITE)) {
		DPRINTK("SETUP FOR PLAYBACK\n");
		state->wr_ref = 1;
		audio_reset(os);
		os->fragsize = AUDIO_FRAGSIZE_DEFAULT;
		os->nbfrags = AUDIO_NBFRAGS_DEFAULT;
		os->mapped = 0;
		init_waitqueue_head(&os->wq);
	}

	if (file->f_mode & FMODE_READ) {
		DPRINTK("SETUP FOR RECORD\n");
		state->rd_ref = 1;
		audio_reset(is);
		is->fragsize = AUDIO_FRAGSIZE_DEFAULT;
		is->nbfrags = AUDIO_NBFRAGS_DEFAULT;
		is->mapped = 0;
		init_waitqueue_head(&is->wq);
	}

	file->private_data = state;
	err = 0;

out:
	up(&state->sem);
	if (err) {
		module_put(state->owner);
		module_put(THIS_MODULE);
	}
	FN_OUT(err);
	return err;
}

/*******************************************************************************
 *
 * audio_release(): Exposed as release function()
 *
 ******************************************************************************/
static int audio_release(struct inode *inode, struct file *file)
{
	audio_state_t *state = file->private_data;
	audio_stream_t *os = state->output_stream;
	audio_stream_t *is = state->input_stream;

	FN_IN;

	down(&state->sem);

	if (file->f_mode & FMODE_READ) {
		audio_discard_buf(is);
		DMA_FREE(is);
		is->dma_spinref = 0;
		if (state->need_tx_for_rx) {
			os->spin_idle = 0;
			if (!state->wr_ref) {
				DMA_FREE(os);
				os->dma_spinref = 0;
			}
		}
		state->rd_ref = 0;
	}

	if (file->f_mode & FMODE_WRITE) {
		audio_sync(file);
		audio_discard_buf(os);
		if (!state->need_tx_for_rx || !state->rd_ref) {
			DMA_FREE(os);
			os->dma_spinref = 0;
		}
		state->wr_ref = 0;
	}

	if (!AUDIO_ACTIVE(state)) {
		if (state->hw_shutdown)
			state->hw_shutdown(state->data);
	}

	up(&state->sem);

	module_put(state->owner);
	module_put(THIS_MODULE);

	FN_OUT(0);
	return 0;
}

MODULE_AUTHOR("Texas Instruments");
MODULE_DESCRIPTION("Common audio handling for DAVINCI processors");
MODULE_LICENSE("GPL");
