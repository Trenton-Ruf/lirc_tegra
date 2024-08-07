/*
 * lirc_tegra.c
 *
 * lirc_tegra - Device driver that records pulse- and pause-lengths
 *	      (space-lengths) (just like the lirc_serial driver does)
 *	      between GPIO interrupt events on the Jetson Nano.
 *	      Lots of code has been taken from the lirc_serial module,
 *	      so I would like say thanks to the authors.
 *
 * Copyright (C) 2012,2016 Aron Robert Szabo <aron@reon.hu>,
 *		      Michael Bishop <cleverca22@gmail.com>
 *		      Bengt Martensson <barf@bengt-martensson.de>
 *		      Trenton Ruf <truf@live.com>
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/module.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/time.h>
#include <linux/timex.h>
#include <linux/timekeeping.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/irq.h>
#include <linux/spinlock.h>
#include <media/lirc.h>
#include <media/lirc_dev.h>
#include <linux/gpio.h>
#include <linux/of_platform.h>

#define LIRC_DRIVER_NAME "lirc_tegra"
#define RBUF_LEN 256
#define LIRC_TRANSMITTER_LATENCY 50

#ifndef MAX_UDELAY_MS
#define MAX_UDELAY_US 5000
#else
#define MAX_UDELAY_US (MAX_UDELAY_MS*1000)
#endif

#define LIRC_TEGRA_MAX_TRANSMITTERS 8
#define INVALID -1
#define dprintk(fmt, args...)					\
	do {							\
		if (debug)					\
			printk(KERN_DEBUG LIRC_DRIVER_NAME ": "	\
			       fmt, ## args);			\
	} while (0)

/* Note: These defaults are pretty brutally overwritten by the devtree stuff. */
#define DEFAULT_GPIO_IN_PIN  149 //ioctl 149 is pin 49
#define DEFAULT_GPIO_OUT_PIN 200 //ioctl 200 is pin 31

/* module parameters */

/* set the default GPIO input pin */
static int gpio_in_pin = DEFAULT_GPIO_IN_PIN;
/* set the default pull behaviour for input pin */
//static int gpio_in_pull = BCM2708_PULL_DOWN;
static int gpio_out_pin[LIRC_TEGRA_MAX_TRANSMITTERS] =
	{DEFAULT_GPIO_OUT_PIN, INVALID, INVALID, INVALID,
	INVALID, INVALID, INVALID, INVALID};
/* actual number of configured transmitters */
static int n_transmitters = 1;
/* enable debugging messages */
static bool debug = 0;
/* -1 = auto, 0 = active high, 1 = active low */
static int sense = -1;
/* use softcarrier by default */
static bool softcarrier = 1;
/* 0 = do not invert output, 1 = invert output */
static bool invert = 0;
/* Transmit mask */
unsigned int tx_mask = 0xFFFFFFFF; /* All transmitters selected as default */

struct gpio_chip *gpiochip;
static int irq_num;

/* forward declarations */
static long send_pulse(unsigned long length);
static void send_space(long length);
static void lirc_tegra_exit(void);

static struct platform_device *lirc_tegra_dev;
static struct timeval lasttv = { 0, 0 };
static struct lirc_buffer rbuf;
static spinlock_t lock;

/* initialized/set in init_timing_params() */
static unsigned int freq = 38000;
static unsigned int duty_cycle = 50;
static unsigned long period;
static unsigned long pulse_width;
static unsigned long space_width;

static bool inline transmitter_enabled(int n) {
	return tx_mask & (1 << n);
}

static void safe_udelay(unsigned long usecs)
{
	while (usecs > MAX_UDELAY_US) {
		udelay(MAX_UDELAY_US);
		usecs -= MAX_UDELAY_US;
	}
	udelay(usecs);
}

static unsigned long read_current_us(void)
{
	struct timespec now;
	getnstimeofday(&now);
	return (now.tv_sec * 1000000) + (now.tv_nsec/1000);
}

