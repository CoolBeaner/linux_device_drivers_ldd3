/* 
 * Copyright (C) 2022, Jax<coolbeaner@126.com>.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <linux/kdev_t.h>
#include <linux/cdev.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include <asm/atomic.h>
#include <asm/semaphore.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/capability.h>
#include <linux/poll.h>

#ifndef _DEBUG
	#define PDEBUG(fmt, args...) 			\
			do {} while(0)
#else
	#define PDEBUG(fmt, args...) 			\
			printk("[%s:%d]"fmt,		\
			__func__, __LINE__, ##args)	
#endif
/*#define min(x,y) ({ 					\
			typeof(x) _x=(x);		\
			typeof(y) _y=(y);		\
			(void)(&_x == &_y);		\
			_x < _y ? _x : y;})
*/					
#define SCULL_PSIZE	1024

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Jax");

struct scull_pipe {
	wait_queue_head_t inq;				/* 读取队列 */
	wait_queue_head_t outq;				/* 写入队列 */
	char *buffer;					/* 缓冲区的起始 */
	char *end;					/* 缓冲区的结尾 */
	int buffersize;					/* 用于指针计算 */
	char *rp;					/* 读取的位置 */
	char *wp;					/* 写入的位置 */
	int nreaders;					/* 用于读打开的数量 */
	int nwriters;					/* 用于写打开的数量 */
	struct fasync_struct *async_queue;		/* 异步读取者 */
	struct semaphore sem;				/* 互斥信号量 */
	struct cdev cdev;				/* 字符设备结构 */
};

static int scull_minor = 0;
static int scull_major = 0;
static int scull_psize = SCULL_PSIZE;
static int scull_nr_devs = 4;
static dev_t dev = 0;
static struct scull_pipe *scull_pipe = NULL;

module_param(scull_minor, int, S_IRUGO);
module_param(scull_major, int, S_IRUGO);
module_param(scull_psize, int, S_IRUGO);
module_param(scull_nr_devs, int, S_IRUGO);

/* 有多少可写入空间 */
static int spacefree(struct scull_pipe *dev)
{
	if (dev->rp == dev->wp)
		return dev->buffersize - 1;
	return ((dev->rp + dev->buffersize - dev->wp) % dev->buffersize) - 1;
}

/* 等待有可用于写入的空间；调用者必须拥有设备信号量。
 * 在错误情况下，信号量将在返回前释放。
 */
static int scull_getwritespace(struct scull_pipe *dev, struct file *filp)
{
	while (spacefree(dev) == 0) { /* full */
		DEFINE_WAIT(wait); /* 有可能编译不通过 */
		
		up(&dev->sem);
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;
		PDEBUG("\"%s\" writing: going to sleep\n", current->comm);
		prepare_to_wait(&dev->outq, &wait, TASK_INTERRUPTIBLE);
		if (spacefree(dev) == 0)
			schedule();
		finish_wait(&dev->outq, &wait);
		if (signal_pending(current))
			return -ERESTARTSYS; /* 信号：通知 fs 层做相应处理 */
		if (down_interruptible(&dev->sem))
			return -ERESTARTSYS;
	}
	return 0;
}

int scull_p_open(struct inode *inode, struct file *filp)
{
	struct scull_pipe *dev;

	dev = container_of(inode->i_cdev, struct scull_pipe, cdev);
	filp->private_data = dev;

	/*
	if ((filp->f_flags & O_ACCMODE) == O_WRONLY) {
		scull_trim(dev);
	}
	 */
	return 0;
}

ssize_t scull_p_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
	struct scull_pipe *dev = filp->private_data;
	
	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;
	
	while (dev->rp == dev->wp) {	/* 无数据可读取 */
		up(&dev->sem); /* 释放锁 */
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;
		PDEBUG("\"%s\" reading: going to sleep\n", current->comm);
		if (wait_event_interruptible(dev->inq, (dev->rp != dev->wp)))
			return -ERESTARTSYS; /* 信号，通知 fs 层做相应处理 */
		/* 否则循环，但首先获取锁 */
		if (down_interruptible(&dev->sem))
			return -ERESTARTSYS;
	}
	/* 数据已就绪，返回 */
	if (dev->wp > dev->rp)
		count = min(count, (size_t) (dev->wp - dev->rp));
	else /* 写入指针回卷，返回数据直到 dev->end */
		count = min(count, (size_t) (dev->end - dev->rp));
	if (copy_to_user(buf, dev->rp, count)) {
		up(&dev->sem);
		return -EFAULT;
	}
	dev->rp += count;
	if (dev->rp == dev->end)
		dev->rp = dev->buffer; /* 回卷 */
	up(&dev->sem);
	
	/* 最后，唤醒所有写入者并返回 */
	wake_up_interruptible(&dev->outq);
	PDEBUG("\"%s\" did read %li bytes\n", current->comm, (long)count);
	return count;
}

ssize_t scull_p_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
	struct scull_pipe *dev = filp->private_data;
	int result;
	
	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;
	/* 确保有空间可写入 */
	result = scull_getwritespace(dev, filp);
	if (result)
		return result; /* scull_getwritespace 会调用 up(&dev->sem) */
	
	/* 有空间可用，接受数据 */
	count = min(count, (size_t)spacefree(dev));
	if (dev->wp >= dev->rp)
		count = min(count, (size_t)(dev->end - dev->wp)); /* 直到缓冲区结尾 */
	else /* 写入指针回卷，填充到rp-1 */
		count = min(count, (size_t)(dev->rp - dev->wp - 1));
	PDEBUG("Going to accept %li bytes to %p form %p\n", (long)count, dev->wp, buf);
	if (copy_from_user(dev->wp, buf, count)) {
		up(&dev->sem);
		return -EFAULT;
	}
	dev->wp += count;
	if (dev->wp == dev->end)
		dev->wp = dev->buffer; /* 回卷 */
	up(&dev->sem);
	
	/* 最后，唤醒读取者 */
	wake_up_interruptible(&dev->inq); /* 阻塞在read()和select()上 */
	
	/* 通知异步读取者 */
	if (dev->async_queue)
		kill_fasync(&dev->async_queue, SIGIO, POLL_IN);
	PDEBUG("\"%s\" did write %li bytes\n", current->comm, (long)count);
	return count;
}

