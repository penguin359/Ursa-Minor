/*
 *	JACK device detection driver.
 *
 *	Copyright (C) 2009 Samsung Electronics, Inc.
 *
 *	Authors:
 *		Uk Kim <w0806.kim@samsung.com>
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; version 2 of the License.
 */

#include <linux/module.h>
#include <linux/sysdev.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/irq.h>
#include <linux/delay.h>
#include <linux/types.h>
#include <linux/input.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/switch.h>
#include <linux/input.h>
#include <linux/timer.h>
#include <linux/wakelock.h>

#include <mach/hardware.h>
#include <mach/gpio.h>
#include <mach/gpio.h>
//#include <plat/regs-gpio.h>
#include <plat/gpio-cfg.h>
#include <plat/irqs.h>
#include <asm/mach-types.h>

#include <mach/sec_jack.h>

#define CONFIG_DEBUG_SEC_JACK
#define SUBJECT "JACK_DRIVER"

#ifdef CONFIG_DEBUG_SEC_JACK
#define SEC_JACKDEV_DBG(format,...)\
	printk ("[ "SUBJECT " (%s,%d) ] " format "\n", __func__, __LINE__, ## __VA_ARGS__);

#else
#define DEBUG_LOG(format,...)
#endif

#define KEYCODE_SENDEND 248

#define DETECTION_CHECK_COUNT	2
#define	DETECTION_CHECK_TIME	get_jiffies_64() + (HZ/10)// 1000ms / 10 = 100ms
#define	SEND_END_ENABLE_TIME	get_jiffies_64() + (HZ*1)// 1000ms * 1 = 1sec

#define SEND_END_CHECK_COUNT	3
#define SEND_END_CHECK_TIME     get_jiffies_64() + (HZ * 2) //2000ms 

#define WAKELOCK_DET_TIMEOUT	HZ * 5 //5 sec
#define DIVIDE_EARMIC_BIAS 1
static struct platform_driver sec_jack_driver;

struct class *jack_class;
EXPORT_SYMBOL(jack_class);
static struct device *jack_selector_fs;				// Sysfs device, this is used for communication with Cal App.
EXPORT_SYMBOL(jack_selector_fs);
extern int s3c_adc_get_adc_data(int channel);

struct sec_jack_info {
	struct sec_jack_port port;
	struct input_dev *input;
};

static struct sec_jack_info *hi;

struct switch_dev switch_jack_detection = {
		.name = "h2w",
};

struct switch_dev switch_dock_detection = {
		.name = "dock",
};

/* To support AT+FCESTEST=1 */
struct switch_dev switch_sendend = {
		.name = "send_end",
};
static struct timer_list send_end_key_event_timer;

static unsigned int current_jack_type_status;
static unsigned int send_end_enable = 0;
static unsigned int send_end_pressed = 0;
static struct wake_lock jack_sendend_wake_lock;
static int recording_status=0;
static int send_end_irq_token=0;

unsigned int get_headset_status(void)
{
	//SEC_JACKDEV_DBG(" headset_status %d", current_jack_type_status);
	return current_jack_type_status;
}


void set_recording_status(int value)
{
	recording_status = value;
}
static int get_recording_status(void)
{
	return recording_status;
}
EXPORT_SYMBOL(get_headset_status);

void set_dock_state(int value)
{
	printk(KERN_INFO "set_dock_state : 0X%x\n", value);
	switch_set_state(&switch_dock_detection, value);
}

static void jack_input_selector(int jack_type_status)
{
	
	switch(jack_type_status)
	{
		case SEC_HEADSET_3_POLE_DEVICE:
		case SEC_HEADSET_4_POLE_DEVICE:	
		{
			gpio_set_value(GPIO_EARPATH_SEL, 1);	//1:headset, 0: TV_OUT	
			break;
		}
		case SEC_TVOUT_DEVICE :
		{
			gpio_set_value(GPIO_EARPATH_SEL, 0);	//1:headset, 0: TV_OUT	
			break;
		}
		case SEC_UNKNOWN_DEVICE:
		{
			printk("unknown jack device attached. User must select jack device type\n");
			break;
		}
		default :
			break;			
	}
}

static int jack_type_detect_change(struct work_struct *ignored)
{
	int adc = 0;
	struct sec_gpio_info   *det_jack = &hi->port.det_jack;
	struct sec_gpio_info   *send_end = &hi->port.send_end;
	int state = gpio_get_value(det_jack->gpio) ^ det_jack->low_active;
	int count_abnormal=0;
	int count_pole=0;
	bool bQuit = false;

	while(!bQuit)
	{
		state = gpio_get_value(det_jack->gpio) ^ det_jack->low_active;

		if(state)
		{
		#if defined(CONFIG_ARIES_NTT)	// to prevent 3/4 pole headset detection error, LBK@100824
			gpio_set_value(GPIO_SUB_MICBIAS_EN, 1);
			msleep(10);
		#endif

			adc = s3c_adc_get_adc_data(SEC_HEADSET_ADC_CHANNEL);
			/*  unstable zone */
#if !defined(CONFIG_ARIES_NTT)
			if(adc > 3250 || adc == 0) //SKULLCAP 3-pole headphone is 0
#else
			if(adc > 3600)
#endif
			{
				

				/* unknown cable or unknown case */
				if(count_abnormal++>100)
				{
					count_abnormal=0;
					bQuit = true;
					printk(KERN_INFO "[ JACK_DRIVER (%s,%d) ] 3 pole headset or TV-out attatched : adc = %d\n", __func__,__LINE__,adc);
					count_pole = 0;
					if(send_end_irq_token==1)
					{
						disable_irq(send_end->eint);
						send_end_irq_token=0;
					}
					current_jack_type_status = SEC_HEADSET_3_POLE_DEVICE;
					if(!get_recording_status())
					{
					#if !defined(CONFIG_ARIES_NTT)
						gpio_set_value(GPIO_MICBIAS_EN, 0);
					#elif defined(CONFIG_ARIES_NTT)
						gpio_set_value(GPIO_SUB_MICBIAS_EN, 0);
					#endif
#ifdef DIVIDE_EARMIC_BIAS
						gpio_set_value(GPIO_EARMICBIAS_EN, 0);//hm83.cho 100806 add earmicbias
#endif// DIVIDE_EARMIC_BIAS
					}

				}

				/* Todo : to prevent unexpected reset bug.
				 * 		  is it msleep bug? need wakelock.
				 */
				wake_lock_timeout(&jack_sendend_wake_lock, WAKELOCK_DET_TIMEOUT);
				msleep(10);

			}
			/* 4 pole zone */
			else if(adc > 2000 || adc >=650 || adc <=675)   //100909 hm83.cho add 4pole adc ( 650<=adc <=675)
			{
				current_jack_type_status = SEC_HEADSET_4_POLE_DEVICE;
				printk(KERN_INFO "[ JACK_DRIVER (%s,%d) ] 4 pole  headset attached : adc = %d\n",__func__,__LINE__, adc);
				bQuit = true;
				if(send_end_irq_token==0)
				{
					enable_irq(send_end->eint);
					send_end_irq_token=1;
				}
#ifdef DIVIDE_EARMIC_BIAS	
				gpio_set_value(GPIO_EARMICBIAS_EN, 1);//hm83.cho 100806 add earmicbias
#endif // DIVIDE_EARMIC_BIAS
			#if !defined(CONFIG_ARIES_NTT)
				gpio_set_value(GPIO_MICBIAS_EN, 0);
			#elif defined(CONFIG_ARIES_NTT)
				gpio_set_value(GPIO_SUB_MICBIAS_EN, 1);
			#endif
//#endif//CONFIG_S5PC111_GA3_BOARD
			}
			/* unstable zone */
			else if(adc > 600)
			{
				SEC_JACKDEV_DBG("invalid adc > 600, adc is %d\n",adc);

				/* unknown cable or unknown case */
				if(count_abnormal++>100)
				{
					count_abnormal=0;
					bQuit = true;
					printk(KERN_INFO "[ JACK_DRIVER (%s,%d) ] 3 pole headset or TV-out attatched : adc = %d\n", __func__,__LINE__,adc);
					count_pole = 0;
					if(send_end_irq_token==1)
					{
						disable_irq(send_end->eint);
						send_end_irq_token=0;
					}
					current_jack_type_status = SEC_HEADSET_3_POLE_DEVICE;
					if(!get_recording_status())
					{
					#if !defined(CONFIG_ARIES_NTT)
						gpio_set_value(GPIO_MICBIAS_EN, 0);
					#elif defined(CONFIG_ARIES_NTT)
						gpio_set_value(GPIO_SUB_MICBIAS_EN, 0);
					#endif	
#ifdef DIVIDE_EARMIC_BIAS	
						gpio_set_value(GPIO_EARMICBIAS_EN, 0);//hm83.cho 100806 add earmicbias
#endif//DIVIDE_EARMIC_BIAS					
					}

				}

				/* Todo : to prevent unexpected reset bug.
				 * 		  is it msleep bug? need wakelock.
				 */
				wake_lock_timeout(&jack_sendend_wake_lock, WAKELOCK_DET_TIMEOUT);
				msleep(10);
			}
			/* 3 pole zone or unstable zone */
			else
			{	
				if(!adc || count_pole == 10)
				{
			
					/* detect 3pole or tv-out cable */
					printk(KERN_INFO "[ JACK_DRIVER (%s,%d) ] 3 pole headset or TV-out attatched : adc = %d\n", __func__,__LINE__,adc);
					count_pole = 0;
					if(send_end_irq_token==1)
					{
						disable_irq(send_end->eint);
						send_end_irq_token=0;
					}
					current_jack_type_status = SEC_HEADSET_3_POLE_DEVICE;
					if(!get_recording_status())
					{
					#if !defined(CONFIG_ARIES_NTT)
						gpio_set_value(GPIO_MICBIAS_EN, 0);
					#elif defined(CONFIG_ARIES_NTT)
						gpio_set_value(GPIO_SUB_MICBIAS_EN, 0);
					#endif
#ifdef DIVIDE_EARMIC_BIAS	
						gpio_set_value(GPIO_EARMICBIAS_EN, 0);//hm83.cho 100806 add earmicbias
#endif//DIVIDE_EARMIC_BIAS
					}
					bQuit=true;
				
				}
				/* If 4 pole is inserted slowly, ADC value should be lower than 250.
			 	* So, check again.
				 */
				else
				{
					++count_pole;
					/* Todo : to prevent unexpected reset bug.
					 * 		  is it msleep bug? need wakelock.
					 */
					wake_lock_timeout(&jack_sendend_wake_lock, WAKELOCK_DET_TIMEOUT);
					msleep(20);

				}

			}

		} /* if(state) */
		else
		{
			bQuit = true;

			gpio_set_value(GPIO_EARPATH_SEL, 1);	
			current_jack_type_status = SEC_JACK_NO_DEVICE;
			if(!get_recording_status())
			{
			#if !defined(CONFIG_ARIES_NTT)
				gpio_set_value(GPIO_MICBIAS_EN, 0);
			#elif defined(CONFIG_ARIES_NTT)
				gpio_set_value(GPIO_SUB_MICBIAS_EN,0);
			#endif
#ifdef DIVIDE_EARMIC_BIAS	
				gpio_set_value(GPIO_EARMICBIAS_EN, 0);//hm83.cho 100806 add earmicbias
#endif//DIVIDE_EARMIC_BIAS
    		}

			SEC_JACKDEV_DBG("JACK dev detached  \n");			

		}
		switch_set_state(&switch_jack_detection, current_jack_type_status);
		jack_input_selector(current_jack_type_status);

	}
			

	return 0;

}


static DECLARE_DELAYED_WORK(detect_jack_type_work, jack_type_detect_change);

static int jack_detect_change(struct work_struct *ignored)
{
	struct sec_gpio_info   *det_jack = &hi->port.det_jack;
	struct sec_gpio_info   *send_end = &hi->port.send_end;
	int state,count=20;

	cancel_delayed_work_sync(&detect_jack_type_work);
	state = gpio_get_value(det_jack->gpio) ^ det_jack->low_active;

	wake_lock_timeout(&jack_sendend_wake_lock, WAKELOCK_DET_TIMEOUT);

	
	/* block send/end key event */
	mod_timer(&send_end_key_event_timer,SEND_END_CHECK_TIME);


	if(state)
	{
#ifdef DIVIDE_EARMIC_BIAS
		gpio_set_value(GPIO_EARMICBIAS_EN, 1);//hm83.cho 100806 add earmicbias
#endif

#if !defined(CONFIG_ARIES_NTT)
		gpio_set_value(GPIO_MICBIAS_EN, 1);
	#elif defined(CONFIG_ARIES_NTT)
		gpio_set_value(GPIO_SUB_MICBIAS_EN, 1);
#endif
//#endif	
		/* check pin state repeatedly */
		while(count--)
		{
			if(state != (gpio_get_value(det_jack->gpio) ^ det_jack->low_active))
			{
				return -1;
				

			}
			msleep(10);
	
		}

		schedule_delayed_work(&detect_jack_type_work,50);
	}
	else if(!state && current_jack_type_status != SEC_JACK_NO_DEVICE)
	{


		if(send_end_irq_token==1)
		{
			disable_irq(send_end->eint);
			send_end_irq_token=0;
		}


		gpio_set_value(GPIO_EARPATH_SEL, 1);	
		current_jack_type_status = SEC_JACK_NO_DEVICE;
        if(!get_recording_status())
        {
	#if !defined(CONFIG_ARIES_NTT)
              gpio_set_value(GPIO_MICBIAS_EN, 0);
	#elif defined(CONFIG_ARIES_NTT)
		gpio_set_value(GPIO_SUB_MICBIAS_EN, 0);
	#endif
#ifdef DIVIDE_EARMIC_BIAS
			gpio_set_value(GPIO_EARMICBIAS_EN, 0);
#endif  //DIVIDE_EARMIC_BIAS
        }
		switch_set_state(&switch_jack_detection, current_jack_type_status);
		SEC_JACKDEV_DBG("JACK dev detached  \n");			

	}
	return 0;
}

static int sendend_switch_change(struct work_struct *ignored)
{

	struct sec_gpio_info   *det_jack = &hi->port.det_jack;
	struct sec_gpio_info   *send_end = &hi->port.send_end;
	int state, headset_state;
	int count=6;
	
	headset_state = gpio_get_value(det_jack->gpio) ^ det_jack->low_active;
	state = gpio_get_value(send_end->gpio) ^ send_end->low_active;
	

	wake_lock_timeout(&jack_sendend_wake_lock,WAKELOCK_DET_TIMEOUT);
		
	/* check pin state repeatedly */
	while(count-- && !send_end_pressed)
	{
		if(state != (gpio_get_value(send_end->gpio) ^ send_end->low_active) || !headset_state || current_jack_type_status == SEC_HEADSET_3_POLE_DEVICE)
		{
			printk(KERN_INFO "[ JACK_DRIVER] (%s,%d) ] SEND/END key is ignored. State is unstable.\n",__func__,__LINE__);
			return -1;
				

		}
		msleep(10);

	}

	input_report_key(hi->input, KEYCODE_SENDEND, state);
	input_sync(hi->input);
	switch_set_state(&switch_sendend,state);
	send_end_pressed = state;
	printk(KERN_INFO "[ JACK_DRIVER (%s,%d) ] Button is %s.\n", __func__,__LINE__, state? "pressed" : "released");

	return 0;
}

static int sendend_timer_work_func(struct work_struct *ignored)
{
	struct sec_gpio_info   *det_jack = &hi->port.det_jack;
 	int headset_state;

	headset_state = gpio_get_value(det_jack->gpio) ^ det_jack->low_active;

	send_end_enable = 1;

	
	if(send_end_pressed && !headset_state)
	{
		input_report_key(hi->input, KEYCODE_SENDEND,0);
		input_sync(hi->input);
		switch_set_state(&switch_sendend,0);
		send_end_pressed = 0;
		SEC_JACKDEV_DBG("Button is %s forcely.\n", "released");
	}


}
static DECLARE_WORK(jack_detect_work, jack_detect_change);
static DECLARE_WORK(sendend_switch_work, sendend_switch_change);
static DECLARE_WORK(sendend_timer_work, sendend_timer_work_func);

//IRQ Handler
static irqreturn_t detect_irq_handler(int irq, void *dev_id)
{
	
	send_end_enable = 0;
	schedule_work(&jack_detect_work);
	return IRQ_HANDLED;
}
 
static void send_end_key_event_timer_handler(unsigned long arg)
{

	schedule_work(&sendend_timer_work);
	
}

static irqreturn_t send_end_irq_handler(int irq, void *dev_id)
{
   struct sec_gpio_info   *det_jack = &hi->port.det_jack;
   int headset_state;

	headset_state = gpio_get_value(det_jack->gpio) ^ det_jack->low_active;

	if (send_end_enable && headset_state)
	{
		schedule_work(&sendend_switch_work);		
	}
		  
	return IRQ_HANDLED;
}

static ssize_t select_jack_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	printk(KERN_INFO "[JACK] %s : operate nothing\n", __FUNCTION__);

	return 0;
}
static ssize_t select_jack_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	int value = 0;
	struct sec_gpio_info   *det_jack = &hi->port.det_jack;
	struct sec_gpio_info   *send_end = &hi->port.send_end;
	int state = gpio_get_value(det_jack->gpio) ^ det_jack->low_active;	

	
	sscanf(buf, "%d", &value);
	printk(KERN_INFO "[JACK_DRIVER] User  selection : 0X%x", value);
		
	switch(value)
	{
		case SEC_HEADSET_3_POLE_DEVICE:
		{
			if(send_end_irq_token==1)
			{
				disable_irq(send_end->eint);
				send_end_irq_token=0;
			}
			if(!get_recording_status())
			{
			#if !defined(CONFIG_ARIES_NTT)
				gpio_set_value(GPIO_MICBIAS_EN, 0);
			#elif defined(CONFIG_ARIES_NTT)
				gpio_set_value(GPIO_SUB_MICBIAS_EN, 0);
			#endif
#ifdef DIVIDE_EARMIC_BIAS	
				gpio_set_value(GPIO_EARMICBIAS_EN, 0);//hm83.cho 100806 add earmicbias
#endif//DIVIDE_EARMIC_BIAS
			}
			current_jack_type_status = SEC_HEADSET_3_POLE_DEVICE;			
			printk(KERN_INFO "[ JACK_DRIVER (%s,%d) ] 3 pole headset or TV-out attatched : \n", __func__,__LINE__);
			break;
		}
		case SEC_HEADSET_4_POLE_DEVICE:
		{
#ifdef DIVIDE_EARMIC_BIAS	
			gpio_set_value(GPIO_EARMICBIAS_EN, 1);//hm83.cho 100806 add earmicbias
#endif //DIVIDE_EARMIC_BIAS
		#if !defined(CONFIG_ARIES_NTT)
			gpio_set_value(GPIO_MICBIAS_EN, 0);
		#elif defined(CONFIG_ARIES_NTT)
			gpio_set_value(GPIO_SUB_MICBIAS_EN, 1);
		#endif
//#endif
			current_jack_type_status = SEC_HEADSET_4_POLE_DEVICE;
			printk(KERN_INFO "[ JACK_DRIVER (%s,%d) ] 4 pole  headset attached : \n",__func__,__LINE__);
			if(send_end_irq_token==0)
			{
				enable_irq(send_end->eint);
				send_end_irq_token=1;
			}
			break;
		}
		case SEC_JACK_NO_DEVICE:
		{
			if(send_end_irq_token==1)
			{
				disable_irq(send_end->eint);
				send_end_irq_token=0;
			}
			current_jack_type_status = SEC_JACK_NO_DEVICE;

			if(!get_recording_status())
			{
			#if !defined(CONFIG_ARIES_NTT)
				gpio_set_value(GPIO_MICBIAS_EN,0);
			#elif defined(CONFIG_ARIES_NTT)
				gpio_set_value(GPIO_SUB_MICBIAS_EN, 0);
			#endif
#ifdef DIVIDE_EARMIC_BIAS	
				gpio_set_value(GPIO_EARMICBIAS_EN, 0);//hm83.cho 100806 add earmicbias
#endif//DIVIDE_EARMIC_BIAS
			}
			printk(KERN_INFO "[ JACK_DRIVER (%s,%d) ] JACK dev detached  \n",__func__,__LINE__);			
			break;

		}
		default:
			break;
	}

	switch_set_state(&switch_jack_detection, current_jack_type_status);
	jack_input_selector(current_jack_type_status);

	return size;
}
static DEVICE_ATTR(select_jack, S_IRUGO | S_IWUSR | S_IWOTH | S_IXOTH, select_jack_show, select_jack_store);
static int sec_jack_probe(struct platform_device *pdev)
{
	int ret;
	struct sec_jack_platform_data *pdata = pdev->dev.platform_data;
	struct sec_gpio_info   *det_jack;
	struct sec_gpio_info   *send_end;
	struct input_dev	   *input;
	current_jack_type_status = SEC_JACK_NO_DEVICE;
	
	printk(KERN_INFO "SEC JACK: Registering jack driver\n");
	
	hi = kzalloc(sizeof(struct sec_jack_info), GFP_KERNEL);
	if (!hi)
		return -ENOMEM;

	memcpy (&hi->port, pdata->port, sizeof(struct sec_jack_port));

	input = hi->input = input_allocate_device();
	if (!input)
	{
		ret = -ENOMEM;
		printk(KERN_ERR "SEC JACK: Failed to allocate input device.\n");
		goto err_request_input_dev;
	}

	input->name = "sec_jack";
	set_bit(EV_SYN, input->evbit);
	set_bit(EV_KEY, input->evbit);
	set_bit(KEYCODE_SENDEND, input->keybit);

	ret = input_register_device(input);
	if (ret < 0)
	{
		printk(KERN_ERR "SEC JACK: Failed to register driver\n");
		goto err_register_input_dev;
	}
	

	init_timer(&send_end_key_event_timer);
	send_end_key_event_timer.function = send_end_key_event_timer_handler;

	
	ret = switch_dev_register(&switch_jack_detection);
	if (ret < 0) 
	{
		printk(KERN_ERR "SEC JACK: Failed to register switch device\n");
		goto err_switch_dev_register;
	}

	ret = switch_dev_register(&switch_dock_detection);
	if (ret < 0) 
	{
		printk(KERN_ERR "SEC DOCK: Failed to register switch device\n");
		goto err_switch_dev_register;
	}

	ret = switch_dev_register(&switch_sendend);
	if (ret < 0) 
	{
		printk(KERN_ERR "SEC JACK: Failed to register switch device\n");
		goto err_switch_dev_register;
	}
	//Create JACK Device file in Sysfs
	jack_class = class_create(THIS_MODULE, "jack");
	if(IS_ERR(jack_class))
	{
		printk(KERN_ERR "Failed to create class(sec_jack)\n");
	}

	jack_selector_fs = device_create(jack_class, NULL, 0, NULL, "jack_selector");
	if (IS_ERR(jack_selector_fs))
		printk(KERN_ERR "Failed to create device(sec_jack)!= %ld\n", IS_ERR(jack_selector_fs));

	if (device_create_file(jack_selector_fs, &dev_attr_select_jack) < 0)
		printk(KERN_ERR "Failed to create device file(%s)!\n", dev_attr_select_jack.attr.name);	

	//GPIO configuration
	send_end = &hi->port.send_end;
	s3c_gpio_cfgpin(send_end->gpio, S3C_GPIO_SFN(send_end->gpio_af));
	s3c_gpio_setpull(send_end->gpio, S3C_GPIO_PULL_NONE);
	set_irq_type(send_end->eint, IRQ_TYPE_EDGE_BOTH);
       
	ret = request_irq(send_end->eint, send_end_irq_handler, IRQF_DISABLED, "sec_headset_send_end", NULL);


	SEC_JACKDEV_DBG("sended isr send=0X%x, ret =%d", send_end->eint, ret);
	if (ret < 0)
	{
		printk(KERN_ERR "SEC HEADSET: Failed to register send/end interrupt.\n");
		goto err_request_send_end_irq;
	}

	disable_irq(send_end->eint);
	send_end_irq_token=0;

	det_jack = &hi->port.det_jack;
	s3c_gpio_cfgpin(det_jack->gpio, S3C_GPIO_SFN(det_jack->gpio_af));
	s3c_gpio_setpull(det_jack->gpio, S3C_GPIO_PULL_NONE);
	set_irq_type(det_jack->eint, IRQ_TYPE_EDGE_BOTH);

	det_jack->low_active = 0; 

	ret = request_irq(det_jack->eint, detect_irq_handler, IRQF_DISABLED, "sec_headset_detect", NULL);

	SEC_JACKDEV_DBG("det isr det=0X%x, ret =%d", det_jack->eint, ret);
	if (ret < 0) 
	{
		printk(KERN_ERR "SEC HEADSET: Failed to register detect interrupt.\n");
		goto err_request_detect_irq;
	}

	// EAR_SEL
	if(gpio_is_valid(GPIO_EARPATH_SEL))
	{
		if(gpio_request(GPIO_EARPATH_SEL,   "GPJ2"))
			printk(KERN_ERR "Failed to request GPIO_EAR_SEL!\n");
		gpio_direction_output(GPIO_EARPATH_SEL,1);
	}
	s3c_gpio_slp_cfgpin(GPIO_EARPATH_SEL, S3C_GPIO_SLP_PREV);
#if !defined(CONFIG_ARIES_NTT)
	if(gpio_is_valid(GPIO_MICBIAS_EN))
		{
			if(gpio_request(GPIO_MICBIAS_EN, "GPJ4[2]"))
				printk(KERN_ERR "Failed to request GPIO_MICBIAS_EN!\n");
			gpio_direction_output(GPIO_MICBIAS_EN,0);
		}

	s3c_gpio_slp_cfgpin(GPIO_MICBIAS_EN, S3C_GPIO_SLP_PREV); 

#elif defined(CONFIG_ARIES_NTT)// Modify NTTS1
	if(gpio_is_valid(GPIO_MICBIAS_EN))
	{
		if(gpio_request(GPIO_MICBIAS_EN, "GPJ4[2]"))
			printk(KERN_ERR "Failed to request GPIO_MICBIAS_EN!\n");

		gpio_direction_output(GPIO_MICBIAS_EN,0);
	}
	s3c_gpio_slp_cfgpin(GPIO_MICBIAS_EN, S3C_GPIO_SLP_PREV); 

	if(gpio_is_valid(GPIO_SUB_MICBIAS_EN))
	{
		if(gpio_request(GPIO_SUB_MICBIAS_EN, "GPJ2[5]"))
			printk(KERN_ERR "Failed to request GPIO_SUB_MICBIAS_EN!\n");

		gpio_direction_output(GPIO_SUB_MICBIAS_EN,0);
	}
	s3c_gpio_slp_cfgpin(GPIO_SUB_MICBIAS_EN, S3C_GPIO_SLP_PREV); 
#endif

//#ifdef CONFIG_S5PC111_GA3_BOARD	
	if(gpio_is_valid(GPIO_EARMICBIAS_EN))
	{
		if(gpio_request(GPIO_EARMICBIAS_EN, "GPJ4[4]"))
			printk(KERN_ERR "Failed to request GPIO_SUB_MICBIAS_EN!\n");
	}
	s3c_gpio_cfgpin(GPIO_EARMICBIAS_EN, S3C_GPIO_OUTPUT);
	s3c_gpio_slp_cfgpin(GPIO_EARMICBIAS_EN, S3C_GPIO_SLP_PREV);
	gpio_direction_output(GPIO_EARMICBIAS_EN,0);
//#endif
	wake_lock_init(&jack_sendend_wake_lock, WAKE_LOCK_SUSPEND, "sec_jack");

	schedule_work(&jack_detect_work);
	
	return 0;

err_request_send_end_irq:
	free_irq(det_jack->eint, 0);
err_request_detect_irq:
	switch_dev_unregister(&switch_jack_detection);
	switch_dev_unregister(&switch_dock_detection);	
	switch_dev_unregister(&switch_sendend);
err_switch_dev_register:
	input_unregister_device(input);
err_register_input_dev:
	input_free_device(input);
err_request_input_dev:
	kfree (hi);

	return ret;	
}

