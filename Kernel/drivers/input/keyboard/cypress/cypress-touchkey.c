/*
 * Driver for keys on GPIO lines capable of generating interrupts.
 *
 * Copyright 2005 Phil Blundell
 * Copyright (C) 2010 Michael Richter (alias neldar)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>

#include <linux/init.h>
#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/sched.h>
#include <linux/pm.h>
#include <linux/sysctl.h>
#include <linux/proc_fs.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <mach/regs-gpio.h>
#include <plat/gpio-cfg.h>
#include <asm/gpio.h>
#include <linux/miscdevice.h>
#include <asm/uaccess.h>
#include <linux/earlysuspend.h>
#include <asm/io.h>
#ifdef CONFIG_CPU_FREQ
#include <mach/cpu-freq-v210.h>
#endif
/*
Melfas touchkey register
*/
#define KEYCODE_REG 0x00
#define FIRMWARE_VERSION 0x01
#define TOUCHKEY_MODULE_VERSION 0x02
#define TOUCHKEY_ADDRESS	0x20

#define UPDOWN_EVENT_BIT 0x08
#define KEYCODE_BIT 0x07
#define ESD_STATE_BIT 0x10

/* keycode value */
#define RESET_KEY 0x01
#define SWTICH_KEY 0x02
#define OK_KEY 0x03
#define END_KEY 0x04

#define I2C_M_WR 0		/* for i2c */

#define IRQ_TOUCH_INT (IRQ_EINT_GROUP22_BASE + 1)

#define DEVICE_NAME "melfas-touchkey"

#ifdef CONFIG_KEYPAD_CYPRESS_TOUCH_USE_BLN
//todo: update this everytime you modify BacklightNotification
#define BACKLIGHTNOTIFICATION_VERSION 7

#define BACKLIGHT_ON 1
#define BACKLIGHT_OFF 2
#endif

static int touchkey_keycode[5] =
    { NULL, KEY_BACK, KEY_MENU, KEY_ENTER, KEY_END };

//NAGSM_Android_SEL_Kernel_Aakash_20100320

static int melfas_evt_enable_status = 1;
static ssize_t melfas_evt_status_show(struct device *dev, struct device_attribute *attr, char *sysfsbuf)
{	


	return sprintf(sysfsbuf, "%d\n", melfas_evt_enable_status);

}

static ssize_t melfas_evt_status_store(struct device *dev, struct device_attribute *attr,const char *sysfsbuf, size_t size)
{

	sscanf(sysfsbuf, "%d", &melfas_evt_enable_status);
	return size;
}

static DEVICE_ATTR(melfasevtcntrl, S_IRUGO | S_IWUSR, melfas_evt_status_show, melfas_evt_status_store);

//NAGSM_Android_SEL_Kernel_Aakash_20100320


static int touchkey_enable = 0;

struct i2c_touchkey_driver {
	struct i2c_client *client;
	struct input_dev *input_dev;
	struct early_suspend early_suspend;
};
struct i2c_touchkey_driver *touchkey_driver = NULL;
struct work_struct touchkey_work;
struct workqueue_struct *touchkey_wq;

struct work_struct touch_update_work;
struct delayed_work touch_resume_work;

static void __iomem *gpio_pend_mask_mem;
#define 	INT_PEND_BASE	0xE0200A54