static int init_timing_params(unsigned int new_duty_cycle,
	unsigned int new_freq)
{
	if (new_freq > 0) {
		if (1000 * 1000000L / new_freq * new_duty_cycle / 100 <=
			LIRC_TRANSMITTER_LATENCY)
			return -EINVAL;
		if (1000 * 1000000L / new_freq * (100 - new_duty_cycle) / 100 <=
			LIRC_TRANSMITTER_LATENCY)
			return -EINVAL;
		duty_cycle = new_duty_cycle;
		freq = new_freq;
		period = 1000 * 1000000L / freq;
		pulse_width = period * duty_cycle / 100;
		space_width = period - pulse_width;
	} else {
		duty_cycle = INVALID;
		freq = 0;
		period = INVALID;
		pulse_width = INVALID;
		space_width = INVALID;
	}
	dprintk("in init_timing_params, freq=%d pulse=%ld, "
		"space=%ld\n", freq, pulse_width, space_width);
	return 0;
}

static long send_pulse_softcarrier(unsigned long length)
{
	int flag;
	unsigned i;
	unsigned long actual, target;
	unsigned long actual_us, initial_us, target_us;

	length *= 1000;

	actual = 0; target = 0; flag = 0;
	actual_us = read_current_us();

	while (actual < length) {
		for (i = 0; i < n_transmitters; i++)
			if (transmitter_enabled(i))
				gpiochip->set(gpiochip, gpio_out_pin[i], ! (flag ^ invert));
		target += flag ? space_width : pulse_width;

		initial_us = actual_us;
		target_us = actual_us + (target - actual) / 1000;
		/*
		 * Note - we've checked in ioctl that the pulse/space
		 * widths are big enough so that d is > 0
		 */
		if  ((int)(target_us - actual_us) > 0)
			udelay(target_us - actual_us);
		actual_us = read_current_us();
		actual += (actual_us - initial_us) * 1000;
		flag = !flag;
	}
	return (actual-length) / 1000;
}

static long send_pulse(unsigned long length)
{
	unsigned int i;

	if (length <= 0)
		return 0;

	if (softcarrier && freq > 0) {
		return send_pulse_softcarrier(length);
	} else {
		for (i = 0; i < n_transmitters; i++)
			if (transmitter_enabled(i))
				gpiochip->set(gpiochip, gpio_out_pin[i], !invert);

		safe_udelay(length);
		return 0;
	}
}

static void send_space(long length)
{
	unsigned i;
	for (i = 0; i < n_transmitters; i++)
		if (transmitter_enabled(i))
			gpiochip->set(gpiochip, gpio_out_pin[i], invert);
	if (length <= 0)
		return;
	safe_udelay(length);
}

static void rbwrite(int l)
{
	if (lirc_buffer_full(&rbuf)) {
		/* no new signals will be accepted */
		dprintk("Buffer overrun\n");
		return;
	}
	lirc_buffer_write(&rbuf, (void *)&l);
}

static void frbwrite(int l)
{
	/* simple noise filter */
	static int pulse, space;
	static unsigned int ptr;

	if (ptr > 0 && (l & PULSE_BIT)) {
		pulse += l & PULSE_MASK;
		if (pulse > 250) {
			rbwrite(space);
			rbwrite(pulse | PULSE_BIT);
			ptr = 0;
			pulse = 0;
		}
		return;
	}
	if (!(l & PULSE_BIT)) {
		if (ptr == 0) {
			if (l > 20000) {
				space = l;
				ptr++;
				return;
			}
		} else {
			if (l > 20000) {
				space += pulse;
				if (space > PULSE_MASK)
					space = PULSE_MASK;
				space += l;
				if (space > PULSE_MASK)
					space = PULSE_MASK;
				pulse = 0;
				return;
			}
			rbwrite(space);
			rbwrite(pulse | PULSE_BIT);
			ptr = 0;
			pulse = 0;
		}
	}
	rbwrite(l);
}