static int sec_jack_remove(struct platform_device *pdev)
{
	SEC_JACKDEV_DBG("");
	input_unregister_device(hi->input);
	free_irq(hi->port.det_jack.eint, 0);
	free_irq(hi->port.send_end.eint, 0);
	switch_dev_unregister(&switch_jack_detection);
	switch_dev_unregister(&switch_sendend);
	return 0;
}

#ifdef CONFIG_PM
static int sec_jack_suspend(struct platform_device *pdev, pm_message_t state)
{
	if(current_jack_type_status == SEC_JACK_NO_DEVICE || current_jack_type_status == SEC_HEADSET_3_POLE_DEVICE)// ||current_jack_type_status == SEC_HEADSET_4_POLE_DEVICE )
	{
        if(!get_recording_status())
        {
		#if !defined(CONFIG_ARIES_NTT)
			gpio_set_value(GPIO_MICBIAS_EN, 0);
		#elif defined(CONFIG_ARIES_NTT)
			gpio_set_value(GPIO_SUB_MICBIAS_EN, 0);
		#endif
//#ifdef CONFIG_S5PC111_GA3_BOARD	
			gpio_set_value(GPIO_EARMICBIAS_EN, 0);//hm83.cho 100806 add earmicbias
//#endif//CONFIG_S5PC111_GA3_BOARD
        }

	}
	return 0;
}
static int sec_jack_resume(struct platform_device *pdev)
{
	return 0;
}
#else
#define s3c_headset_resume	NULL
#define s3c_headset_suspend	NULL
#endif

static int __init sec_jack_init(void)
{
	SEC_JACKDEV_DBG("");
	return platform_driver_register(&sec_jack_driver);
}

static void __exit sec_jack_exit(void)
{
	platform_driver_unregister(&sec_jack_driver);
}

static struct platform_driver sec_jack_driver = {
	.probe		= sec_jack_probe,
	.remove		= sec_jack_remove,
	.suspend	= sec_jack_suspend,
	.resume		= sec_jack_resume,
	.driver		= {
		.name		= "sec_jack",
		.owner		= THIS_MODULE,
	},
};

module_init(sec_jack_init);
module_exit(sec_jack_exit);

MODULE_AUTHOR("Uk Kim <w0806.kim@samsung.com>");
MODULE_DESCRIPTION("SEC JACK detection driver");
MODULE_LICENSE("GPL");
