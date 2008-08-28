/*
 * libudev - interface to udev device information
 *
 * Copyright (C) 2008 Kay Sievers <kay.sievers@vrfy.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _LIBUDEV_PRIVATE_H_
#define _LIBUDEV_PRIVATE_H_

#include "libudev.h"
#include "../udev.h"

struct udev {
	int refcount;
	void (*log_fn)(struct udev *udev,
		       int priority, const char *file, int line, const char *fn,
		       const char *format, va_list args);
};

struct udev_device {
	int refcount;
	struct udev *udev;
	struct udevice *udevice;
};

#ifdef USE_LOG
#define log_dbg(udev, arg...) \
	udev_log(udev, LOG_DEBUG, __FILE__, __LINE__, __FUNCTION__, ## arg)

#define log_info(udev, arg...) \
	udev_log(udev, LOG_INFO, __FILE__, __LINE__, __FUNCTION__, ## arg)

#define log_err(udev, arg...) \
	udev_log(udev, LOG_ERR, __FILE__, __LINE__, __FUNCTION__, ## arg)

void udev_log(struct udev *udev,
	      int priority, const char *file, int line, const char *fn,
	      const char *format, ...)
	      __attribute__ ((format(printf, 6, 7)));
#else
static inline void udev_log(struct udev *udev,
	      int priority, const char *file, int line, const char *fn,
	      const char *format, ...)
	      __attribute__ ((format(printf, 6, 7))) {}
#endif

extern ssize_t util_get_sys_subsystem(struct udev *udev, const char *devpath, char *subsystem, size_t size);

#endif