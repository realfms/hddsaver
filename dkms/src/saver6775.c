// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * nct6775 - Driver for the hardware monitoring functionality of
 *	       Nuvoton NCT677x Super-I/O chips
 *
 * Copyright (C) 2012  Guenter Roeck <linux@roeck-us.net>
 *
 * Derived from w83627ehf driver
 * Copyright (C) 2005-2012  Jean Delvare <jdelvare@suse.de>
 * Copyright (C) 2006  Yuan Mu (Winbond),
 *		       Rudolf Marek <r.marek@assembler.cz>
 *		       David Hubbard <david.c.hubbard@gmail.com>
 *		       Daniel J Blueman <daniel.blueman@gmail.com>
 * Copyright (C) 2010  Sheng-Yuan Huang (Nuvoton) (PS00)
 *
 * Shamelessly ripped from the w83627hf driver
 * Copyright (C) 2003  Mark Studebaker
 *
 * Supports the following chips:
 *
 * Chip        #vin    #fan    #pwm    #temp  chip IDs       man ID
 * nct6106d     9      3       3       6+3    0xc450 0xc1    0x5ca3
 * nct6116d     9      5       5       3+3    0xd280 0xc1    0x5ca3
 * nct6775f     9      4       3       6+3    0xb470 0xc1    0x5ca3
 * nct6776f     9      5       3       6+3    0xc330 0xc1    0x5ca3
 * nct6779d    15      5       5       2+6    0xc560 0xc1    0x5ca3
 * nct6791d    15      6       6       2+6    0xc800 0xc1    0x5ca3
 * nct6792d    15      6       6       2+6    0xc910 0xc1    0x5ca3
 * nct6793d    15      6       6       2+6    0xd120 0xc1    0x5ca3
 * nct6795d    14      6       6       2+6    0xd350 0xc1    0x5ca3
 * nct6796d    14      7       7       2+6    0xd420 0xc1    0x5ca3
 * nct6797d    14      7       7       2+6    0xd450 0xc1    0x5ca3
 *                                           (0xd451)
 * nct6798d    14      7       7       2+6    0xd428 0xc1    0x5ca3
 *                                           (0xd429)
 *
 * #temp lists the number of monitored temperature sources (first value) plus
 * the number of directly connectable temperature sensors (second value).
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/platform_device.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/hwmon-vid.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/acpi.h>
#include <linux/bitops.h>
#include <linux/dmi.h>
#include <linux/io.h>
#include <linux/nospec.h>
#include <linux/delay.h>

#define DRVNAME "saver6775"
#define SIO_REG_LDSEL		0x07	/* Logical device select */
#define SIO_REG_DEVID		0x20	/* Device ID (2 bytes) */
#define SIO_REG_ENABLE		0x30	/* Logical device enable */
#define SIO_REG_ADDR		0x60	/* Logical device address (2 bytes) */
#define SIO_ID_MASK		0xFFF8
#define SIO_NCT6791_ID		0xc800
#define NCT6775_LD_HWM		0x0b
#define NCT6775_LD_GPIO_DATA		0x08
#define NCT6775_REG_CR_GPIO1_DATA  0xf1
/* NCT6791 specific data */

#define NCT6791_REG_HM_IO_SPACE_LOCK_ENABLE	0x28

/*
 * ISA constants
 */

#define IOREGION_ALIGNMENT	(~7)
#define IOREGION_OFFSET		5
#define IOREGION_LENGTH		2
#define ADDR_REG_OFFSET		0
#define DATA_REG_OFFSET		1
#define NCT6775_REG_BANK	0x4E
#define NCT6775_REG_CONFIG	0x40

#define MAXRETRIES 5

enum kinds { nct6791 };

struct nct6775_sio_data {
	int sioreg;
	enum kinds kind;
};

/*
 * Data structures and manipulation thereof
 */

struct nct6775_data {
	int addr;	/* IO base of hw monitor block */
	int sioreg;	/* SIO register address */
	enum kinds kind;

	u16 REG_CONFIG;

	struct mutex update_lock;
	bool valid;		/* true if following fields are valid */
	unsigned long last_updated;	/* In jiffies */

	/* Register values */
	u8 bank;		/* current register bank */
	u8 in_num;		/* number of in inputs we have */
	u8 in[15][3];		/* [0]=in, [1]=in_max, [2]=in_min */

	bool have_hddsaver; /* True if hdd saver is supported */
	bool hddsaver_status; /* True if enabled */