static const struct i2c_device_id melfas_touchkey_id[] = {
	{"melfas_touchkey", 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, melfas_touchkey_id);

static void init_hw(void);
static int i2c_touchkey_probe(struct i2c_client *client,
			      const struct i2c_device_id *id);

extern int get_touchkey_firmware(char *version);
static int touchkey_led_status = 0;


#ifdef CONFIG_KEYPAD_CYPRESS_TOUCH_USE_BLN
static bool touchkey_controller_vdd_on = false;
static bool bln_enabled = false;			//indicates if BLN function is enabled/allowed
static bool BLN_blink_enabled = false;				//indicates blink is set
bool BacklightNotification_ongoing= false;		//indicates ongoing LED Notification
EXPORT_SYMBOL(BacklightNotification_ongoing);		//export for mach-aries.c
#endif

struct i2c_driver touchkey_i2c_driver = {
	.driver = {
		   .name = "melfas_touchkey_driver",
		   },
	.id_table = melfas_touchkey_id,
	.probe = i2c_touchkey_probe,
};

static int touchkey_debug_count = 0;
static char touchkey_debug[104];
static int touch_version = 0;
static void set_touchkey_debug(char value)
{
	if (touchkey_debug_count == 100)
		touchkey_debug_count = 0;
	touchkey_debug[touchkey_debug_count] = value;
	touchkey_debug_count++;
}

#ifdef CONFIG_KEYPAD_CYPRESS_TOUCH_USE_BLN
static void touchkey_power_on(void){
    gpio_direction_output(_3_GPIO_TOUCH_EN, 1);
#if !defined(CONFIG_ARIES_NTT)
    gpio_direction_output(_3_GPIO_TOUCH_CE, 1);
#endif
    touchkey_controller_vdd_on = true;
}

static void touchkey_power_off(void){
    touchkey_controller_vdd_on = false;
    gpio_direction_output(_3_GPIO_TOUCH_EN, 0);
#if !defined(CONFIG_ARIES_NTT)
    gpio_direction_output(_3_GPIO_TOUCH_CE, 0);
#endif
}

static void touchkey_power_off_with_i2c(void){
    touchkey_power_off();
    gpio_direction_output(_3_TOUCH_SDA_28V, 0);
    gpio_direction_output(_3_TOUCH_SCL_28V, 0);
}
#endif

static int i2c_touchkey_read(u8 reg, u8 * val, unsigned int len)
{
	int err;
	int retry = 10;
	struct i2c_msg msg[1];

	if ((touchkey_driver == NULL)) {
		printk(KERN_DEBUG "touchkey is not enabled.R\n");
		return -ENODEV;
	}
	while (retry--) {
		msg->addr = touchkey_driver->client->addr;
		msg->flags = I2C_M_RD;
		msg->len = len;
		msg->buf = val;
		err = i2c_transfer(touchkey_driver->client->adapter, msg, 1);
		if (err >= 0) {
			return 0;
		}
		printk(KERN_DEBUG "%s %d i2c transfer error\n", __func__, __LINE__);	/* add by inter.park */
		mdelay(10);
	}
	return err;

}

static int i2c_touchkey_write(u8 * val, unsigned int len)
{
	int err;
	struct i2c_msg msg[1];
	unsigned char data[2];
	int retry = 2;

#ifdef CONFIG_KEYPAD_CYPRESS_TOUCH_USE_BLN
	if (touchkey_driver == NULL) {
#else
	if ((touchkey_driver == NULL) || !(touchkey_enable == 1)) {
#endif
		printk(KERN_DEBUG "touchkey is not enabled.W\n");
		return -ENODEV;
	}

	while (retry--) {
		data[0] = *val;
		msg->addr = touchkey_driver->client->addr;
		msg->flags = I2C_M_WR;
		msg->len = len;
		msg->buf = data;
		err = i2c_transfer(touchkey_driver->client->adapter, msg, 1);
		if (err >= 0)
			return 0;
		printk(KERN_DEBUG "%s %d i2c transfer error\n", __func__,
		       __LINE__);
		mdelay(10);
	}
	return err;
}

extern unsigned int touch_state_val;
extern void TSP_forced_release(void);
void touchkey_work_func(struct work_struct *p)
{
	u8 data[3];
	int ret;
	int retry = 10;

	set_touchkey_debug('a');
	if (!gpio_get_value(_3_GPIO_TOUCH_INT)) {
		#ifdef CONFIG_CPU_FREQ
		set_dvfs_target_level(LEV_800MHZ);
		#endif
		ret = i2c_touchkey_read(KEYCODE_REG, data, 1);
		set_touchkey_debug(data[0]);
		if(melfas_evt_enable_status)		//NAGSM_Android_SEL_Kernel_Aakash_20100320
		{
		if ((data[0] & ESD_STATE_BIT) || (ret != 0)) {
			printk
			    ("ESD_STATE_BIT set or I2C fail: data: %d, retry: %d\n",
			     data[0], retry);
			//releae key 
            input_report_key(touchkey_driver->input_dev,  touchkey_keycode[1], 0);
            input_report_key(touchkey_driver->input_dev,  touchkey_keycode[2], 0);
            input_report_key(touchkey_driver->input_dev,  touchkey_keycode[3], 0);
            input_report_key(touchkey_driver->input_dev,  touchkey_keycode[4], 0);			
			retry = 10;
			while (retry--) {
#ifdef CONFIG_KEYPAD_CYPRESS_TOUCH_USE_BLN
				touchkey_power_off();
#else
				gpio_direction_output(_3_GPIO_TOUCH_EN, 0);
#endif
				mdelay(300);
				init_hw();
				if (i2c_touchkey_read(KEYCODE_REG, data, 3) >=
				    0) {
					printk("%s touchkey init success\n",
					       __func__);
					set_touchkey_debug('O');
					enable_irq(IRQ_TOUCH_INT);
					return;
				}
				printk("%s %d i2c transfer error retry = %d\n",
				       __func__, __LINE__, retry);
			}
			//touchkey die , do not enable touchkey
			//enable_irq(IRQ_TOUCH_INT);
			touchkey_enable = -1;
#ifdef CONFIG_KEYPAD_CYPRESS_TOUCH_USE_BLN
			touchkey_power_off_with_i2c();
#else
			gpio_direction_output(_3_GPIO_TOUCH_EN, 0);
#if !defined(CONFIG_ARIES_NTT)
			gpio_direction_output(_3_GPIO_TOUCH_CE, 0);
#endif
			gpio_direction_output(_3_TOUCH_SDA_28V, 0);
			gpio_direction_output(_3_TOUCH_SCL_28V, 0);
#endif
			printk("%s touchkey died\n", __func__);
			set_touchkey_debug('D');
			return;
		}

		if (data[0] & UPDOWN_EVENT_BIT) {
			input_report_key(touchkey_driver->input_dev,
					 touchkey_keycode[data[0] &
							  KEYCODE_BIT], 0);
			input_sync(touchkey_driver->input_dev);
			//printk(" touchkey release keycode: %d\n", touchkey_keycode[data[0] & KEYCODE_BIT]);
			printk(KERN_DEBUG "touchkey release keycode:%d \n",
			       touchkey_keycode[data[0] & KEYCODE_BIT]);

		} else {
			if (touch_state_val == 1) {
				printk(KERN_DEBUG
				       "touchkey pressed but don't send event because touch is pressed. \n");
				set_touchkey_debug('P');
			} else {
				if ((data[0] & KEYCODE_BIT) == 2) {	// if back key is pressed, release multitouch
					//printk("touchkey release tsp input. \n");
					TSP_forced_release();
				}

				input_report_key(touchkey_driver->input_dev,
						 touchkey_keycode[data[0] &
								  KEYCODE_BIT],
						 1);
				input_sync(touchkey_driver->input_dev);
				//printk(" touchkey press keycode: %d\n", touchkey_keycode[data[0] & KEYCODE_BIT]);
				printk(KERN_DEBUG
				       "touchkey press keycode:%d \n",
				       touchkey_keycode[data[0] & KEYCODE_BIT]);
			}
			}
		}
	}
	//clear interrupt
	if (readl(gpio_pend_mask_mem) & (0x1 << 1))
		writel(readl(gpio_pend_mask_mem) | (0x1 << 1),
		       gpio_pend_mask_mem);

	set_touchkey_debug('A');
	enable_irq(IRQ_TOUCH_INT);
}

#if 0
void touchkey_resume_func(struct work_struct *p)
{
	char data[3];
	printk("---%s---\n", __FUNCTION__);
	get_touchkey_firmware(data);

	//clear interrupt
	if (readl(gpio_pend_mask_mem) & (0x1 << 1))
		writel(readl(gpio_pend_mask_mem) | (0x1 << 1),
		       gpio_pend_mask_mem);

	enable_irq(IRQ_TOUCH_INT);
	touchkey_enable = 1;
	return;
}
#endif

static irqreturn_t touchkey_interrupt(int irq, void *dummy)
{
	set_touchkey_debug('I');
	disable_irq_nosync(IRQ_TOUCH_INT);
	queue_work(touchkey_wq, &touchkey_work);

	return IRQ_HANDLED;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void melfas_touchkey_early_suspend(struct early_suspend *h)
{
	touchkey_enable = 0;
	set_touchkey_debug('S');
	printk(KERN_DEBUG "melfas_touchkey_early_suspend\n");
	if (touchkey_enable < 0) {
		printk("---%s---touchkey_enable: %d\n", __FUNCTION__,
		       touchkey_enable);
		return;
	}

	disable_irq(IRQ_TOUCH_INT);
#ifdef CONFIG_KEYPAD_CYPRESS_TOUCH_USE_BLN
	touchkey_power_off_with_i2c();
#else
	gpio_direction_output(_3_GPIO_TOUCH_EN, 0);
#if !defined(CONFIG_ARIES_NTT)
	gpio_direction_output(_3_GPIO_TOUCH_CE, 0);
#endif
	gpio_direction_output(_3_TOUCH_SDA_28V, 0);
	gpio_direction_output(_3_TOUCH_SCL_28V, 0);
#endif
}

static void melfas_touchkey_early_resume(struct early_suspend *h)
{
	set_touchkey_debug('R');
	printk(KERN_DEBUG "melfas_touchkey_early_resume\n");
	if (touchkey_enable < 0) {
		printk("---%s---touchkey_enable: %d\n", __FUNCTION__,
		       touchkey_enable);
		return;
	}
#ifdef CONFIG_KEYPAD_CYPRESS_TOUCH_USE_BLN
	touchkey_power_on();
#else
	gpio_direction_output(_3_GPIO_TOUCH_EN, 1);
#if !defined(CONFIG_ARIES_NTT)
	gpio_direction_output(_3_GPIO_TOUCH_CE, 1);
#endif
#endif
	msleep(50);

	//clear interrupt
	if (readl(gpio_pend_mask_mem) & (0x1 << 1))
		writel(readl(gpio_pend_mask_mem) | (0x1 << 1),
		       gpio_pend_mask_mem);

	enable_irq(IRQ_TOUCH_INT);
	touchkey_enable = 1;

}
#endif				// End of CONFIG_HAS_EARLYSUSPEND

extern int mcsdl_download_binary_data(void);
static int i2c_touchkey_probe(struct i2c_client *client,
			      const struct i2c_device_id *id)
{
//      struct i2c_touchkey_driver * state;
	struct device *dev = &client->dev;
	struct input_dev *input_dev;
	int err = 0;

	printk("melfas i2c_touchkey_probe\n");

	touchkey_driver =
	    kzalloc(sizeof(struct i2c_touchkey_driver), GFP_KERNEL);
	if (touchkey_driver == NULL) {
		dev_err(dev, "failed to create our state\n");
		return -ENOMEM;
	}

	touchkey_driver->client = client;

	touchkey_driver->client->irq = IRQ_TOUCH_INT;
	strlcpy(touchkey_driver->client->name, "melfas-touchkey",
		I2C_NAME_SIZE);

	// i2c_set_clientdata(client, state);

	input_dev = input_allocate_device();

	if (!input_dev)
		return -ENOMEM;

	touchkey_driver->input_dev = input_dev;

	input_dev->name = DEVICE_NAME;
	input_dev->phys = "melfas-touchkey/input0";
	input_dev->id.bustype = BUS_HOST;

	set_bit(EV_SYN, input_dev->evbit);
	set_bit(EV_KEY, input_dev->evbit);
	set_bit(touchkey_keycode[1], input_dev->keybit);
	set_bit(touchkey_keycode[2], input_dev->keybit);
	set_bit(touchkey_keycode[3], input_dev->keybit);
	set_bit(touchkey_keycode[4], input_dev->keybit);

	err = input_register_device(input_dev);
	if (err) {
		input_free_device(input_dev);
		return err;
	}

	gpio_pend_mask_mem = ioremap(INT_PEND_BASE, 0x10);
//       INIT_DELAYED_WORK(&touch_resume_work, touchkey_resume_func);

#ifdef CONFIG_HAS_EARLYSUSPEND
	touchkey_driver->early_suspend.suspend = melfas_touchkey_early_suspend;
	touchkey_driver->early_suspend.resume = melfas_touchkey_early_resume;
	register_early_suspend(&touchkey_driver->early_suspend);
#endif				/* CONFIG_HAS_EARLYSUSPEND */

	touchkey_enable = 1;

	if (request_irq
	    (IRQ_TOUCH_INT, touchkey_interrupt, IRQF_DISABLED, DEVICE_NAME,
	     NULL)) {
		printk(KERN_ERR "%s Can't allocate irq ..\n", __FUNCTION__);
		return -EBUSY;
	}

	set_touchkey_debug('K');
	return 0;
}

static void init_hw(void)
{
#ifdef CONFIG_KEYPAD_CYPRESS_TOUCH_USE_BLN
	touchkey_power_on();
#else
	gpio_direction_output(_3_GPIO_TOUCH_EN, 1);
#if !defined(CONFIG_ARIES_NTT)
	gpio_direction_output(_3_GPIO_TOUCH_CE, 1);
#endif
#endif
	msleep(200);
	s3c_gpio_setpull(_3_GPIO_TOUCH_INT, S3C_GPIO_PULL_NONE);
	set_irq_type(IRQ_TOUCH_INT, IRQ_TYPE_LEVEL_LOW);
	s3c_gpio_cfgpin(_3_GPIO_TOUCH_INT, _3_GPIO_TOUCH_INT_AF);
}

int touchkey_update_open(struct inode *inode, struct file *filp)
{
	return 0;
}

ssize_t touchkey_update_read(struct file * filp, char *buf, size_t count,
			     loff_t * f_pos)
{
	char data[3] = { 0, };

	get_touchkey_firmware(data);
	put_user(data[1], buf);

	return 1;
}

#if 0
extern int mcsdl_download_binary_file(unsigned char *pData,
				      unsigned short nBinary_length);
ssize_t touchkey_update_write(struct file *filp, const char *buf, size_t count,
			      loff_t * f_pos)
{
	unsigned char *pdata;

	disable_irq(IRQ_TOUCH_INT);
	printk("count = %d\n", count);
	pdata = kzalloc(count, GFP_KERNEL);
	if (pdata == NULL) {
		printk("memory allocate fail \n");
		return 0;
	}
	if (copy_from_user(pdata, buf, count)) {
		printk("copy fail \n");
		kfree(pdata);
		return 0;
	}

	mcsdl_download_binary_file((unsigned char *)pdata,
				   (unsigned short)count);
	kfree(pdata);

	init_hw();
	enable_irq(IRQ_TOUCH_INT);
	return count;
}
#endif

int touchkey_update_release(struct inode *inode, struct file *filp)
{
	return 0;
}

struct file_operations touchkey_update_fops = {
	.owner = THIS_MODULE,
	.read = touchkey_update_read,
//      .write   = touchkey_update_write,
	.open = touchkey_update_open,
	.release = touchkey_update_release,
};

static struct miscdevice touchkey_update_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "melfas_touchkey",
	.fops = &touchkey_update_fops,
};

static ssize_t touch_version_read(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	char data[3] = { 0, };
	int count;

	init_hw();
	if (get_touchkey_firmware(data) != 0)
		i2c_touchkey_read(KEYCODE_REG, data, 3);
	count = sprintf(buf, "0x%x\n", data[1]);

	printk("touch_version_read 0x%x\n", data[1]);
	return count;
}

static ssize_t touch_version_write(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t size)
{
	//buf[size]=0;
	printk("input data --> %s\n", buf);

	return size;
}

extern int ISSP_main();
static int touchkey_update_status = 0;

void touchkey_update_func(struct work_struct *p)
{
	int retry = 10;
	touchkey_update_status = 1;
	printk("%s start\n", __FUNCTION__);
	while (retry--) {
		if (ISSP_main() == 0) {
			touchkey_update_status = 0;
			printk("touchkey_update succeeded\n");
			enable_irq(IRQ_TOUCH_INT);
			return;
		}
	}
	touchkey_update_status = -1;
	printk("touchkey_update failed\n");
	return;
}

static ssize_t touch_update_write(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t size)
{
	printk("touchkey firmware update \n");
	if (*buf == 'S') {
		disable_irq(IRQ_TOUCH_INT);
		INIT_WORK(&touch_update_work, touchkey_update_func);
		queue_work(touchkey_wq, &touch_update_work);
	}
	return size;
}

static ssize_t touch_update_read(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	int count = 0;

	printk("touch_update_read: touchkey_update_status %d\n",
	       touchkey_update_status);

	if (touchkey_update_status == 0) {
		count = sprintf(buf, "PASS\n");
	} else if (touchkey_update_status == 1) {
		count = sprintf(buf, "Downloading\n");
	} else if (touchkey_update_status == -1) {
		count = sprintf(buf, "Fail\n");
	}

	return count;
}

#ifdef CONFIG_KEYPAD_CYPRESS_TOUCH_USE_BLN
static void set_backlight(u8 backlight_status){
    if (touchkey_controller_vdd_on){
	touchkey_led_status = backlight_status;
	i2c_touchkey_write(&backlight_status, 1);
    }
}
#endif

static ssize_t touch_led_control(struct device *dev,
				 struct device_attribute *attr, const char *buf,
				 size_t size)
{
	unsigned char data;
	if (sscanf(buf, "%d\n", &data) == 1) {
		printk(KERN_DEBUG "touch_led_control: %d \n", data);
#ifdef CONFIG_KEYPAD_CYPRESS_TOUCH_USE_BLN
		if ((touchkey_enable == 1)) {
		    if (data == 1)
		    	set_backlight(BACKLIGHT_ON);
		    if (data == 2) {
		    	/* only disable leds if no notification is enabled*/
		    	if (BacklightNotification_ongoing)
		    		set_backlight(BACKLIGHT_ON);
		    	else
		    		set_backlight(BACKLIGHT_OFF);
		    }
		} else
		    printk(KERN_DEBUG "touchkey is not enabled.W\n");
#else
		i2c_touchkey_write(&data, 1);
		touchkey_led_status = data;
#endif
	} else
		printk("touch_led_control Error\n");

	return size;
}

static ssize_t touchkey_enable_disable(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t size)
{
#if 0
	printk("touchkey_enable_disable %c \n", *buf);
	if (*buf == '0') {
		set_touchkey_debug('d');
		disable_irq(IRQ_TOUCH_INT);
#ifdef CONFIG_KEYPAD_CYPRESS_TOUCH_USE_BLN
		touchkey_power_off();
#else
		gpio_direction_output(_3_GPIO_TOUCH_EN, 0);
#if !defined(CONFIG_ARIES_NTT)
		gpio_direction_output(_3_GPIO_TOUCH_CE, 0);
#endif
#endif
		touchkey_enable = -2;
	} else if (*buf == '1') {
		if (touchkey_enable == -2) {
			set_touchkey_debug('e');
#ifdef CONFIG_KEYPAD_CYPRESS_TOUCH_USE_BLN
			touchkey_power_on();
#else
			gpio_direction_output(_3_GPIO_TOUCH_EN, 1);
#if !defined(CONFIG_ARIES_NTT)
			gpio_direction_output(_3_GPIO_TOUCH_CE, 1);
#endif
#endif
			touchkey_enable = 1;
			enable_irq(IRQ_TOUCH_INT);
		}
	} else {
		printk("touchkey_enable_disable: unknown command %c \n", *buf);
	}
#endif
	return size;
}


#ifdef CONFIG_KEYPAD_CYPRESS_TOUCH_USE_BLN

static void enable_led_notification(void){
	if (bln_enabled){
		if (touchkey_enable != 1){
			touchkey_power_on();
			mdelay(100);

			/* enable touchkey vdd in sleep mode */
			BacklightNotification_ongoing = true;

			/* write to i2cbus, enable backlights */
			set_backlight(BACKLIGHT_ON);

			printk(KERN_DEBUG "%s: notification led enabled\n", __FUNCTION__);
		}
		else
			printk(KERN_DEBUG "%s: cannot set notification led, touchkeys are enabled\n",__FUNCTION__);
	}
}

static void disable_led_notification(void){
	printk(KERN_DEBUG "%s: notification led disabled\n", __FUNCTION__);
	/* disable touchkey vdd in sleep mode */
	BacklightNotification_ongoing = false;

	/* disable the blink state */
	BLN_blink_enabled = false;

	if (touchkey_enable != 1){
		/* write to i2cbus, disable backlights */
		set_backlight(BACKLIGHT_OFF);
	}
}

static ssize_t backlightnotification_status_read(struct device *dev, struct device_attribute *attr, char *buf) {
    return sprintf(buf,"%u\n",(bln_enabled ? 1 : 0));
}
static ssize_t backlightnotification_status_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	unsigned int data;
	if(sscanf(buf, "%u\n", &data) == 1) {
		printk(KERN_DEBUG "%s: %u \n", __FUNCTION__, data);
		if(data == 0 || data == 1){

			if(data == 1){
				printk(KERN_DEBUG "%s: backlightnotification function enabled\n", __FUNCTION__);
				bln_enabled = true;
			}

			if(data == 0){
				printk(KERN_DEBUG "%s: backlightnotification function disabled\n", __FUNCTION__);
				bln_enabled = false;
				if (BacklightNotification_ongoing)
					disable_led_notification();
			}
		}
		else
			printk(KERN_DEBUG "%s: wrong input %u\n", __FUNCTION__, data);
	}
	else
		printk("%s: input error\n", __FUNCTION__);

	return size;
}

static ssize_t notification_led_status_read(struct device *dev, struct device_attribute *attr, char *buf) {
	return sprintf(buf,"%u\n", (BacklightNotification_ongoing ? 1 : 0)); //todo: boolean for notification_led
}

static ssize_t notification_led_status_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	unsigned int data;

	if(sscanf(buf, "%u\n", &data) == 1) {
		if(data == 0 || data == 1){
			printk(KERN_DEBUG "%s: %u \n", __FUNCTION__, data);
			if (data == 1)
				enable_led_notification();

			if(data == 0)
				disable_led_notification();

		} else
			printk(KERN_DEBUG "%s: wrong input %u\n", __FUNCTION__, data);
	} else
		printk("%s: input error\n", __FUNCTION__);

	return size;
}

