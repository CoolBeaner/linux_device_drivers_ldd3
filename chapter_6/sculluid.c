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
#include <linux/ioctl.h>
#include <linux/capability.h>
#include <linux/sched.h>
  
#define SCULL_IOC_MAGIC 'k'
#define SCULL_IOCRESET _IO(SCULL_IOC_MAGIC, 0)
/*
 * S means "Set" through a ptr,
 * T means "Tell" directly with the argument value
 * G means "Get" reply by setting through a pointer
 * Q means "Query" response is on the return value
 * X means "eXchange" switch G and S atomically
 * H means "sHift" switch T and Q atomically
 */
#define SCULL_IOCSQUANTUM _IOW(SCULL_IOC_MAGIC,  1, int)
#define SCULL_IOCSQSET    _IOW(SCULL_IOC_MAGIC,  2, int)
#define SCULL_IOCTQUANTUM _IO(SCULL_IOC_MAGIC,   3)
#define SCULL_IOCTQSET    _IO(SCULL_IOC_MAGIC,   4)
#define SCULL_IOCGQUANTUM _IOR(SCULL_IOC_MAGIC,  5, int)
#define SCULL_IOCGQSET    _IOR(SCULL_IOC_MAGIC,  6, int)
#define SCULL_IOCQQUANTUM _IO(SCULL_IOC_MAGIC,   7)
#define SCULL_IOCQQSET    _IO(SCULL_IOC_MAGIC,   8)
#define SCULL_IOCXQUANTUM _IOWR(SCULL_IOC_MAGIC, 9, int)
#define SCULL_IOCXQSET	  _IOWR(SCULL_IOC_MAGIC, 10, int)
#define SCULL_IOCHQUANTUM _IO(SCULL_IOC_MAGIC,   11)
#define SCULL_IOCHQSET	  _IO(SCULL_IOC_MAGIC,   12)

#define SCULL_IOC_MAXNR 	14
#define SCULL_QUANTUM  		4096
#define SCULL_QSET		1024  

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Jax");

struct scull_qset {
	void **data;
	struct scull_qset *next;
};

struct scull_dev {
	struct scull_qset *data;
	int quantum;
	int qset;
	unsigned long size;
	//unsigned int access_key;
	struct semaphore sem;
	struct cdev cdev;
};

static int scull_minor = 0;
static int scull_major = 0;
static int scull_quantum = SCULL_QUANTUM;
static int scull_qset = SCULL_QSET;
static int scull_nr_devs = 4;
static dev_t dev = 0;
static struct scull_dev *scull_dev = NULL;
static int scull_u_count = 0;
spinlock_t scull_u_lock = SPIN_LOCK_UNLOCKDE;
uid_t	   scull_u_owner;

module_param(scull_minor, int, S_IRUGO);
module_param(scull_major, int, S_IRUGO);
module_param(scull_quantum, int, S_IRUGO);
module_param(scull_qset, int, S_IRUGO);
module_param(scull_nr_devs, int, S_IRUGO);

int scull_trim(struct scull_dev *dev)
{

	struct scull_qset *next, *dptr;
	int qset = dev->qset;
	int i;
	
	for (dptr = dev->data; dptr; dptr = next) {
		if (dptr->data) {
			for (i = 0; i < qset; i++)
				kfree(dptr->data[i]);
			kfree(dptr->data);
			dptr->data = NULL;
		}
		next = dptr->next;
		kfree(dptr);
	}
	
	dev->size = 0;
	dev->quantum = scull_quantum;
	dev->qset = scull_qset;
	dev->data = NULL;
	
	return 0;
	
}

struct scull_qset* scull_follow(struct scull_dev *dev, int item)
{
	struct scull_qset *dptr = NULL;

	if (!dev)
		goto out;
	
	for (dptr = dev->data; dptr && item > 0; item--) {
		if (!dptr->next) {
			dptr->next = \
				kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
			memset(dptr->next, 0, sizeof(struct scull_qset));
		}
		dptr = dptr->next;
	}
	
out:
	return dptr;
}

int scull_open (struct inode *inode, struct file *filp)
{
	struct scull_dev *dev;

	spin_lock(&scull_u_lock);
	/* uid is the user */
	/* euid is the user who execute "su" */
	/* capable(CAP_DAC_OVERRIDE) is the root user */
	if (scull_u_count &&						\
			(scull_u_owner != current->uid) && 	\
			(scull_u_owner != current->euid) && \
			!capable(CAP_DAC_OVERRIDE)) {			
		spin_unlock(&scull_u_lock);
		return -EBUSY;	/* 返回-EPERM会让用户混淆 */
	}

	if (scull_u_count == 0)
		scull_u_owner = current->uid;	/* 获得所有者 */
	scull_u_count++;
	spin_unlock(&scull_u_lock);
	
	dev = container_of(inode->i_cdev, struct scull_dev, cdev);
	filp->private_data = dev;

	/*
	if ((filp->f_flags & O_ACCMODE) == O_WRONLY) {
		scull_trim(dev);
	}
	 */
	return 0;
}

