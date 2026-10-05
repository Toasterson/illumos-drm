/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's firmware loading (<linux/firmware.h>) over firmload(9F)
 * (firmware_open(), firmware_get_size(), firmware_read(),
 * firmware_close(); provided by genunix, uts/common/os/firmload.c). A
 * name "dir/file" is firmware "file" of driver "dir", which firmload
 * looks up as firmware/dir/file in the module path (/kernel/firmware/,
 * /usr/kernel/firmware/, ...); a name without a slash uses the driver
 * name of the device's node. The whole file is read into one kmem
 * buffer. All of this may sleep (firmload allocates with KM_SLEEP and
 * reads the file), as request_firmware() may on Linux.
 */

#include <linux/kernel.h>
#include <linux/firmware.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/errno.h>
#include <sys/firmload.h>

/* Larger files are refused (-EFBIG) rather than exhausting kmem. */
#define	LKPI_FW_MAX	(64UL * 1024 * 1024)

/* struct firmware.priv of a firmware read into the caller's buffer. */
static char lkpi_fw_user_buf;

#define	LKPI_FW_WARN	0x1	/* log a failure */
#define	LKPI_FW_INTO	0x2	/* read into the caller's buffer */

static int
lkpi_fw_load(const struct firmware **fwp, const char *name,
    struct device *device, void *buf, size_t bufsize, uint_t flags)
{
	firmware_handle_t fh;
	struct firmware *fw;
	const char *slash, *drv, *img;
	char drvbuf[MAXNAMELEN];
	off_t size;
	void *data;
	int err;

	if (fwp == NULL)
		return (-EINVAL);
	*fwp = NULL;
	if (name == NULL || name[0] == '\0')
		return (-EINVAL);

	slash = strchr(name, '/');
	if (slash != NULL) {
		size_t len = (size_t)(slash - name);

		if (len == 0 || len >= sizeof (drvbuf) || slash[1] == '\0')
			return (-EINVAL);
		(void) memcpy(drvbuf, name, len);
		drvbuf[len] = '\0';
		drv = drvbuf;
		img = slash + 1;
	} else {
		dev_info_t *dip = device != NULL ? lkpi_dev_dip(device) : NULL;

		if (dip == NULL)
			return (-EINVAL);
		drv = ddi_driver_name(dip);
		img = name;
	}

	err = firmware_open(drv, img, &fh);
	if (err != 0) {
		err = -err;
		goto fail;
	}
	size = firmware_get_size(fh);
	if (size <= 0) {
		err = -EINVAL;
	} else if ((size_t)size > LKPI_FW_MAX) {
		err = -EFBIG;
	} else if ((flags & LKPI_FW_INTO) && (size_t)size > bufsize) {
		err = -EFBIG;
	} else {
		err = 0;
	}
	if (err != 0) {
		(void) firmware_close(fh);
		goto fail;
	}

	fw = kmem_zalloc(sizeof (*fw), KM_SLEEP);
	data = (flags & LKPI_FW_INTO) ? buf : kmem_alloc((size_t)size,
	    KM_SLEEP);
	if (firmware_read(fh, 0, data, (size_t)size) != 0) {
		if (!(flags & LKPI_FW_INTO))
			kmem_free(data, (size_t)size);
		kmem_free(fw, sizeof (*fw));
		(void) firmware_close(fh);
		err = -EIO;
		goto fail;
	}
	(void) firmware_close(fh);
	fw->size = (size_t)size;
	fw->data = data;
	fw->priv = (flags & LKPI_FW_INTO) ? &lkpi_fw_user_buf : NULL;
	*fwp = fw;
	return (0);

fail:
	if (flags & LKPI_FW_WARN) {
		cmn_err(CE_NOTE, "!linuxkpi: firmware %s (firmware/%s/%s) not "
		    "loaded: error %d", name, drv, img, err);
	}
	return (err);
}

int
request_firmware(const struct firmware **fw, const char *name,
    struct device *device)
{
	return (lkpi_fw_load(fw, name, device, NULL, 0, LKPI_FW_WARN));
}

int
firmware_request_nowarn(const struct firmware **fw, const char *name,
    struct device *device)
{
	return (lkpi_fw_load(fw, name, device, NULL, 0, 0));
}

/* There is no user-space fallback to skip: the same as nowarn. */
int
request_firmware_direct(const struct firmware **fw, const char *name,
    struct device *device)
{
	return (lkpi_fw_load(fw, name, device, NULL, 0, 0));
}

int
request_firmware_into_buf(const struct firmware **fw, const char *name,
    struct device *device, void *buf, size_t size)
{
	if (buf == NULL)
		return (-EINVAL);
	return (lkpi_fw_load(fw, name, device, buf, size,
	    LKPI_FW_WARN | LKPI_FW_INTO));
}

void
release_firmware(const struct firmware *fw)
{
	if (fw == NULL)
		return;
	if (fw->priv != &lkpi_fw_user_buf && fw->data != NULL)
		kmem_free((void *)fw->data, fw->size);
	kmem_free((void *)fw, sizeof (*fw));
}

/* request_firmware_nowait(): the load and the callback run in a work item. */
struct lkpi_fw_work {
	struct work_struct fw_work;
	char *fw_name;
	struct device *fw_device;
	void *fw_context;
	void (*fw_cont)(const struct firmware *, void *);
	bool fw_warn;
};

static void
lkpi_fw_work_fn(struct work_struct *w)
{
	struct lkpi_fw_work *fw = container_of(w, struct lkpi_fw_work,
	    fw_work);
	const struct firmware *f = NULL;

	(void) lkpi_fw_load(&f, fw->fw_name, fw->fw_device, NULL, 0,
	    fw->fw_warn ? LKPI_FW_WARN : 0);
	fw->fw_cont(f, fw->fw_context);
	kfree(fw->fw_name);
	kfree(fw);
}

/*
 * Queues the load on the system workqueue; cont() gets the firmware or
 * NULL and owns it. The device must stay attached until cont() ran.
 */
int
request_firmware_nowait(struct lx_module *module, bool uevent,
    const char *name, struct device *device, gfp_t gfp, void *context,
    void (*cont)(const struct firmware *fw, void *context))
{
	struct lkpi_fw_work *fw;

	if (name == NULL || cont == NULL)
		return (-EINVAL);
	fw = kzalloc(sizeof (*fw), gfp);
	if (fw == NULL)
		return (-ENOMEM);
	fw->fw_name = kstrdup(name, gfp);
	if (fw->fw_name == NULL) {
		kfree(fw);
		return (-ENOMEM);
	}
	fw->fw_device = device;
	fw->fw_context = context;
	fw->fw_cont = cont;
	fw->fw_warn = uevent;
	INIT_WORK(&fw->fw_work, lkpi_fw_work_fn);
	(void) queue_work(system_wq, &fw->fw_work);
	return (0);
}