static ssize_t blink_control_read(struct device *dev, struct device_attribute *attr, char *buf) {
	return sprintf(buf,"%u\n", (BLN_blink_enabled ? 1 : 0)); //todo: boolean for notification_led
}

static ssize_t blink_control_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	unsigned int data;

	if(sscanf(buf, "%u\n", &data) == 1) {
		if(data == 0 || data == 1){
			if (BacklightNotification_ongoing){
				printk(KERN_DEBUG "%s: %u \n", __FUNCTION__, data);
				if (data == 1){
					BLN_blink_enabled = true;
					set_backlight(BACKLIGHT_OFF);
				}

				if(data == 0){
					BLN_blink_enabled = false;
					set_backlight(BACKLIGHT_ON);
				}
			}

		} else
			printk(KERN_DEBUG "%s: wrong input %u\n", __FUNCTION__, data);
	} else
		printk("%s: input error\n", __FUNCTION__);

	return size;
}

static ssize_t backlightnotification_version(struct device *dev, struct device_attribute *attr, char *buf) {
	return sprintf(buf, "%u\n", BACKLIGHTNOTIFICATION_VERSION);
}

static DEVICE_ATTR(blink_control, S_IRUGO | S_IWUGO , blink_control_read, blink_control_write);
static DEVICE_ATTR(enabled, S_IRUGO | S_IWUGO , backlightnotification_status_read, backlightnotification_status_write);
static DEVICE_ATTR(notification_led, S_IRUGO | S_IWUGO , notification_led_status_read, notification_led_status_write);
static DEVICE_ATTR(version, S_IRUGO , backlightnotification_version, NULL);