int scull_release (struct inode *inode, struct file *filp)
{
	spin_lock(&scull_u_lock);
	scull_u_count--; /* 除此之外不做任何事情 */
	spin_unlock(&scull_u_lock);
	filp->private_data = NULL;
	return 0;
}

ssize_t scull_read (struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
	struct scull_dev *dev = filp->private_data;
	struct scull_qset *dptr;
	int quantum = dev->quantum;
	int qset = dev->qset;
	int itemsize = quantum * qset;
	int item, s_pos, q_pos, rest;
	ssize_t retval =  0;

	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;
	if (*f_pos >= dev->size) 
		goto out;
	
	if (*f_pos + count > dev->size)
		count = dev->size - *f_pos;

	item = (long)*f_pos / itemsize;
	rest = (long)*f_pos % itemsize;
	s_pos = rest / quantum;
	q_pos = rest % quantum;

	dptr = scull_follow(dev, item);

	if (dptr == NULL || !dptr->data || !dptr->data[s_pos])
		goto out;

	if (count > quantum - q_pos)
		count = quantum - q_pos;

	if (copy_to_user(buf, dptr->data[s_pos] + q_pos, count)) {
		retval = -EFAULT;
		goto out;
	}

	*f_pos += count;
	retval = count;

out:
	up(&dev->sem);
	return retval;
}

ssize_t scull_write (struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
	struct scull_dev *dev = filp->private_data;
	struct scull_qset *dptr;
	int quantum = dev->quantum;
	int qset = dev->qset;
	int itemsize = quantum * qset;
	int item, s_pos, q_pos, rest;
	ssize_t retval =  -ENOMEM;
	
	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;
	item = (long)*f_pos / itemsize;
	rest = (long)*f_pos % itemsize;
	s_pos = rest / quantum;
	q_pos = rest % quantum;
	dptr = scull_follow(dev, item);
	if (dptr == NULL) 
		goto out;
	if (!dptr->data) {
		dptr->data = kmalloc(qset * sizeof(char *), GFP_KERNEL);
		if (!dptr->data) 
			goto out;
		memset(dptr->data, 0, qset * sizeof(char *));
	}
	if (!dptr->data[s_pos]) {
		dptr->data[s_pos] = kmalloc(quantum, GFP_KERNEL);
		if (!dptr->data[s_pos])
			goto out;
	}
	if (count > quantum - q_pos)
		count = quantum - q_pos;
	if (copy_from_user(dptr->data[s_pos] + q_pos, buf, count)) {
		retval = -EFAULT;
		goto out;
	}
	
	*f_pos += count;
	retval = count;

	if (dev->size < *f_pos)
		dev->size = *f_pos;

	printk(KERN_INFO "Total Size: %lu\n", dev->size);
	
out:
	up(&dev->sem);
	return retval;


}

int scull_ioctl (struct inode *inode, struct file *filp, unsigned int cmd, unsigned long arg)
{
	int err = 0;
	int tmp = 0;
	int retval = 0;
	
	if (_IOC_TYPE(cmd) != SCULL_IOC_MAGIC) 	return -ENOTTY;
	if (_IOC_NR(cmd) > SCULL_IOC_MAXNR) 	return -ENOTTY;
	if (_IOC_DIR(cmd) & _IOC_READ)
		err = !access_ok(VERIFY_WRITE, (void __user *)arg, _IOC_SIZE(cmd));
	else if (_IOC_DIR(cmd) & _IOC_WRITE)
		err = !access_ok(VERIFY_READ, (void __user *)arg, _IOC_SIZE(cmd)); 
	if (err) return -EFAULT;

	switch (cmd) {
		case SCULL_IOCRESET:
			scull_quantum = SCULL_QUANTUM;
			scull_qset = SCULL_QSET;
			break;
			
		case SCULL_IOCSQUANTUM:
			if (! capable(CAP_SYS_ADMIN)) 			
				return -EPERM;
			retval = __get_user(scull_quantum, (int __user *)arg);
			break;
			
		case SCULL_IOCTQUANTUM:
			if (! capable(CAP_SYS_ADMIN)) 			
				return -EPERM;
			scull_quantum = arg;
			break;
			
		case SCULL_IOCGQUANTUM:
			retval = __put_user(scull_quantum, (int __user *)arg);
			break;
		
		case SCULL_IOCQQUANTUM:
			return scull_quantum;
			
		case SCULL_IOCXQUANTUM:
			if (! capable(CAP_SYS_ADMIN)) 			
				return -EPERM;
			tmp = scull_quantum;
			retval = __get_user(scull_quantum, (int __user *)arg);
			if (retval == 0)
				retval = __put_user(tmp, (int __user *)arg);
			break;	
			
		case SCULL_IOCHQUANTUM:
			if (! capable(CAP_SYS_ADMIN)) 			
				return -EPERM;
			tmp = scull_quantum;
			scull_quantum = arg;
			return tmp;
			
		case SCULL_IOCSQSET:
			if (! capable(CAP_SYS_ADMIN)) 			
				return -EPERM;
			retval = __get_user(scull_qset, (int __user *)arg);
			break;
			
		case SCULL_IOCTQSET:
			if (! capable(CAP_SYS_ADMIN)) 			
				return -EPERM;
			scull_qset = arg;
			break;
			
		case SCULL_IOCGQSET:
			retval = __put_user(scull_qset, (int __user *)arg);
			break;
		
		case SCULL_IOCQQSET:
			return scull_qset;
			
		case SCULL_IOCXQSET:
			if (! capable(CAP_SYS_ADMIN)) 			
				return -EPERM;
			tmp = scull_qset;
			retval = __get_user(scull_qset, (int __user *)arg);
			if (retval == 0)
				retval = __put_user(tmp, (int __user *)arg);
			break;	
			
		case SCULL_IOCHQSET:
			if (! capable(CAP_SYS_ADMIN)) 			
				return -EPERM;
			tmp = scull_qset;
			scull_qset = arg;
			return tmp;
			
		default:
			return -ENOTTY;			
	}
	return retval;
}