	/* Remember extra register values over suspend/resume */
	u8 sio_reg_enable;
};

/* I/O functions*/
static inline void
superio_outb(int ioreg, int reg, int val)
{
	outb(reg, ioreg);
	outb(val, ioreg + 1);
}

static inline int
superio_inb(int ioreg, int reg)
{
	outb(reg, ioreg);
	return inb(ioreg + 1);
}

static inline void
superio_select(int ioreg, int ld)
{
	outb(SIO_REG_LDSEL, ioreg);
	outb(ld, ioreg + 1);
}

static inline int
superio_enter(int ioreg)
{
	/*
	 * Try to reserve <ioreg> and <ioreg + 1> for exclusive access.
	 */
	if (!request_muxed_region(ioreg, 2, DRVNAME))
		return -EBUSY;

	outb(0x87, ioreg);
	outb(0x87, ioreg);

	return 0;
}

static inline void
superio_exit(int ioreg)
{
	outb(0xaa, ioreg);
	outb(0x02, ioreg);
	outb(0x02, ioreg + 1);
	release_region(ioreg, 2);
}

static struct nct6775_data *nct6775_update_device(struct device *dev)
{
	struct nct6775_data *data = dev_get_drvdata(dev);

	mutex_lock(&data->update_lock);

	if (time_after(jiffies, data->last_updated + HZ + HZ / 2)
	    || !data->valid) {

		data->last_updated = jiffies;
		data->valid = true;
	}

	mutex_unlock(&data->update_lock);
	return data;
}


static ssize_t
show_hddsaver(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct nct6775_data *data = nct6775_update_device(dev);

	return sprintf(buf, "%s\n", (data->hddsaver_status ? "On" : "Off"));
}

static ssize_t
store_hddsaver(struct device *dev, struct device_attribute *attr, const char *buf,
	       size_t count)
{
	struct nct6775_data *data = dev_get_drvdata(dev);
	struct nct6775_sio_data *sio_data = dev_get_platdata(dev);
	bool val;
	int err, ret;
	u8 tmp;

	err = kstrtobool(buf, &val);
	if (err == -EINVAL)
		return -EINVAL;

	pr_info("Trying to change HDD Saver to %s\n", val ? "On" : "Off");
	mutex_lock(&data->update_lock);
	ret = superio_enter(sio_data->sioreg);
	if (ret) {
		count = ret;
		goto error;
	}

	if (val != data->hddsaver_status) {
		superio_select(sio_data->sioreg, NCT6775_LD_GPIO_DATA); /* Logical Device 8 */
		tmp = superio_inb(sio_data->sioreg,
				  NCT6775_REG_CR_GPIO1_DATA); /* GPIO1 date reg */
		superio_outb(sio_data->sioreg, NCT6775_REG_CR_GPIO1_DATA, tmp ^ (1<<0));
		data->hddsaver_status = val;
		pr_info("HDD Saver is %s\n", val ? "On" : "Off");
	}
	superio_exit(sio_data->sioreg);
error:
	mutex_unlock(&data->update_lock);
	return count;
}

static void nct6791_enable_io_mapping(int sioaddr)
{
	int val;

	val = superio_inb(sioaddr, NCT6791_REG_HM_IO_SPACE_LOCK_ENABLE);
	if (val & 0x10) {
		pr_info("Enabling hardware saver logical device mappings.\n");
		superio_outb(sioaddr, NCT6791_REG_HM_IO_SPACE_LOCK_ENABLE,
			     val & ~0x10);
	}
}

static int __maybe_unused hddsaver_suspend(struct device *dev)
{
	pr_warn("Entering suspend mode\n");
	struct nct6775_data *data = nct6775_update_device(dev);

	mutex_lock(&data->update_lock);
	//Should we store enable status?
	mutex_unlock(&data->update_lock);

	return 0;
}

static int __maybe_unused hddsaver_resume(struct device *dev)
{
	struct nct6775_data *data = dev_get_drvdata(dev);
	int sioreg = data->sioreg;
	int err = 0;
	u8 reg;

	pr_warn("Resuming from suspend\n");
	mutex_lock(&data->update_lock);
	data->bank = 0xff;		/* Force initial bank selection */

	err = superio_enter(sioreg);
	if (err)
		goto abort;

	superio_select(sioreg, NCT6775_LD_HWM);
	reg = superio_inb(sioreg, SIO_REG_ENABLE);
	if (reg != data->sio_reg_enable)
		superio_outb(sioreg, SIO_REG_ENABLE, data->sio_reg_enable);

	if (data->kind == nct6791)
		nct6791_enable_io_mapping(sioreg);

	superio_exit(sioreg);

abort:
	/* Force re-reading all values */
	data->valid = false;
	mutex_unlock(&data->update_lock);

	return err;
}