static irqreturn_t irq_handler(int i, void *blah, struct pt_regs *regs)
{
	struct timeval tv;
	long deltv;
	int data;
	int signal;

	/* use the GPIO signal level */
	signal = gpiochip->get(gpiochip, gpio_in_pin);

	if (sense != -1) {
		/* get current time */
		do_gettimeofday(&tv);

		/* calc time since last interrupt in microseconds */
		deltv = tv.tv_sec-lasttv.tv_sec;
		if (tv.tv_sec < lasttv.tv_sec ||
		    (tv.tv_sec == lasttv.tv_sec &&
		     tv.tv_usec < lasttv.tv_usec)) {
			printk(KERN_WARNING LIRC_DRIVER_NAME
			       ": AIEEEE: your clock just jumped backwards\n");
			printk(KERN_WARNING LIRC_DRIVER_NAME
			       ": %d %d %lx %lx %lx %lx\n", signal, sense,
			       tv.tv_sec, lasttv.tv_sec,
			       tv.tv_usec, lasttv.tv_usec);
			data = PULSE_MASK;
		} else if (deltv > 15) {
			data = PULSE_MASK; /* really long time */
			if (!(signal^sense)) {
				/* sanity check */
				printk(KERN_DEBUG LIRC_DRIVER_NAME
				       ": AIEEEE: %d %d %lx %lx %lx %lx\n",
				       signal, sense, tv.tv_sec, lasttv.tv_sec,
				       tv.tv_usec, lasttv.tv_usec);
				/*
				 * detecting pulse while this
				 * MUST be a space!
				 */
				sense = sense ? 0 : 1;
			}
		} else {
			data = (int) (deltv*1000000 +
				      (tv.tv_usec - lasttv.tv_usec));
		}
		frbwrite(signal^sense ? data : (data|PULSE_BIT));
		lasttv = tv;
		wake_up_interruptible(&rbuf.wait_poll);
	}

	return IRQ_HANDLED;
}

static int is_right_chip(struct gpio_chip *chip, void *data)
{
	dprintk("is_right_chip %s %d\n", chip->label, strcmp(data, chip->label));

	if (strcmp(data, chip->label) == 0)
		return 1;
	return 0;
}

static inline int read_bool_property(const struct device_node *np,
				     const char *propname,
				     bool *out_value)
{
	u32 value = 0;
	int err = of_property_read_u32(np, propname, &value);
	if (err == 0)
		*out_value = (value != 0);
	return err;
}

// TODO 
static void read_pin_settings(struct device_node *node)
{
	u32 pin;
	int index;
	int output_index = 0;
	for (index = 0;
	     of_property_read_u32_index(
		     node,
		     "brcm,pins",
		     index,
		     &pin) == 0;
	     index++) {
		u32 function;
		int err;
		err = of_property_read_u32_index(
			node,
			"brcm,function",
			index,
			&function);
		if (err == 0) {
			if (function == 1) /* Output */
				gpio_out_pin[output_index++] = pin;
			else if (function == 0) /* Input */
				gpio_in_pin = pin;
		}
	}
	n_transmitters = output_index;
}

static int init_port(void)
{
	int i, nlow, nhigh;
	struct device_node *node;

	node = lirc_tegra_dev->dev.of_node;

	gpiochip = gpiochip_find("tegra-gpio", is_right_chip);

	if (!gpiochip && node)
		gpiochip = gpiochip_find("tegra-gpio", is_right_chip);

	if (!gpiochip) {
		pr_err(LIRC_DRIVER_NAME ": gpio chip not found!\n");
		return -ENODEV;
	}

	 //TODO Figure out if this does anything other than set the internal pullups
	 // defined in the device tree
	/*	 
	if (node) {
		struct device_node *pins_node;

		pins_node = of_parse_phandle(node, "pinctrl-0", 0);
		if (!pins_node) {
			printk(KERN_ERR LIRC_DRIVER_NAME
			       ": pinctrl settings not found!\n");
			return -EINVAL;
		}

		read_pin_settings(pins_node);

		of_property_read_u32(node, "tegra,sense", &sense);

		read_bool_property(node, "tegra,softcarrier", &softcarrier);

		read_bool_property(node, "tegra,invert", &invert);

		read_bool_property(node, "tegra,debug", &debug);

	} else {
		return -EINVAL;
	}
	*/

	for (i = 0; i < n_transmitters; i++)
		gpiochip->set(gpiochip, gpio_out_pin[i], invert);

	irq_num = gpiochip->to_irq(gpiochip, gpio_in_pin);
	dprintk("to_irq %d\n", irq_num);

	/* if pin is high, then this must be an active low receiver. */
	if (sense == -1) {
		/* wait 1/2 sec for the power supply */
		msleep(500);

		/*
		 * probe 9 times every 0.04s, collect "votes" for
		 * active high/low
		 */
		nlow = 0;
		nhigh = 0;
		for (i = 0; i < 9; i++) {
			if (gpiochip->get(gpiochip, gpio_in_pin))
				nlow++;
			else
				nhigh++;
			msleep(40);
		}
		sense = (nlow >= nhigh ? 1 : 0);
		printk(KERN_INFO LIRC_DRIVER_NAME
		       ": auto-detected active %s receiver on GPIO pin %d\n",
		       sense ? "low" : "high", gpio_in_pin);
	} else {
		printk(KERN_INFO LIRC_DRIVER_NAME
		       ": manually using active %s receiver on GPIO pin %d\n",
		       sense ? "low" : "high", gpio_in_pin);
	}

	return 0;
}

