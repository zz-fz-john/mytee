/*
 * Copyright 2011 Broadcom Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 *
 * This driver dynamically manages the CPU Frequency of the ARM
 * processor. Messages are sent to Videocore either setting or requesting the
 * frequency of the ARM in order to match an appropiate frequency to the current
 * usage of the processor. The policy which selects the frequency to use is
 * defined in the kernel .config file, but can be changed during runtime.
 */

/* ---------- INCLUDES ---------- */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/cpufreq.h>
#include <soc/bcm2835/raspberrypi-firmware.h>

#ifdef CONFIG_MYTEE
#include <asm/mytee.h>
#include <asm/virt.h>
#endif

/* ---------- DEFINES ---------- */
/*#define CPUFREQ_DEBUG_ENABLE*/		/* enable debugging */
#define MODULE_NAME "bcm2835-cpufreq"

#define VCMSG_ID_ARM_CLOCK 0x000000003		/* Clock/Voltage ID's */

/* debug printk macros */
#ifdef CPUFREQ_DEBUG_ENABLE
#define print_debug(fmt,...) pr_debug("%s:%s:%d: "fmt, MODULE_NAME, __func__, __LINE__, ##__VA_ARGS__)
#else
#define print_debug(fmt,...)
#endif
#define print_err(fmt,...) pr_err("%s:%s:%d: "fmt, MODULE_NAME, __func__,__LINE__, ##__VA_ARGS__)
#define print_info(fmt,...) pr_info("%s: "fmt, MODULE_NAME, ##__VA_ARGS__)

/* ---------- GLOBALS ---------- */
static struct cpufreq_driver bcm2835_cpufreq_driver;	/* the cpufreq driver global */
static unsigned int min_frequency, max_frequency;
static struct cpufreq_frequency_table bcm2835_freq_table[3];

/*
 ===============================================
  clk_rate either gets or sets the clock rates.
 ===============================================
*/

static int bcm2835_cpufreq_clock_property(u32 tag, u32 id, u32 *val)
{
	struct rpi_firmware *fw = rpi_firmware_get(NULL);
	struct {
		u32 id;
		u32 val;
	} packet;
	int ret;
	
	#ifdef CONFIG_MYTEE
	mytee_up_priv(MYTEE_UP_PRIV,0,0,0);
	u32* trusted_display_mmap_flag = MYTEE_TRUSTED_FB_MMAP_FLAG_VIRT;
	u32* trusted_display_write_flag = MYTEE_TRUSTED_FB_WRITE_FLAG_VIRT;

	if(*trusted_display_mmap_flag==0x1 || *trusted_display_write_flag==0x1){		// mailbox requests caused by periodic tesks is blocked 
		mytee_down_priv(MYTEE_DOWN_PRIV,0);
		goto exit;
	}
	mytee_down_priv(MYTEE_DOWN_PRIV,0);
	#endif

	packet.id = id;
	packet.val = *val;
	ret = rpi_firmware_property(fw, tag, &packet, sizeof(packet));
	if (ret)
		return ret;

	*val = packet.val;
#ifdef CONFIG_MYTEE
exit:
#endif
	return 0;
}

static uint32_t bcm2835_cpufreq_set_clock(int cur_rate, int arm_rate)
{
	u32 rate = arm_rate * 1000;
	int ret;

	ret = bcm2835_cpufreq_clock_property(RPI_FIRMWARE_SET_CLOCK_RATE, VCMSG_ID_ARM_CLOCK, &rate);
	if (ret) {
		print_err("Failed to set clock: %d (%d)\n", arm_rate, ret);
		return 0;
	}

	rate /= 1000;
	print_debug("Setting new frequency = %d -> %d (actual %d)\n", cur_rate, arm_rate, rate);

	return rate;
}