static loff_t scull_llseek(struct file *filp, loff_t off, int whence)
{
	struct scull_dev *dev = filp->private_data;
	loff_t newpos;
	
	switch(whence) {
		case 0: /* SEEK_SET */
			newpos = off;
			break;
			
		case 1: /* SEEK_CUR */
			newpos = filp->f_pos + off;
			break;
		
		case 2: /* SEEK_END */
			newpos = dev->size + off;
			break;
		
		default:
			return -EINVAL;
	}
	if (newpos < 0) return -EINVAL;
	filp->f_pos = newpos;
	return newpos;
}

struct file_operations scull_fops = {
	.owner   = THIS_MODULE,
	.llseek  = scull_llseek,
	.read    = scull_read,
	.write   = scull_write,
	.ioctl   = scull_ioctl,
	.open    = scull_open,
	.release = scull_release,
};

int scull_setup_cdev(struct scull_dev *dev, int index)
{
	int err = 0;
	int devno = 0;

	devno = MKDEV(scull_major, scull_minor + index);
	cdev_init(&dev->cdev, &scull_fops);
	dev->cdev.owner = THIS_MODULE;
	dev->cdev.ops = &scull_fops;
	err = cdev_add(&dev->cdev, devno, 1);
	if (err) 
		printk(KERN_NOTICE "Error %d adding scull%d\n", err, index);
	
	return err;
}

int scull_dev_init(struct scull_dev **dev)
{
	int retval = 0; 

	*dev = kmalloc(sizeof(struct scull_dev), GFP_KERNEL);
	if (!(*dev)) {
		retval = -ENOMEM;
		goto out;
	}	

	(*dev)->data = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
	if (!(*dev)->data) {
		retval = -ENOMEM;
		goto err0;
	}
	memset((*dev)->data, 0, sizeof(struct scull_qset));
	(*dev)->quantum = scull_quantum;
	(*dev)->qset = scull_qset;
	(*dev)->size = 0;
	//(*dev)->access_key = 0;
	sema_init(&(*dev)->sem, 2);
	retval = scull_setup_cdev((*dev), 0);
	if (retval) 
		goto err1;
	else 
		goto out;
	
err1:
	kfree((*dev)->data);
err0:
	kfree(*dev);
out:	
	return retval;
}

static void scull_dev_del(struct scull_dev **dev)
{
	if ((*dev)->data) 
		scull_trim(*dev);	
	//(*dev)->access_key = 0;
	cdev_del(&(*dev)->cdev);
	kfree(*dev);
	*dev = NULL;
}

static int __init scull_init(void)
{
	int result = 0;

	printk(KERN_ALERT "Hello World\n");

	if (scull_major) {

		dev = MKDEV(scull_major,scull_minor);
		result = register_chrdev_region(dev, scull_nr_devs, "sculluid");
	} else {

		result = alloc_chrdev_region(&dev, scull_minor, scull_nr_devs, \
			"sculluid");
		scull_major = MAJOR(dev);
	}

	if (result < 0) {
		printk(KERN_WARNING "sculluid: can't get major %d\n", scull_major);
		goto out;
	}
	
	result = scull_dev_init(&scull_dev); 
	if (result) 
		goto err0;
	else
		goto out;

err0:
	unregister_chrdev_region(dev, scull_nr_devs);
out:
	return result;
}

static void __exit scull_exit(void)
{
	scull_dev_del(&scull_dev);
	unregister_chrdev_region(dev, scull_nr_devs);
	printk(KERN_ALERT "Goodbye, Cruel World\n");
}

module_init(scull_init);
module_exit(scull_exit);
