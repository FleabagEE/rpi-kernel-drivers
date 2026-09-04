#include <linux/module.h>
#include <linux/netdevice.h>    // net_device, net_device_ops
#include <linux/etherdevice.h>  // alloc_etherdev, eth_type_trans
#include <linux/skbuff.h>       // sk_buff
#include <linux/socket.h>
#include <linux/net.h>
#include <linux/inet.h>
#include <linux/kthread.h>   // kthread_run
#include <linux/delay.h>     // 之後可能會用到

static struct socket *my_sock;
static struct net_device *my_dev;   // 我們的網路裝置
static struct sockaddr_in dest_addr;
static struct socket *recv_sock;
static struct task_struct *recv_thread;

/* ───── 介面啟動：ip link set mynet0 up 時呼叫 ───── */
static int my_open(struct net_device *dev)
{
    printk(KERN_INFO "mynet: device opened\n");
    netif_start_queue(dev);   // 告訴 kernel「我準備好收送封包了」
    return 0;
}

/* ───── 介面關閉：ip link set mynet0 down 時呼叫 ───── */
static int my_stop(struct net_device *dev)
{
    printk(KERN_INFO "mynet: device stopped\n");
    netif_stop_queue(dev);    // 停止收送
    return 0;
}

/* ───── 送封包：★ 核心！有封包要從這介面送出時呼叫 ───── */
static netdev_tx_t my_xmit(struct sk_buff *skb, struct net_device *dev)
{
    struct kvec vec;
    struct msghdr msg;
    int sent;
    // skb 就是要送的封包（包在 sk_buff 裡）
    printk(KERN_INFO "mynet: transmitting packet, len = %u\n", skb->len);

    // 統計：記錄送了幾個封包、幾 bytes
    dev->stats.tx_packets++;
    dev->stats.tx_bytes += skb->len;
    
    /* ★ 這段放在這裡 ★ */
    vec.iov_base = skb->data;
    vec.iov_len = skb->len;

    memset(&msg, 0, sizeof(msg));
    msg.msg_name = &dest_addr;
    msg.msg_namelen = sizeof(dest_addr);

    sent = kernel_sendmsg(my_sock, &msg, &vec, 1, skb->len);
    if (sent < 0) {
        printk(KERN_ERR "mynet: sendmsg failed, ret = %d\n", sent);
    }
    // 這個簡化版：我們不真的送到別的機器，
    // 只是「處理完就釋放」這個 skb（loopback 概念）
    dev_kfree_skb(skb);   // 釋放封包

    return NETDEV_TX_OK;  // 告訴 kernel「送成功了」
}

static int recv_thread_fn(void *data)
{
    struct kvec vec;
    struct msghdr msg;
    unsigned char buffer[1500];
    int len;

    while (!kthread_should_stop()) {
        vec.iov_base = buffer;
        vec.iov_len = sizeof(buffer);

        memset(&msg, 0, sizeof(msg));

        len = kernel_recvmsg(recv_sock, &msg, &vec, 1, sizeof(buffer), 0);

        if (len > 0) {
            struct sk_buff *skb;

            printk(KERN_INFO "mynet: received %d bytes\n", len);

            skb = dev_alloc_skb(len + 2);
            if (!skb) {
                printk(KERN_ERR "mynet: failed to allocate skb\n");
                continue;
            }

            skb_reserve(skb, 2);
            memcpy(skb_put(skb, len), buffer, len);

            skb->dev = my_dev;
            skb->protocol = eth_type_trans(skb, my_dev);

            my_dev->stats.rx_packets++;
            my_dev->stats.rx_bytes += len;

            netif_rx(skb);
        }
    }

    return 0;
}

/* ───── 操作對應表 ───── */
static struct net_device_ops my_netdev_ops = {
    .ndo_open       = my_open,
    .ndo_stop       = my_stop,
    .ndo_start_xmit = my_xmit,
};

/* ───── module 載入 ───── */
static int __init my_init(void)
{
    int ret;
    // 1. 建立一個 ethernet 裝置（alloc_etherdev 幫你設好一堆預設值）
    my_dev = alloc_etherdev(0);
    if (!my_dev)
        return -ENOMEM;

    // 2. 設定裝置名稱和操作表
    strcpy(my_dev->name, "mynet0");        // 介面名稱
    my_dev->netdev_ops = &my_netdev_ops;   // 綁定操作表
    //補充
    my_dev->flags |= IFF_NOARP;
    ret = sock_create_kern(&init_net, AF_INET, SOCK_DGRAM, IPPROTO_UDP, &my_sock);
    if (ret < 0) {
        printk(KERN_ERR "mynet: failed to create socket, ret = %d\n", ret);
        free_netdev(my_dev);
        return ret;
    }
     /* ★ 加在這裡：填入目標位址 ★ */
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(12345);
    in4_pton("192.168.86.23", -1, (u8 *)&dest_addr.sin_addr.s_addr, -1, NULL);

    /* ★ 建立接收 socket，綁定到本機 port 12345 ★ */
struct sockaddr_in recv_addr;

ret = sock_create_kern(&init_net, AF_INET, SOCK_DGRAM, IPPROTO_UDP, &recv_sock);
if (ret < 0) {
    printk(KERN_ERR "mynet: failed to create recv socket, ret = %d\n", ret);
    sock_release(my_sock);
    free_netdev(my_dev);
    return ret;
}

recv_addr.sin_family = AF_INET;
recv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
recv_addr.sin_port = htons(12345);

ret = kernel_bind(recv_sock, (struct sockaddr *)&recv_addr, sizeof(recv_addr));
if (ret < 0) {
    printk(KERN_ERR "mynet: failed to bind recv socket, ret = %d\n", ret);
    sock_release(recv_sock);
    sock_release(my_sock);
    free_netdev(my_dev);
    return ret;
}

recv_thread = kthread_run(recv_thread_fn, NULL, "mynet_recv");
if (IS_ERR(recv_thread)) {
    printk(KERN_ERR "mynet: failed to start recv thread\n");
    sock_release(recv_sock);
    sock_release(my_sock);
    free_netdev(my_dev);
    return PTR_ERR(recv_thread);
}

    // 3. 註冊裝置（系統多一個 mynet0 介面）
    if (register_netdev(my_dev)) {
        free_netdev(my_dev);
        printk(KERN_ERR "mynet: register failed\n");
        return -1;
    }

    printk(KERN_INFO "mynet: registered as mynet0\n");
    return 0;
}

/* ───── module 卸載 ───── */
static void __exit my_exit(void)
{
    if (recv_thread)
        kthread_stop(recv_thread);

    unregister_netdev(my_dev);
    free_netdev(my_dev);

    if (my_sock)
        sock_release(my_sock);

    if (recv_sock)
        sock_release(recv_sock);

    printk(KERN_INFO "mynet: unregistered\n");
}

module_init(my_init);
module_exit(my_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tiffany");
MODULE_DESCRIPTION("A simple network driver");
