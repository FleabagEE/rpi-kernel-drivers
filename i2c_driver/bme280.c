#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/init.h>

#define BME280_I2C_ADDR    0x76
#define BME280_CHIP_ID_REG 0xD0

static struct i2c_client *bme280_client;

static int bme280_probe(struct i2c_client *client)
{
    int chip_id;

    chip_id = i2c_smbus_read_byte_data(client, BME280_CHIP_ID_REG);
    if (chip_id < 0) {
        printk(KERN_ERR "bme280: failed to read chip id, ret = %d\n", chip_id);
        return chip_id;
    }

    printk(KERN_INFO "bme280: chip id = 0x%02x\n", chip_id);
    return 0;
}

static void bme280_remove(struct i2c_client *client)
{
    printk(KERN_INFO "bme280: removed\n");
}

static const struct i2c_device_id bme280_id[] = {
    { "my_bme280", 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, bme280_id);

static struct i2c_driver bme280_driver = {
    .driver = {
        .name = "my_bme280",
    },
    .probe = bme280_probe,
    .remove = bme280_remove,
    .id_table = bme280_id,
};

static int __init bme280_init(void)
{
    struct i2c_adapter *adapter;
    struct i2c_board_info board_info = {
        I2C_BOARD_INFO("my_bme280", BME280_I2C_ADDR)
    };
    int ret;

    adapter = i2c_get_adapter(1);
    if (!adapter) {
        printk(KERN_ERR "bme280: failed to get i2c adapter\n");
        return -ENODEV;
    }

    bme280_client = i2c_new_client_device(adapter, &board_info);
    i2c_put_adapter(adapter);

    if (!bme280_client) {
        printk(KERN_ERR "bme280: failed to create i2c client\n");
        return -ENODEV;
    }

    ret = i2c_add_driver(&bme280_driver);
    if (ret < 0) {
        printk(KERN_ERR "bme280: failed to register driver\n");
        i2c_unregister_device(bme280_client);
        return ret;
    }

    printk(KERN_INFO "bme280: module loaded\n");
    return 0;
}

static void __exit bme280_exit(void)
{
    i2c_del_driver(&bme280_driver);

    if (bme280_client)
        i2c_unregister_device(bme280_client);

    printk(KERN_INFO "bme280: module unloaded\n");
}

module_init(bme280_init);
module_exit(bme280_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tiffany");
MODULE_DESCRIPTION("BME280 I2C chip ID reader");	