static DEVICE_ATTR(enable, S_IRUGO | S_IWUSR, show_hddsaver, store_hddsaver);
static struct attribute *hddsaver_attrs[] = {
    &dev_attr_enable.attr,
    NULL
};
ATTRIBUTE_GROUPS(hddsaver);

static int nct6775_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct nct6775_sio_data *sio_data = dev_get_platdata(dev);
	struct nct6775_data *data;
	struct resource *res;
	int i, err = 0, region = 0;
	u8 cr2a;
	struct device *hwmon_dev;
	const char *board_vendor, *board_name;

	//If we have hwmon activated, we need to retry to access chip
	for (i=0; i < MAXRETRIES; i++) {
		res = platform_get_resource(pdev, IORESOURCE_IO, 0);
		region = devm_request_region(&pdev->dev, res->start, IOREGION_LENGTH, DRVNAME);
		if (region)
			break;
		usleep_range(100000, 100001);
		pr_warn("Retrying chip access\n");
	}
	if (!region) {
		pr_warn("Chip is busy\n");
		return -EBUSY;
	}

	data = devm_kzalloc(&pdev->dev, sizeof(struct nct6775_data),
			    GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->kind = sio_data->kind;
	data->sioreg = sio_data->sioreg;
	data->addr = res->start;
	mutex_init(&data->update_lock);
	data->bank = 0xff;		/* Force initial bank selection */
	platform_set_drvdata(pdev, data);

	switch (data->kind) {
	case nct6791:
		data->in_num = 15;

		data->REG_CONFIG = NCT6775_REG_CONFIG;

		break;

	default:
		pr_warn("Incompatible chipset found: 0x%04x\n", data->kind);
		return -ENODEV;
	}

	board_vendor = dmi_get_system_info(DMI_BOARD_VENDOR);
	board_name = dmi_get_system_info(DMI_BOARD_NAME);

	err = superio_enter(sio_data->sioreg);
	if (err)
		return err;

	cr2a = superio_inb(sio_data->sioreg, 0x2a);
	switch (data->kind) {
	case nct6791:
		if (board_name && board_vendor &&
		    !strcmp(board_vendor, "ASRock")) {
            /* Z97 Extreme6 should also work (the same GPIO10 pin is used) */
            /* but it needs testing!!! */
			if (!strcmp(board_name, "Z97 Extreme4") || !strcmp(board_name, "Z97 Extreme6") || !strcmp(board_name, "X99 Extreme4/3.1")) {
                data->have_hddsaver = (cr2a & (1<<6));
            }
        }
        break;
	default:
		pr_warn("Incompatible chipset found: 0x%04x\n", data->kind);
	}

    if (data->have_hddsaver) {
		u8 tmp;

		pr_notice("HDD Saver found\n");
		superio_select(sio_data->sioreg, NCT6775_LD_GPIO_DATA); /* Logical Device 8 */
		tmp = superio_inb(sio_data->sioreg, NCT6775_REG_CR_GPIO1_DATA); /* GPIO1 data reg */
		data->hddsaver_status = tmp & (1<<0); /* check bit0 */
		if (data->hddsaver_status) {
			pr_warn("HDD Saver is disabled\n");
		} else {
			pr_warn("HDD Saver is enabled\n");
		}
	}

	superio_exit(sio_data->sioreg);
    err = sysfs_create_group(&pdev->dev.kobj, &hddsaver_group);
    if (err) {
        dev_err(&pdev->dev, "sysfs creation failed\n");
        return err;
    }

    return PTR_ERR_OR_ZERO(hwmon_dev);
}

static SIMPLE_DEV_PM_OPS(hddsaver_dev_pm_ops, hddsaver_suspend, hddsaver_resume);

static struct platform_driver saver_driver = {
	.driver = {
		.name	= DRVNAME,
		.pm	= &hddsaver_dev_pm_ops,
	},
	.probe		= nct6775_probe,
};

static struct platform_device *pdev;