static struct attribute *bln_notification_attributes[] = {
		&dev_attr_blink_control.attr,
		&dev_attr_enabled.attr,
		&dev_attr_notification_led.attr,
		&dev_attr_version.attr,
		NULL
};

static struct attribute_group bln_notification_group = {
		.attrs  = bln_notification_attributes,
};

static struct miscdevice backlightnotification_device = {
		.minor = MISC_DYNAMIC_MINOR,
		.name = "backlightnotification",
};
#endif

static DEVICE_ATTR(touch_version, S_IRUGO | S_IWUSR | S_IWOTH | S_IXOTH,
		   touch_version_read, touch_version_write);
static DEVICE_ATTR(touch_update, S_IRUGO | S_IWUSR | S_IWOTH | S_IXOTH,
		   touch_update_read, touch_update_write);
static DEVICE_ATTR(brightness, S_IRUGO | S_IWUSR | S_IWOTH | S_IXOTH, NULL,
		   touch_led_control);
static DEVICE_ATTR(enable_disable, S_IRUGO | S_IWUSR | S_IWOTH | S_IXOTH, NULL,
		   touchkey_enable_disable);


static int __init touchkey_init(void)
{
	int ret = 0;
	int retry = 10;
	char data[3] = { 0, };
#if 0 
	if(HWREV >= 0xA)
	{
		touchkey_keycode[2] = KEY_ENTER;
	}
#endif 	
#if !defined(CONFIG_ARIES_NTT)
	if (ret = gpio_request(_3_GPIO_TOUCH_CE, "_3_GPIO_TOUCH_CE"))
		printk(KERN_ERR "Failed to request gpio %s:%d\n", __func__, __LINE__);
#endif
	if (ret = gpio_request(_3_GPIO_TOUCH_EN, "_3_GPIO_TOUCH_EN"))
		printk(KERN_ERR "Failed to request gpio %s:%d\n", __func__, __LINE__);
	if (ret = gpio_request(_3_TOUCH_SDA_28V, "_3_TOUCH_SDA_28V"))
		printk(KERN_ERR "Failed to request gpio %s:%d\n", __func__, __LINE__);
	if (ret = gpio_request(_3_TOUCH_SCL_28V, "_3_TOUCH_SCL_28V"))
		printk(KERN_ERR "Failed to request gpio %s:%d\n", __func__, __LINE__);

	ret = misc_register(&touchkey_update_device);
	if (ret) {
		printk("%s misc_register fail\n", __FUNCTION__);
	}

	if (device_create_file
	    (touchkey_update_device.this_device, &dev_attr_touch_version) < 0) {
		printk("%s device_create_file fail dev_attr_touch_version\n",
		       __FUNCTION__);
		pr_err("Failed to create device file(%s)!\n",
		       dev_attr_touch_version.attr.name);
	}

	if (device_create_file
	    (touchkey_update_device.this_device, &dev_attr_touch_update) < 0) {
		printk("%s device_create_file fail dev_attr_touch_update\n",
		       __FUNCTION__);
		pr_err("Failed to create device file(%s)!\n",
		       dev_attr_touch_update.attr.name);
	}

	if (device_create_file
	    (touchkey_update_device.this_device, &dev_attr_brightness) < 0) {
		printk("%s device_create_file fail dev_attr_touch_update\n",
		       __FUNCTION__);
		pr_err("Failed to create device file(%s)!\n",
		       dev_attr_brightness.attr.name);
	}

	if (device_create_file
	    (touchkey_update_device.this_device,
	     &dev_attr_enable_disable) < 0) {
		printk("%s device_create_file fail dev_attr_touch_update\n",
		       __FUNCTION__);
		pr_err("Failed to create device file(%s)!\n",
		       dev_attr_enable_disable.attr.name);
	}
#ifdef CONFIG_KEYPAD_CYPRESS_TOUCH_USE_BLN
	ret = 0;
	ret = misc_register(&backlightnotification_device);
	if (ret) {
		printk("%s misc_register fail\n", __FUNCTION__, backlightnotification_device.name);
	}
	//add the backlightnotification attributes
	//add the backlightnotification attributes
	if (sysfs_create_group(&backlightnotification_device.this_device->kobj, &bln_notification_group) < 0)
	{
		printk("%s sysfs_create_group fail\n", __FUNCTION__);
		pr_err("Failed to create sysfs group for device (%s)!\n", backlightnotification_device.name);
	}
#endif

	//NAGSM_Android_SEL_Kernel_Aakash_20100320

	if (device_create_file(touchkey_update_device.this_device, &dev_attr_melfasevtcntrl) < 0)
	{
		printk("%s device_create_file fail dev_attr_melfasevtcntrl\n",__FUNCTION__);
		pr_err("Failed to create device file(%s)!\n", dev_attr_melfasevtcntrl.attr.name);
	}

	//NAGSM_Android_SEL_Kernel_Aakash_20100320
	touchkey_wq = create_singlethread_workqueue("melfas_touchkey_wq");
	if (!touchkey_wq)
		return -ENOMEM;

	INIT_WORK(&touchkey_work, touchkey_work_func);

	init_hw();

	while (retry--) {
		if (get_touchkey_firmware(data) == 0)	//melfas need delay for multiple read
			break;
	}
	printk("%s F/W version: 0x%x, Module version:0x%x\n", __FUNCTION__,
	       data[1], data[2]);
	touch_version = data[1];
	retry = 3;
#if 0
	//update version "eclair/vendor/samsung/apps/Lcdtest/src/com/sec/android/app/lcdtest/touch_firmware.java"
	if ((data[1] >= 0xa1) && (data[1] < 0xa9)) {
		set_touchkey_debug('U');
		while (retry--) {
			if (ISSP_main() == 0) {
				printk("touchkey_update succeeded\n");
				set_touchkey_debug('C');
				break;
			}
			printk("touchkey_update failed... retry...\n");
			set_touchkey_debug('f');
		}
		if (retry <= 0) {
#ifdef CONFIG_KEYPAD_CYPRESS_TOUCH_USE_BLN
			touchkey_power_off();
#else
			gpio_direction_output(_3_GPIO_TOUCH_EN, 0);
#if !defined(CONFIG_ARIES_NTT)
			gpio_direction_output(_3_GPIO_TOUCH_CE, 0);
#endif
#endif
			msleep(300);
		}
		init_hw();	//after update, re initalize.
	}
#endif
	ret = i2c_add_driver(&touchkey_i2c_driver);

	if (ret) {
		printk
		    ("melfas touch keypad registration failed, module not inserted.ret= %d\n",
		     ret);
	}
	return ret;
}

static void __exit touchkey_exit(void)
{
	printk("%s \n", __FUNCTION__);
	i2c_del_driver(&touchkey_i2c_driver);
	misc_deregister(&touchkey_update_device);
#ifdef CONFIG_KEYPAD_CYPRESS_TOUCH_USE_BLN
	misc_deregister(&backlightnotification_device);
#endif
	if (touchkey_wq)
		destroy_workqueue(touchkey_wq);
#if !defined(CONFIG_ARIES_NTT)
	gpio_free(_3_GPIO_TOUCH_CE);
#endif
	gpio_free(_3_GPIO_TOUCH_EN);
	gpio_free(_3_TOUCH_SDA_28V);
	gpio_free(_3_TOUCH_SCL_28V);
}

late_initcall(touchkey_init);
module_exit(touchkey_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("@@@");
MODULE_DESCRIPTION("melfas touch keypad");
