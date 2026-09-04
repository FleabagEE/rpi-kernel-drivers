#include <linux/module.h>     // 所有 kernel module 都要
#include <linux/fs.h>         // file_operations, register_chrdev
#include <linux/uaccess.h>    // copy_to_user, copy_from_user
#include <linux/init.h>       // __init, __exit
#include <linux/ioctl.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/sched.h>

#define MYDEVICE_MAGIC 'M'
#define MYDEVICE_CLEAR   _IO(MYDEVICE_MAGIC, 0)
#define MYDEVICE_GET_LEN _IOR(MYDEVICE_MAGIC, 1, int)

#define DEVICE_NAME "mydevice"
#define BUF_SIZE 256

static int major;                     // kernel 分配給我們的 major number
static char kernel_buffer[BUF_SIZE];  // driver 內部的資料緩衝區
static int buffer_len;                // 目前 buffer 裡有多少有效資料
static DECLARE_WAIT_QUEUE_HEAD(my_wait_queue);

/* ───── open：user 呼叫 open() 時執行 ───── */
static int my_open(struct inode *inode, struct file *file)
{
    printk(KERN_INFO "mydevice: opened\n");
    return 0;   // 0 = 成功
}

/* ───── release：user 呼叫 close() 時執行 ───── */
static int my_release(struct inode *inode, struct file *file)
{
    printk(KERN_INFO "mydevice: closed\n");
    return 0;
}

/* ───── write：user 寫資料進來（user → kernel）───── */
static ssize_t my_write(struct file *file, const char __user *buf,
                        size_t len, loff_t *offset)
{
    if (len > BUF_SIZE)
        len = BUF_SIZE;

    if (copy_from_user(kernel_buffer, buf, len) != 0)
        return -EFAULT;

    buffer_len = len;

    wake_up_interruptible(&my_wait_queue);   // ★ 新增：叫醒在等資料的 process


    printk(KERN_INFO "mydevice: wrote %zu bytes\n", len);

    // 4. 回傳「寫了幾 byte」（user 的 write() 會拿到這個數字）
    return len;
}

/* ───── read：user 讀資料出去（kernel → user）───── */
static ssize_t my_read(struct file *file, char __user *buf,
                       size_t len, loff_t *offset)
{
    // 1. EOF 判斷：如果已經讀到 buffer 尾端，回傳 0（告訴 user「讀完了」）
    if (*offset >= buffer_len)
        return 0;

    // 2. 算這次要讀多少：不能超過「buffer 剩下的量」
    size_t remaining = buffer_len - *offset;
    if (len > remaining)
        len = remaining;

    // 3. 從 kernel buffer 安全地複製到 user space
    //    注意 kernel_buffer + *offset：從「還沒讀的位置」開始
    if (copy_to_user(buf, kernel_buffer + *offset, len) != 0)
        return -EFAULT;

    // 4. 更新 offset：往前推進 len，下次 read 從新位置繼續
    *offset += len;

    printk(KERN_INFO "mydevice: read %zu bytes\n", len);

    // 5. 回傳「讀了幾 byte」
    return len;
}

static long my_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    switch (cmd) {
    case MYDEVICE_CLEAR:
        buffer_len = 0;
        memset(kernel_buffer, 0, BUF_SIZE);
        printk(KERN_INFO "mydevice: cleared\n");
        break;

    case MYDEVICE_GET_LEN:
        if (copy_to_user((int __user *)arg, &buffer_len, sizeof(int)) != 0)
            return -EFAULT;
        break;

    default:
        return -EINVAL;
    }
    return 0;
}
static __poll_t my_poll(struct file *file, struct poll_table_struct *wait)
{
    __poll_t mask = 0;

    // 把這個 process 登記到 wait queue（要等的話會睡在這）
    poll_wait(file, &my_wait_queue, wait);

    // 有資料 → 標記「可讀」
    if (buffer_len > 0)
        mask |= POLLIN | POLLRDNORM;

    // buffer 沒滿 → 標記「可寫」
    if (buffer_len < BUF_SIZE)
        mask |= POLLOUT | POLLWRNORM;

    return mask;
}

/* ───── file_operations：把操作對應到函式 ───── */
static struct file_operations fops = {
    .owner   = THIS_MODULE,
    .open    = my_open,
    .read    = my_read,
    .write   = my_write,
    .release = my_release,
    .unlocked_ioctl = my_ioctl,
    .poll           = my_poll,       // ★ 新增
};

/* ───── module 載入：註冊 character device ───── */
static int __init my_init(void)
{
    // register_chrdev(0, ...) 讓 kernel 自動分配 major number
    // 回傳值就是分配到的 major number
    major = register_chrdev(0, DEVICE_NAME, &fops);
    if (major < 0) {
        printk(KERN_ERR "mydevice: register failed\n");
        return major;   // 註冊失敗，回傳錯誤碼
    }

    printk(KERN_INFO "mydevice: registered, major = %d\n", major);
    return 0;
}

/* ───── module 卸載：註銷 character device ───── */
static void __exit my_exit(void)
{
    unregister_chrdev(major, DEVICE_NAME);
    printk(KERN_INFO "mydevice: unregistered\n");
}

module_init(my_init);
module_exit(my_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tiffany");
MODULE_DESCRIPTION("A simple character driver");