// called when the character device is opened
static int set_use_inc(void *data)
{
	int result;

	/* initialize timestamp */
	do_gettimeofday(&lasttv);

	result = request_irq(irq_num,
			     (irq_handler_t) irq_handler,
			     IRQ_TYPE_EDGE_RISING | IRQ_TYPE_EDGE_FALLING,
			     LIRC_DRIVER_NAME, (void*) 0);

	switch (result) {
	case -EBUSY:
		printk(KERN_ERR LIRC_DRIVER_NAME
		       ": IRQ %d is busy\n",
		       irq_num);
		return -EBUSY;
	case -EINVAL:
		printk(KERN_ERR LIRC_DRIVER_NAME
		       ": Bad irq number or handler\n");
		return -EINVAL;
	default:
		dprintk("Interrupt %d obtained\n",
			irq_num);
		break;
	};

	/* initialize pulse/space widths */
	init_timing_params(duty_cycle, freq);

	return 0;
}

static void set_use_dec(void *data)
{
	/* GPIO Pin Falling/Rising Edge Detect Disable */
	irq_set_irq_type(irq_num, 0);
	disable_irq(irq_num);

	free_irq(irq_num, (void *) 0);

	dprintk(KERN_INFO LIRC_DRIVER_NAME
		": freed IRQ %d\n", irq_num);
}

static ssize_t lirc_write(struct file *file, const char *buf,
	size_t n, loff_t *ppos)
{
	int i, count;
	unsigned long flags;
	long delta = 0;
	int *wbuf;

	count = n / sizeof(int);
	if (n % sizeof(int) || count % 2 == 0)
		return -EINVAL;
	wbuf = memdup_user(buf, n);
	if (IS_ERR(wbuf))
		return PTR_ERR(wbuf);
	spin_lock_irqsave(&lock, flags);

	for (i = 0; i < count; i++) {
		if (i%2)
			send_space(wbuf[i] - delta);
		else
			delta = send_pulse(wbuf[i]);
	}
	for (i = 0; i < n_transmitters; i++)
		gpiochip->set(gpiochip, gpio_out_pin[i], invert);

	spin_unlock_irqrestore(&lock, flags);
	kfree(wbuf);
	return n;
}