static uint32_t bcm2835_cpufreq_get_clock(int tag)
{
	u32 rate;
	int ret;

	ret = bcm2835_cpufreq_clock_property(tag, VCMSG_ID_ARM_CLOCK, &rate);
	if (ret) {
		print_err("Failed to get clock (%d)\n", ret);
		return 0;
	}

	rate /= 1000;
	print_debug("%s frequency = %u\n",
		tag == RPI_FIRMWARE_GET_CLOCK_RATE ? "Current":
		tag == RPI_FIRMWARE_GET_MIN_CLOCK_RATE ? "Min":
		tag == RPI_FIRMWARE_GET_MAX_CLOCK_RATE ? "Max":
		"Unexpected", rate);

	return rate;
}

/*
 ====================================================
  Module Initialisation registers the cpufreq driver
 ====================================================
*/
static int __init bcm2835_cpufreq_module_init(void)
{
	print_debug("IN\n");
	return cpufreq_register_driver(&bcm2835_cpufreq_driver);
}

/*
 =============
  Module exit
 =============
*/
static void __exit bcm2835_cpufreq_module_exit(void)
{
	print_debug("IN\n");
	cpufreq_unregister_driver(&bcm2835_cpufreq_driver);
	return;
}

/*
 ==============================================================
  Initialisation function sets up the CPU policy for first use
 ==============================================================
*/
static int bcm2835_cpufreq_driver_init(struct cpufreq_policy *policy)
{
	/* measured value of how long it takes to change frequency */
	const unsigned int transition_latency = 355000; /* ns */

	if (!rpi_firmware_get(NULL)) {
		print_err("Firmware is not available\n");
		return -ENODEV;
	}

	/* now find out what the maximum and minimum frequencies are */
	min_frequency = bcm2835_cpufreq_get_clock(RPI_FIRMWARE_GET_MIN_CLOCK_RATE);
	max_frequency = bcm2835_cpufreq_get_clock(RPI_FIRMWARE_GET_MAX_CLOCK_RATE);

	if (min_frequency == max_frequency) {
		bcm2835_freq_table[0].frequency = min_frequency;
		bcm2835_freq_table[1].frequency = CPUFREQ_TABLE_END;
	} else {
		bcm2835_freq_table[0].frequency = min_frequency;
		bcm2835_freq_table[1].frequency = max_frequency;
		bcm2835_freq_table[2].frequency = CPUFREQ_TABLE_END;
	}

	print_info("min=%d max=%d\n", min_frequency, max_frequency);
	return cpufreq_generic_init(policy, bcm2835_freq_table, transition_latency);
}

/*
 =====================================================================
  Target index function chooses the requested frequency from the table
 =====================================================================
*/

static int bcm2835_cpufreq_driver_target_index(struct cpufreq_policy *policy, unsigned int state)
{
	unsigned int target_freq = state == 0 ? min_frequency : max_frequency;
	unsigned int cur = bcm2835_cpufreq_set_clock(policy->cur, target_freq);

	if (!cur)
	{
		print_err("Error occurred setting a new frequency (%d)\n", target_freq);
		return -EINVAL;
	}
	print_debug("%s: %i: freq %d->%d\n", policy->governor->name, state, policy->cur, cur);
	return 0;
}

/*
 ======================================================
  Get function returns the current frequency from table
 ======================================================
*/

static unsigned int bcm2835_cpufreq_driver_get(unsigned int cpu)
{
	unsigned int actual_rate = bcm2835_cpufreq_get_clock(RPI_FIRMWARE_GET_CLOCK_RATE);
	print_debug("cpu%d: freq=%d\n", cpu, actual_rate);
	return actual_rate <= min_frequency ? min_frequency : max_frequency;
}

/* the CPUFreq driver */
static struct cpufreq_driver bcm2835_cpufreq_driver = {
	.name         = "BCM2835 CPUFreq",
	.init         = bcm2835_cpufreq_driver_init,
	.verify       = cpufreq_generic_frequency_table_verify,
	.target_index = bcm2835_cpufreq_driver_target_index,
	.get          = bcm2835_cpufreq_driver_get,
	.attr         = cpufreq_generic_attr,
};

MODULE_AUTHOR("Dorian Peake and Dom Cobley");
MODULE_DESCRIPTION("CPU frequency driver for BCM2835 chip");
MODULE_LICENSE("GPL");

module_init(bcm2835_cpufreq_module_init);
module_exit(bcm2835_cpufreq_module_exit);