static unsigned int scull_p_poll(struct file *filp, struct poll_table_struct *wait)
{
		struct scull_pipe *dev = filp->private_data;
		unsigned int mask = 0;
		/*
		 * The buffer is circular; it is considered full
		 * if "wp" is right behind "rp" and empty if the
		 * two are equal.
		 */
		 down(&dev->sem);
		 poll_wait(filp, &dev->inq,  wait);
		 poll_wait(filp, &dev->outq, wait);
		 if (dev->rp != dev->wp)
			 mask |= POLLIN | POLLRDNORM;	/* can be read */
		 if (spacefree(dev))
			 mask |= POLLIN | POLLWRNORM;	/* can be write */
		 up(&dev->sem);
		 
		return mask; 
}

static int scull_p_fasync(int fd, struct file *filp, int mode)
{
	struct scull_pipe *dev = filp->private_data;
	
	return fasync_helper(fd, filp, mode, &dev->async_queue);
}

int scull_p_release(struct inode *inode, struct file *filp)
{
	filp->private_data = NULL;
	scull_p_fasync(-1, filp, 0);
	return 0;
}

struct file_operations scull_pipe_fops = {
	.owner   = THIS_MODULE,
	.open    = scull_p_open,
	//.llseek  = scull_llseek,
	.read    = scull_p_read,
	.write   = scull_p_write,
	.poll    = scull_p_poll,
	.fasync  = scull_p_fasync,
	.release = scull_p_release,
};

int scull_setup_cdev(struct scull_pipe *dev, int index)
{
	int err = 0;
	int devno = 0;

	devno = MKDEV(scull_major, scull_minor + index);
	cdev_init(&dev->cdev, &scull_pipe_fops);
	dev->cdev.owner = THIS_MODULE;
	dev->cdev.ops = &scull_pipe_fops;
	err = cdev_add(&dev->cdev, devno, 1);
	if (err) 
		printk(KERN_NOTICE "Error %d adding scull_pipe%d\n", err, index);
	
	return err;
}

int scull_p_init(struct scull_pipe **dev)
{
	int retval = 0; 

	if (!scull_psize) {
		retval = -EINVAL;
		goto out;
	}
	
	*dev = kmalloc(sizeof(struct scull_pipe), GFP_KERNEL);
	if (!(*dev)) {
		retval = -ENOMEM;
		goto out;
	}	

	init_waitqueue_head(&(*dev)->inq);
	init_waitqueue_head(&(*dev)->outq);
	/* --------------------------------------------------
	 * +	   +	   +	   +	   +	   +	    +
	 * +	   +	   +	   +	   +       + sentry +
	 * +	   +	   +	   +	   +	   +	    +
	 * --------------------------------------------------
	 * exp: buffersize = 5
	 * the last space is sentry, will not be used.
	 */
	(*dev)->buffer = kmalloc(scull_psize + 1, GFP_KERNEL);
	if (!(*dev)->buffer) {
		retval = -ENOMEM;
		goto err0;
	}
	memset((*dev)->buffer, 0, scull_psize + 1);
	(*dev)->end = (*dev)->buffer + scull_psize - 1;
	(*dev)->buffersize = scull_psize;
	(*dev)->rp = (*dev)->buffer;
	(*dev)->wp = (*dev)->buffer;
	(*dev)->nreaders = 0;
	(*dev)->nwriters = 0;
	//(*dev)->async_queue = NULL;			/* need to edit */
	sema_init(&(*dev)->sem, 1);
	retval = scull_setup_cdev((*dev), 0);
	if (retval) 
		goto err1;
	else 
		goto out;
	
err1:
	kfree((*dev)->buffer);
err0:
	kfree(*dev);
out:	
	return retval;
}

static void scull_pipe_del(struct scull_pipe **dev)
{
	//(*dev)->async_queue = NULL;			/* need to edit */
	cdev_del(&(*dev)->cdev);
	kfree((*dev)->buffer);
	kfree(*dev);
	*dev = NULL;
}

static int __init scull_pipe_init(void)
{
	int result = 0;

	printk(KERN_ALERT "Hello World\n");

	if (scull_major) {

		dev = MKDEV(scull_major,scull_minor);
		result = register_chrdev_region(dev, scull_nr_devs, "scull_pipe");
	} else {

		result = alloc_chrdev_region(&dev, scull_minor, scull_nr_devs, \
			"scull_pipe");
		scull_major = MAJOR(dev);
	}

	if (result < 0) {
		printk(KERN_WARNING "scull_pipe: can't get major %d\n", scull_major);
		goto out;
	}
	
	result = scull_p_init(&scull_pipe); 
	if (result) 
		goto err0;
	else
		goto out;

err0:
	unregister_chrdev_region(dev, scull_nr_devs);
out:
	return result;
}

static void __exit scull_pipe_exit(void)
{
	scull_pipe_del(&scull_pipe);
	unregister_chrdev_region(dev, scull_nr_devs);
	printk(KERN_ALERT "Goodbye, Cruel World\n");
}

module_init(scull_pipe_init);
module_exit(scull_pipe_exit);