static long lirc_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
	int result;
	__u32 value;

	switch (cmd) {
	case LIRC_GET_SEND_MODE:
		dprintk("LIRC_GET_SEND_MODE\n");
		return -ENOIOCTLCMD;
		break;

	case LIRC_SET_SEND_MODE:
		dprintk("LIRC_SET_SEND_MODE\n");
		result = get_user(value, (__u32 *) arg);
		if (result)
			return result;
		/* only LIRC_MODE_PULSE supported */
		if (value != LIRC_MODE_PULSE)
			return -ENOSYS;
		break;

	case LIRC_GET_LENGTH:
		dprintk("LIRC_GET_LENGTH\n");
		return -ENOSYS;
		break;

	case LIRC_SET_SEND_DUTY_CYCLE:
		dprintk("SET_SEND_DUTY_CYCLE\n");
		result = get_user(value, (__u32 *) arg);
		if (result)
			return result;
		if (value <= 0 || value > 100)
			return -EINVAL;
		return init_timing_params(value, freq);
		break;

	case LIRC_SET_SEND_CARRIER:
		dprintk("SET_SEND_CARRIER\n");
		result = get_user(value, (__u32 *) arg);
		if (result)
			return result;
		if (value > 500000 || value < 0)
			return -EINVAL;
		return init_timing_params(duty_cycle, value);
		break;

	case LIRC_SET_TRANSMITTER_MASK:
		result = get_user(value, (__u32 *) arg);
		dprintk("SET_TRANSMITTER_MASK %d\n", value);
		if (result)
			return result;
		if ((value & ((1 << n_transmitters) - 1)) != value)
			return n_transmitters;
		tx_mask = value;
		break;

	default:
		dprintk("COMMAND handed over to lirc_dev_fop_ioctl: %u\n", cmd);
		return lirc_dev_fop_ioctl(filep, cmd, arg);
	}
	return 0;
}

static const struct file_operations lirc_fops = {
	.owner		= THIS_MODULE,
	.write		= lirc_write,
	.unlocked_ioctl	= lirc_ioctl,
	.read		= lirc_dev_fop_read,
	.poll		= lirc_dev_fop_poll,
	.open		= lirc_dev_fop_open,
	.release	= lirc_dev_fop_close,
	.llseek		= no_llseek,
};

static struct lirc_driver driver = {
	.name		= LIRC_DRIVER_NAME,
	.minor		= -1,
	.code_length	= 1,
	.sample_rate	= 0,
	.data		= NULL,
	.add_to_buf	= NULL,
	.rbuf		= &rbuf,
	.set_use_inc	= set_use_inc,
	.set_use_dec	= set_use_dec,
	.fops		= &lirc_fops,
	.dev		= NULL,
	.owner		= THIS_MODULE,
};

static const struct of_device_id lirc_tegra_of_match[] = {
	{ .compatible = "tegra,lirc-tegra", },
	{},
};
MODULE_DEVICE_TABLE(of, lirc_tegra_of_match);

static struct platform_driver lirc_tegra_driver = {
	.driver = {
		.name   = LIRC_DRIVER_NAME,
		.owner  = THIS_MODULE,
		.of_match_table = of_match_ptr(lirc_tegra_of_match),
	},
};

static int __init lirc_tegra_init(void)
{
	struct device_node *node;
	int result;

	/* Init read buffer. */
	result = lirc_buffer_init(&rbuf, sizeof(int), RBUF_LEN);
	if (result < 0)
		return -ENOMEM;

	result = platform_driver_register(&lirc_tegra_driver);
	if (result) {
		printk(KERN_ERR LIRC_DRIVER_NAME
		       ": lirc register returned %d\n", result);
		goto exit_buffer_free;
	}

	node = of_find_compatible_node(NULL, NULL,
				       lirc_tegra_of_match[0].compatible);

	if (node) {
		/* DT-enabled */
		lirc_tegra_dev = of_find_device_by_node(node);
		WARN_ON(lirc_tegra_dev->dev.of_node != node);
		of_node_put(node);
	}
	else {
		lirc_tegra_dev = platform_device_alloc(LIRC_DRIVER_NAME, 0);
		if (!lirc_tegra_dev) {
			result = -ENOMEM;
			goto exit_driver_unregister;
		}

		result = platform_device_add(lirc_tegra_dev);
		if (result)
			goto exit_device_put;
	}

	return 0;

	exit_device_put:
	platform_device_put(lirc_tegra_dev);

	exit_driver_unregister:
	platform_driver_unregister(&lirc_tegra_driver);

	exit_buffer_free:
	lirc_buffer_free(&rbuf);

	return result;
}

static void lirc_tegra_exit(void)
{
	if (!lirc_tegra_dev->dev.of_node)
		platform_device_unregister(lirc_tegra_dev);
	platform_driver_unregister(&lirc_tegra_driver);
	lirc_buffer_free(&rbuf);
}

