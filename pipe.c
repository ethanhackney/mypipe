#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/wait.h>

/* default major number */
#define DEFAULT_MAJOR           (0)

/* default first minor number */
#define DEFAULT_MINOR           (0)

/* default number of pipes devices */
#define DEFAULT_NR_PIPES        (1)

/* default pipe buffer size */
#define DEFAULT_PIPE_SIZE       (4096)

/* pipe context */
struct pipe {
        /* reader queue */
        wait_queue_head_t       p_readq;
        /* writer queue */
        wait_queue_head_t       p_writeq;
        /* protects access to all fields of pipe */
        struct mutex            p_lock;
        /* associated character device */
        struct cdev             p_cdev;
        /* kmalloc()'ed ring buffer */
        char                    *p_rbuf;
        /* next place to read from in p_rbuf */
        int                     p_readp;
        /* next place to write to in p_rbuf */
        int                     p_writep;
        /* number of valid bytes in p_rbuf */
        int                     p_count;
        /* number of bytes in p_rbuf */
        int                     p_size;
        /* number of readers */
        int                     p_nr_readers;
        /* number of writes */
        int                     p_nr_writers;
};

/* number of pipe devices */
static int nr_pipes = DEFAULT_NR_PIPES;
module_param(nr_pipes, int, S_IRUGO);

/* size of pipe buffer size */
static int pipe_size = DEFAULT_PIPE_SIZE;
module_param(pipe_size, int, S_IRUGO);

/* major number */
static int pipe_major = DEFAULT_MAJOR;
module_param(pipe_major, int, S_IRUGO);

/* first minor number */
static int pipe_minor = DEFAULT_MINOR;
module_param(pipe_minor, int, S_IRUGO);

/* array of 'nr_pipes' pipe devices */
static struct pipe *pipes;

/* open a pipe */
static int pipe_open(struct inode *inode, struct file *filp)
{
        struct pipe *pipe;

        pipe = container_of(inode->i_cdev, struct pipe, p_cdev);
        filp->private_data = pipe;

        if (mutex_lock_interruptible(&pipe->p_lock))
                return -ERESTARTSYS;

        /* if ring buffer has not been allocated yet */
        if (!pipe->p_rbuf) {
                pipe->p_rbuf = kzalloc(pipe_size, GFP_KERNEL);
                if (!pipe->p_rbuf) {
                        mutex_unlock(&pipe->p_lock);
                        return -ENOMEM;
                }
        }

        /* initialize ring buffer */
        pipe->p_size = pipe_size;
        pipe->p_readp = 0;
        pipe->p_writep = 0;
        pipe->p_count = 0;

        /* if we are reading, increment readers */
        if (filp->f_mode & FMODE_READ)
                pipe->p_nr_readers++;

        /* if we are writing, increment writers */
        if (filp->f_mode & FMODE_WRITE)
                pipe->p_nr_writers++;

        /* do not allow seeking on pipe */
        mutex_unlock(&pipe->p_lock);
        return nonseekable_open(inode, filp);
}

/* read from a pipe */
static ssize_t pipe_read(struct file *filp,
                         char __user *buf,
                         size_t count,
                         loff_t *f_pos)
{
        struct pipe *pipe = filp->private_data;
        ssize_t ret;
        ssize_t topcount;

        if (mutex_lock_interruptible(&pipe->p_lock))
                return -ERESTARTSYS;

        /* wait for more data */
        while (!pipe->p_count) {
                /* give up our lock before sleeping */
                mutex_unlock(&pipe->p_lock);

                /* return right away for nonblocking readers */
                if (filp->f_flags & O_NONBLOCK)
                        return -EAGAIN;

                /* time to sleep */
                if (wait_event_interruptible(pipe->p_readq, pipe->p_count != 0))
                        return -ERESTARTSYS;

                /* get lock back before checking byte count */
                if (mutex_lock_interruptible(&pipe->p_lock))
                        return -ERESTARTSYS;
        }
        /* okay, we have data to read */

        /* only read as much as we have */
        if (count > pipe->p_count)
                count = pipe->p_count;

        /* easy case, read pointer is less than write pointer */
        if (pipe->p_readp < pipe->p_writep) {
                if (copy_to_user(buf, pipe->p_rbuf + pipe->p_readp, count)) {
                        ret = -EFAULT;
                        goto fail;
                }
                pipe->p_readp += count;
                if (pipe->p_readp == pipe->p_size)
                        pipe->p_readp = 0;
        } else {
                /* hard case, have to copy top half and bottom half of buf */
                topcount = pipe->p_size - pipe->p_readp;

                if (topcount > count)
                        topcount = count;

                /* copy top half */
                if (copy_to_user(buf, pipe->p_rbuf + pipe->p_readp, topcount)) {
                        ret = -EFAULT;
                        goto fail;
                }

                /* if we still have to copy more */
                if (count - topcount) {
                        /* copy bottom half */
                        if (copy_to_user(buf + topcount, pipe->p_rbuf, count - topcount)) {
                                ret = -EFAULT;
                                goto fail;
                        }
                }

                pipe->p_readp = count - topcount;
        }

        /* handle wrap */
        pipe->p_count -= count;
        /* wake up processes waiting for buffer space */
        mutex_unlock(&pipe->p_lock);
        wake_up_interruptible(&pipe->p_writeq);
        return count;
fail:
        mutex_unlock(&pipe->p_lock);
        return ret;
}