/* nct6775_find() looks for a '627 in the Super-I/O config space */
static int __init nct6775_find(int sioaddr, struct nct6775_sio_data *sio_data)
{
	u16 val;
	int err;
	int addr;

	err = superio_enter(sioaddr);
	if (err)
		return err;

	val = (superio_inb(sioaddr, SIO_REG_DEVID) << 8) |
		superio_inb(sioaddr, SIO_REG_DEVID + 1);

	switch (val & SIO_ID_MASK) {
	case SIO_NCT6791_ID:
		sio_data->kind = nct6791;
		break;
	default:
		if (val != 0xffff)
			pr_debug("unsupported chip ID: 0x%04x\n", val);
		superio_exit(sioaddr);
		return -ENODEV;
	}

	/* We have a known chip, find the HWM I/O address */
	superio_select(sioaddr, NCT6775_LD_HWM);
	val = (superio_inb(sioaddr, SIO_REG_ADDR) << 8)
	    | superio_inb(sioaddr, SIO_REG_ADDR + 1);
	addr = val & IOREGION_ALIGNMENT;
	if (addr == 0) {
		pr_err("Refusing to enable a Super-I/O device with a base I/O port 0\n");
		superio_exit(sioaddr);
		return -ENODEV;
	}

	/* Activate logical device if needed */
	val = superio_inb(sioaddr, SIO_REG_ENABLE);
	if (!(val & 0x01)) {
		pr_warn("Forcibly enabling Super-I/O. Sensor is probably unusable.\n");
		superio_outb(sioaddr, SIO_REG_ENABLE, val | 0x01);
	}

	if (sio_data->kind == nct6791)
		nct6791_enable_io_mapping(sioaddr);

	superio_exit(sioaddr);
	pr_info("Found hddsaver or compatible chip at %#x:%#x\n",
		sioaddr, addr);
	sio_data->sioreg = sioaddr;

	return addr;
}

static int __init hddsaver_init(void)
{
	int err;
	bool found = false;
	int address;
	struct resource res;
	struct nct6775_sio_data sio_data;
	int sioaddr = 0x2e;

	err = platform_driver_register(&saver_driver);
	if (err)
		return err;

	/*
	 * initialize sio_data->kind and sio_data->sioreg.
	 *
	 * when Super-I/O functions move to a separate file, the Super-I/O
	 * driver will probe 0x2e and 0x4e and auto-detect the presence of a
	 * nct6775 hardware monitor, and call probe()
	 */
	address = nct6775_find(sioaddr, &sio_data);
	if (address > 0) {
		found = true;

		pdev = platform_device_alloc(DRVNAME, address);
		if (!pdev) {
			err = -ENOMEM;
			goto exit_device_unregister;
		}

		err = platform_device_add_data(pdev, &sio_data,
							sizeof(struct nct6775_sio_data));
		if (err)
			goto exit_device_put;

		memset(&res, 0, sizeof(res));
		res.name = DRVNAME;
		res.start = address + IOREGION_OFFSET;
		res.end = address + IOREGION_OFFSET + IOREGION_LENGTH - 1;
		res.flags = IORESOURCE_IO;

		err = acpi_check_resource_conflict(&res);
		if (err) {
			platform_device_put(pdev);
			pdev = NULL;
			err = -ENODEV;
			goto exit_unregister;
		}

		err = platform_device_add_resources(pdev, &res, 1);
		if (err)
			goto exit_device_put;

		/* platform_device_add calls probe() */
		err = platform_device_add(pdev);
		if (err)
			goto exit_device_put;
	}

	if (!found) {
		err = -ENODEV;
		goto exit_unregister;
	}

	return 0;

exit_device_put:
	platform_device_put(pdev);
exit_device_unregister:
	if (pdev)
		platform_device_unregister(pdev);
exit_unregister:
	platform_driver_unregister(&saver_driver);
	return err;
}

static int hddsaver_remove(struct platform_device *pdev) {
	sysfs_remove_group(&pdev->dev.kobj, &hddsaver_group);
	return 0;
}

static void __exit hddsaver_exit(void)
{
	if (pdev)
		platform_device_unregister(pdev);
	platform_driver_unregister(&saver_driver);
}

MODULE_AUTHOR("Francisco Mayoral <fmayoral@wisecoding.es>");
MODULE_DESCRIPTION("Driver for ASRock Extreme4 HDD Saver");
MODULE_LICENSE("GPL");

module_init(hddsaver_init);
module_exit(hddsaver_exit);