static int __init lirc_tegra_init_module(void)
{
	int result;
	int i;

	result = lirc_tegra_init();
	if (result)
		return result;

	result = init_port();
	if (result < 0)
		goto exit_tegra;

	driver.features = 0;
	if (softcarrier) {
		driver.features |= LIRC_CAN_SET_SEND_DUTY_CYCLE;
		driver.features |= LIRC_CAN_SET_SEND_CARRIER;
	}
	if (n_transmitters > 0)
		driver.features |= LIRC_CAN_SEND_PULSE;
	if (n_transmitters > 1)
		driver.features |= LIRC_CAN_SET_TRANSMITTER_MASK;
	if (gpio_in_pin != INVALID)
		driver.features |= LIRC_CAN_REC_MODE2;

	driver.dev = &lirc_tegra_dev->dev;
	driver.minor = lirc_register_driver(&driver);

	if (driver.minor < 0) {
		printk(KERN_ERR LIRC_DRIVER_NAME
		       ": device registration failed with %d\n", result);
		result = -EIO;
		goto exit_tegra;
	}

	printk(KERN_INFO LIRC_DRIVER_NAME ": driver registered!\n");

	dprintk("driver.features = %d\n", driver.features);
	dprintk("softcarrier = %d\n", softcarrier);
	dprintk("gpio_in_pin = %d\n", gpio_in_pin);
	for (i = 0; i < n_transmitters; i++)
		dprintk("gpio_out_pin[%d] = %d\n", i, gpio_out_pin[i]);

	return 0;

	exit_tegra:
	lirc_tegra_exit();

	return result;
}

static void __exit lirc_tegra_exit_module(void)
{
	int i;
	lirc_unregister_driver(driver.minor);

	for (i = 0; i < n_transmitters; i++)
		gpio_free(gpio_out_pin[i]);
	gpio_free(gpio_in_pin);

	lirc_tegra_exit();

	printk(KERN_INFO LIRC_DRIVER_NAME ": cleaned up module\n");
}

module_init(lirc_tegra_init_module);
module_exit(lirc_tegra_exit_module);

#define xstringify(s) stringify(s)
#define stringify(s) #s

MODULE_DESCRIPTION("Infra-red receiver and blaster driver for Jetson Nano GPIO.");
MODULE_AUTHOR("Aron Robert Szabo <aron@reon.hu>");
MODULE_AUTHOR("Michael Bishop <cleverca22@gmail.com>");
MODULE_AUTHOR("Bengt Martensson <barf@bengt-martensson.de>");
MODULE_AUTHOR("Trenton Ruf <truf@live.com>");
MODULE_LICENSE("GPL");

module_param_array(gpio_out_pin, int, &n_transmitters, S_IRUGO);
MODULE_PARM_DESC(gpio_out_pin, "GPIO output/transmitter pins of the BCM"
		 " processor. (default " xstringify(DEFAULT_GPIO_OUT_PIN) ")");

module_param(gpio_in_pin, int, S_IRUGO);
MODULE_PARM_DESC(gpio_in_pin, "GPIO input pin number of the BCM processor."
		 " (default " xstringify(DEFAULT_GPIO_IN_PIN) ")");
/*
module_param(gpio_in_pull, int, S_IRUGO);
MODULE_PARM_DESC(gpio_in_pull, "GPIO input pin pull configuration."
		 " (0 = off, 1 = up, 2 = down, default down)");
*/

module_param(sense, int, S_IRUGO);
MODULE_PARM_DESC(sense, "Override autodetection of IR receiver circuit"
		 " (0 = active high, 1 = active low )");

module_param(softcarrier, bool, S_IRUGO);
MODULE_PARM_DESC(softcarrier, "Software carrier (0 = off, 1 = on, default on)");

module_param(invert, bool, S_IRUGO);
MODULE_PARM_DESC(invert, "Invert output (0 = off, 1 = on, default off");

module_param(debug, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(debug, "Enable debugging messages");

// tx_mask is deliberately not made available as module parameter;
// it is a user parameter, not a hardware configuration parameter.