/* write to a pipe */
static ssize_t pipe_write(struct file *filp,
                          const char __user *buf,
                          size_t count,
                          loff_t *f_pos)
{
        struct pipe *pipe = filp->private_data;
        ssize_t ret;
        ssize_t topcount;

        if (mutex_lock_interruptible(&pipe->p_lock))
                return -ERESTARTSYS;

        /* wait for buffer space */
        while (pipe->p_count == pipe->p_size) {
                /* give up our lock before sleeping */
                mutex_unlock(&pipe->p_lock);

                /* return right away for nonblocking readers */
                if (filp->f_flags & O_NONBLOCK)
                        return -EAGAIN;

                /* time to sleep */
                if (wait_event_interruptible(pipe->p_readq, pipe->p_count != pipe->p_size))
                        return -ERESTARTSYS;

                /* get lock back before checking byte count */
                if (mutex_lock_interruptible(&pipe->p_lock))
                        return -ERESTARTSYS;
        }
        /* ok, we have space to write to */

        if (count > pipe->p_count)
                count = pipe->p_count;

        if (pipe->p_readp < pipe->p_writep) {
                topcount = pipe->p_size - pipe->p_writep;

                if (topcount > count)
                        topcount = count;

                if (copy_from_user(pipe->p_rbuf + pipe->p_writep,
                                   buf,
                                   topcount)) {
                        ret = -EFAULT;
                        goto fail;
                }

                if (count - topcount) {
                        if (copy_from_user(pipe->p_rbuf,
                                           buf + topcount,
                                           count - topcount)) {
                                ret = -EFAULT;
                                goto fail;
                        }
                }

                pipe->p_writep = count - topcount;
        } else {
                if (copy_from_user(pipe->p_rbuf + pipe->p_writep,
                                   buf,
                                   count)) {
                        ret = -EFAULT;
                        goto fail;
                }
                pipe->p_writep += count;
        }

        pipe->p_count += count;
        mutex_unlock(&pipe->p_lock);
        wake_up_interruptible(&pipe->p_readq);
        return count;
fail:
        mutex_unlock(&pipe->p_lock);
        return ret;
}

/* release a pipe */
static int pipe_release(struct inode *inode, struct file *filp)
{
        struct pipe *pipe = filp->private_data;

        mutex_lock(&pipe->p_lock);

        if (filp->f_mode & FMODE_READ)
                pipe->p_nr_readers--;

        if (filp->f_mode & FMODE_WRITE)
                pipe->p_nr_writers--;

        if (pipe->p_nr_readers + pipe->p_nr_writers == 0) {
                kfree(pipe->p_rbuf);
                pipe->p_rbuf = NULL;
        }

        mutex_unlock(&pipe->p_lock);
        return 0;
}

/* pipe file operations */
static const struct file_operations pipe_fops = {
        .owner          = THIS_MODULE,
        .llseek         = no_llseek,
        .read           = pipe_read,
        .write          = pipe_write,
        .open           = pipe_open,
        .release        = pipe_release,
};

static int __init pipe_mod_init(void)
{
        struct pipe *pipe;
        dev_t dev;
        int ret;
        int i;

        /* get our device region */
        if (pipe_major) {
                /* either statically if user provided device number */
                dev = MKDEV(pipe_major, pipe_minor);
                ret = register_chrdev_region(dev, nr_pipes, "pipemod");
        } else {
                /* or dynamically */
                dev = 0;
                ret = alloc_chrdev_region(&dev, pipe_minor, nr_pipes, "pipemod");
                pipe_major = MAJOR(dev);
        }
        if (ret) {
                printk(KERN_WARNING "pipemod: could not request device region\n");
                return ret;
        }

        /* allocate pipe devices */
        pipes = kzalloc(sizeof(*pipes) * nr_pipes, GFP_KERNEL);
        if (!pipes) {
                printk(KERN_WARNING "pipemod: could not get space for pipes\n");
                dev = MKDEV(pipe_major, pipe_minor);
                unregister_chrdev_region(dev, nr_pipes);
                return -ENOMEM;
        }

        /* initialize pipes */
        for (i = 0; i < nr_pipes; i++) {
                pipe = &pipes[i];

                /* initializer wait queues */
                init_waitqueue_head(&pipe->p_readq);
                init_waitqueue_head(&pipe->p_writeq);

                /* and mutex */
                mutex_init(&pipe->p_lock);

                /* and add our character device or fail gracefully */
                dev = MKDEV(pipe_major, pipe_minor + i);
                cdev_init(&pipe->p_cdev, &pipe_fops);
                pipe->p_cdev.owner = THIS_MODULE;
                ret = cdev_add(&pipe->p_cdev, dev, 1);
                if (ret)
                        printk(KERN_NOTICE "pipemod: could not add pipe%d", i);
        }

        return 0;
}

static void __exit pipe_mod_exit(void)
{
        struct pipe *pipe;
        dev_t dev;
        int i;

        /* free pipe devices */
        for (i = 0; i < nr_pipes; i++) {
                pipe = &pipes[i];

                /* remove our character device */
                cdev_del(&pipe->p_cdev);

                /* free pipe buffer */
                kfree(pipe->p_rbuf);
        }
        kfree(pipes);

        /* give up our device region */
        dev = MKDEV(pipe_major, pipe_minor);
        unregister_chrdev_region(dev, nr_pipes);
}

module_init(pipe_mod_init);
module_exit(pipe_mod_exit);

MODULE_LICENSE("GPL");
