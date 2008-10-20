/*
 * Copyright (C) 2004-2008 Kay Sievers <kay.sievers@vrfy.org>
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

#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <linux/sockios.h>

#include "udev.h"

struct udev_event *udev_event_new(struct udev_device *dev)
{
	struct udev_event *event;

	event = malloc(sizeof(struct udev_event));
	if (event == NULL)
		return NULL;
	memset(event, 0x00, sizeof(struct udev_event));

	event->dev = dev;
	event->udev = udev_device_get_udev(dev);
	udev_list_init(&event->run_list);
	event->mode = 0660;
	util_strlcpy(event->owner, "0", sizeof(event->owner));
	util_strlcpy(event->group, "0", sizeof(event->group));

	dbg(event->udev, "allocated event %p\n", event);
	return event;
}

void udev_event_unref(struct udev_event *event)
{
	udev_list_cleanup_entries(event->udev, &event->run_list);
	dbg(event->udev, "free event %p\n", event);
	free(event);
}

/* extract possible {attr} and move str behind it */
static char *get_format_attribute(struct udev *udev, char **str)
{
	char *pos;
	char *attr = NULL;

	if (*str[0] == '{') {
		pos = strchr(*str, '}');
		if (pos == NULL) {
			err(udev, "missing closing brace for format\n");
			return NULL;
		}
		pos[0] = '\0';
		attr = *str+1;
		*str = pos+1;
		dbg(udev, "attribute='%s', str='%s'\n", attr, *str);
	}
	return attr;
}

/* extract possible format length and move str behind it*/
static int get_format_len(struct udev *udev, char **str)
{
	int num;
	char *tail;

	if (isdigit(*str[0])) {
		num = (int) strtoul(*str, &tail, 10);
		if (num > 0) {
			*str = tail;
			dbg(udev, "format length=%i\n", num);
			return num;
		} else {
			err(udev, "format parsing error '%s'\n", *str);
		}
	}
	return -1;
}

/* handle "[<SUBSYSTEM>/<KERNEL>]<attribute>" format */
int udev_event_apply_subsys_kernel(struct udev_event *udev_event, const char *string,
				   char *result, size_t maxsize, int read_value)
{
	char temp[UTIL_PATH_SIZE];
	char *subsys;
	char *sysname;
	char *attr;
	struct udev_device *dev;

	if (string == NULL)
		string = result;
	if (string[0] != '[')
		return -1;

	util_strlcpy(temp, string, sizeof(temp));

	subsys = &temp[1];

	sysname = strchr(subsys, '/');
	if (sysname == NULL)
		return -1;
	sysname[0] = '\0';
	sysname = &sysname[1];

	attr = strchr(sysname, ']');
	if (attr == NULL)
		return -1;
	attr[0] = '\0';
	attr = &attr[1];
	if (attr[0] == '/')
		attr = &attr[1];
	if (attr[0] == '\0')
		attr = NULL;

	if (read_value && attr == NULL)
		return -1;

	dev = udev_device_new_from_subsystem_sysname(udev_event->udev, subsys, sysname);
	if (dev == NULL)
		return -1;

	if (read_value) {
		const char *val;

		val = udev_device_get_sysattr_value(dev, attr);
		if (val != NULL)
			util_strlcpy(result, val, maxsize);
		else
			result[0] = '\0';
		info(udev_event->udev, "value '[%s/%s]%s' is '%s'\n", subsys, sysname, attr, result);
	} else {
		util_strlcpy(result, udev_device_get_syspath(dev), maxsize);
		if (attr != NULL) {
			util_strlcat(result, "/", maxsize);
			util_strlcat(result, attr, maxsize);
		}
		info(udev_event->udev, "path '[%s/%s]%s' is '%s'\n", subsys, sysname, attr, result);
	}
	udev_device_unref(dev);
	return 0;
}

void udev_event_apply_format(struct udev_event *event, char *string, size_t maxsize)
{
	struct udev_device *dev = event->dev;
	char temp[UTIL_PATH_SIZE];
	char temp2[UTIL_PATH_SIZE];
	char *head, *tail, *cpos, *attr, *rest;
	int len;
	int i;
	int count;
	enum subst_type {
		SUBST_UNKNOWN,
		SUBST_DEVPATH,
		SUBST_KERNEL,
		SUBST_KERNEL_NUMBER,
		SUBST_ID,
		SUBST_DRIVER,
		SUBST_MAJOR,
		SUBST_MINOR,
		SUBST_RESULT,
		SUBST_ATTR,
		SUBST_PARENT,
		SUBST_TEMP_NODE,
		SUBST_NAME,
		SUBST_LINKS,
		SUBST_ROOT,
		SUBST_SYS,
		SUBST_ENV,
	};
	static const struct subst_map {
		char *name;
		char fmt;
		enum subst_type type;
	} map[] = {
		{ .name = "devpath",	.fmt = 'p',	.type = SUBST_DEVPATH },
		{ .name = "number",	.fmt = 'n',	.type = SUBST_KERNEL_NUMBER },
		{ .name = "kernel",	.fmt = 'k',	.type = SUBST_KERNEL },
		{ .name = "id",		.fmt = 'b',	.type = SUBST_ID },
		{ .name = "driver",	.fmt = 'd',	.type = SUBST_DRIVER },
		{ .name = "major",	.fmt = 'M',	.type = SUBST_MAJOR },
		{ .name = "minor",	.fmt = 'm',	.type = SUBST_MINOR },
		{ .name = "result",	.fmt = 'c',	.type = SUBST_RESULT },
		{ .name = "attr",	.fmt = 's',	.type = SUBST_ATTR },
		{ .name = "sysfs",	.fmt = 's',	.type = SUBST_ATTR },
		{ .name = "parent",	.fmt = 'P',	.type = SUBST_PARENT },
		{ .name = "tempnode",	.fmt = 'N',	.type = SUBST_TEMP_NODE },
		{ .name = "name",	.fmt = 'D',	.type = SUBST_NAME },
		{ .name = "links",	.fmt = 'L',	.type = SUBST_LINKS },
		{ .name = "root",	.fmt = 'r',	.type = SUBST_ROOT },
		{ .name = "sys",	.fmt = 'S',	.type = SUBST_SYS },
		{ .name = "env",	.fmt = 'E',	.type = SUBST_ENV },
		{ NULL, '\0', 0 }
	};
	enum subst_type type;
	const struct subst_map *subst;

	head = string;
	while (1) {
		len = -1;
		while (head[0] != '\0') {
			if (head[0] == '$') {
				/* substitute named variable */
				if (head[1] == '\0')
					break;
				if (head[1] == '$') {
					util_strlcpy(temp, head+2, sizeof(temp));
					util_strlcpy(head+1, temp, maxsize);
					head++;
					continue;
				}
				head[0] = '\0';
				for (subst = map; subst->name; subst++) {
					if (strncasecmp(&head[1], subst->name, strlen(subst->name)) == 0) {
						type = subst->type;
						tail = head + strlen(subst->name)+1;
						dbg(event->udev, "will substitute format name '%s'\n", subst->name);
						goto found;
					}
				}
				head[0] = '$';
				err(event->udev, "unknown format variable '%s'\n", head);
			} else if (head[0] == '%') {
				/* substitute format char */
				if (head[1] == '\0')
					break;
				if (head[1] == '%') {
					util_strlcpy(temp, head+2, sizeof(temp));
					util_strlcpy(head+1, temp, maxsize);
					head++;
					continue;
				}
				head[0] = '\0';
				tail = head+1;
				len = get_format_len(event->udev, &tail);
				for (subst = map; subst->name; subst++) {
					if (tail[0] == subst->fmt) {
						type = subst->type;
						tail++;
						dbg(event->udev, "will substitute format char '%c'\n", subst->fmt);
						goto found;
					}
				}
				head[0] = '%';
				err(event->udev, "unknown format char '%c'\n", tail[0]);
			}
			head++;
		}
		break;
found:
		attr = get_format_attribute(event->udev, &tail);
		util_strlcpy(temp, tail, sizeof(temp));
		dbg(event->udev, "format=%i, string='%s', tail='%s'\n", type ,string, tail);

		switch (type) {
		case SUBST_DEVPATH:
			util_strlcat(string, udev_device_get_devpath(dev), maxsize);
			dbg(event->udev, "substitute devpath '%s'\n", udev_device_get_devpath(dev));
			break;
		case SUBST_KERNEL:
			util_strlcat(string, udev_device_get_sysname(dev), maxsize);
			dbg(event->udev, "substitute kernel name '%s'\n", udev_device_get_sysname(dev));
			break;
		case SUBST_KERNEL_NUMBER:
			if (udev_device_get_sysnum(dev) == NULL)
				break;
			util_strlcat(string, udev_device_get_sysnum(dev), maxsize);
			dbg(event->udev, "substitute kernel number '%s'\n", udev_device_get_sysnum(dev));
			break;
		case SUBST_ID:
			if (event->dev_parent != NULL) {
				util_strlcat(string, udev_device_get_sysname(event->dev_parent), maxsize);
				dbg(event->udev, "substitute id '%s'\n", udev_device_get_sysname(event->dev_parent));
			}
			break;
		case SUBST_DRIVER:
			if (event->dev_parent != NULL) {
				const char *driver = udev_device_get_driver(event->dev_parent);

				if (driver == NULL)
					break;
				util_strlcat(string, driver, maxsize);
				dbg(event->udev, "substitute driver '%s'\n", driver);
			}
			break;
		case SUBST_MAJOR:
			sprintf(temp2, "%d", major(udev_device_get_devnum(dev)));
			util_strlcat(string, temp2, maxsize);
			dbg(event->udev, "substitute major number '%s'\n", temp2);
			break;
		case SUBST_MINOR:
			sprintf(temp2, "%d", minor(udev_device_get_devnum(dev)));
			util_strlcat(string, temp2, maxsize);
			dbg(event->udev, "substitute minor number '%s'\n", temp2);
			break;
		case SUBST_RESULT:
			if (event->program_result[0] == '\0')
				break;
			/* get part part of the result string */
			i = 0;
			if (attr != NULL)
				i = strtoul(attr, &rest, 10);
			if (i > 0) {
				dbg(event->udev, "request part #%d of result string\n", i);
				cpos = event->program_result;
				while (--i) {
					while (cpos[0] != '\0' && !isspace(cpos[0]))
						cpos++;
					while (isspace(cpos[0]))
						cpos++;
				}
				if (i > 0) {
					err(event->udev, "requested part of result string not found\n");
					break;
				}
				util_strlcpy(temp2, cpos, sizeof(temp2));
				/* %{2+}c copies the whole string from the second part on */
				if (rest[0] != '+') {
					cpos = strchr(temp2, ' ');
					if (cpos)
						cpos[0] = '\0';
				}
				util_strlcat(string, temp2, maxsize);
				dbg(event->udev, "substitute part of result string '%s'\n", temp2);
			} else {
				util_strlcat(string, event->program_result, maxsize);
				dbg(event->udev, "substitute result string '%s'\n", event->program_result);
			}
			break;
		case SUBST_ATTR:
			if (attr == NULL)
				err(event->udev, "missing file parameter for attr\n");
			else {
				char value[UTIL_NAME_SIZE] = "";
				size_t size;

				udev_event_apply_subsys_kernel(event, attr, value, sizeof(value), 1);

				/* try the current device, other matches may have selected */
				if (value[0] == '\0' && event->dev_parent != NULL && event->dev_parent != event->dev) {
					const char *val;

					val = udev_device_get_sysattr_value(event->dev_parent, attr);
					if (val != NULL)
						util_strlcpy(value, val, sizeof(value));
				}

				/* look at all devices along the chain of parents */
				if (value[0] == '\0') {
					struct udev_device *dev_parent = dev;
					const char *val;

					do {
						dbg(event->udev, "looking at '%s'\n", udev_device_get_syspath(dev_parent));
						val = udev_device_get_sysattr_value(dev_parent, attr);
						if (val != NULL) {
							util_strlcpy(value, val, sizeof(value));
							break;
						}
						dev_parent = udev_device_get_parent(dev_parent);
					} while (dev_parent != NULL);
				}

				if (value[0]=='\0')
					break;

				/* strip trailing whitespace, and replace unwanted characters */
				size = strlen(value);
				while (size > 0 && isspace(value[--size]))
					value[size] = '\0';
				count = util_replace_chars(value, ALLOWED_CHARS_INPUT);
				if (count > 0)
					info(event->udev, "%i character(s) replaced\n" , count);
				util_strlcat(string, value, maxsize);
				dbg(event->udev, "substitute sysfs value '%s'\n", value);
			}
			break;
		case SUBST_PARENT:
			{
				struct udev_device *dev_parent;
				const char *devnode;

				dev_parent = udev_device_get_parent(event->dev);
				if (dev_parent == NULL)
					break;
				devnode = udev_device_get_devnode(dev_parent);
				if (devnode != NULL) {
					size_t devlen = strlen(udev_get_dev_path(event->udev))+1;

					util_strlcat(string, &devnode[devlen], maxsize);
					dbg(event->udev, "found parent '%s', got node name '%s'\n",
					    udev_device_get_syspath(dev_parent), &devnode[devlen]);
				}
			}
			break;
		case SUBST_TEMP_NODE:
			if (event->tmp_node[0] == '\0' && major(udev_device_get_devnum(dev)) > 0) {
				dbg(event->udev, "create temporary device node for callout\n");
				snprintf(event->tmp_node, sizeof(event->tmp_node), "%s/.tmp-%u-%u",
					 udev_get_dev_path(event->udev),
					 major(udev_device_get_devnum(dev)), minor(udev_device_get_devnum(dev)));
				udev_node_mknod(dev, event->tmp_node, makedev(0,0), 0600, 0, 0);
			}
			util_strlcat(string, event->tmp_node, maxsize);
			dbg(event->udev, "substitute temporary device node name '%s'\n", event->tmp_node);
			break;
		case SUBST_NAME:
			if (event->name != NULL) {
				util_strlcat(string, event->name, maxsize);
				dbg(event->udev, "substitute name '%s'\n", event->name);
			} else {
				util_strlcat(string, udev_device_get_sysname(dev), maxsize);
				dbg(event->udev, "substitute sysname '%s'\n", udev_device_get_sysname(dev));
			}
			break;
		case SUBST_LINKS:
			{
				struct udev_list_entry *list_entry;

				list_entry = udev_device_get_properties_list_entry(dev);
				util_strlcpy(string, udev_list_entry_get_name(list_entry), maxsize);
				udev_list_entry_foreach(list_entry, udev_list_entry_get_next(list_entry)) {
					util_strlcat(string, " ", maxsize);
					util_strlcat(string, udev_list_entry_get_name(list_entry), maxsize);
				}
			}
			break;
		case SUBST_ROOT:
			util_strlcat(string, udev_get_dev_path(event->udev), maxsize);
			dbg(event->udev, "substitute udev_root '%s'\n", udev_get_dev_path(event->udev));
			break;
		case SUBST_SYS:
			util_strlcat(string, udev_get_sys_path(event->udev), maxsize);
			dbg(event->udev, "substitute sys_path '%s'\n", udev_get_sys_path(event->udev));
			break;
		case SUBST_ENV:
			if (attr == NULL) {
				dbg(event->udev, "missing attribute\n");
				break;
			} else {
				struct udev_list_entry *list_entry;
				const char *value;

				list_entry = udev_device_get_properties_list_entry(event->dev);
				list_entry = udev_list_entry_get_by_name(list_entry, attr);
				if (list_entry == NULL)
					break;
				value = udev_list_entry_get_value(list_entry);
				dbg(event->udev, "substitute env '%s=%s'\n", attr, value);
				util_strlcat(string, value, maxsize);
				break;
			}
		default:
			err(event->udev, "unknown substitution type=%i\n", type);
			break;
		}
		/* possibly truncate to format-char specified length */
		if (len >= 0 && len < (int)strlen(head)) {
			head[len] = '\0';
			dbg(event->udev, "truncate to %i chars, subtitution string becomes '%s'\n", len, head);
		}
		util_strlcat(string, temp, maxsize);
	}
}

static void rename_netif_kernel_log(struct ifreq ifr)
{
	int klog;
	FILE *f;

	klog = open("/dev/kmsg", O_WRONLY);
	if (klog < 0)
		return;

	f = fdopen(klog, "w");
	if (f == NULL) {
		close(klog);
		return;
	}

	fprintf(f, "<6>udev: renamed network interface %s to %s\n",
		ifr.ifr_name, ifr.ifr_newname);
	fclose(f);
}

static int rename_netif(struct udev_event *event)
{
	struct udev_device *dev = event->dev;
	int sk;
	struct ifreq ifr;
	int err;

	info(event->udev, "changing net interface name from '%s' to '%s'\n",
	     udev_device_get_sysname(dev), event->name);
	if (event->test)
		return 0;

	sk = socket(PF_INET, SOCK_DGRAM, 0);
	if (sk < 0) {
		err(event->udev, "error opening socket: %m\n");
		return -1;
	}

	memset(&ifr, 0x00, sizeof(struct ifreq));
	util_strlcpy(ifr.ifr_name, udev_device_get_sysname(dev), IFNAMSIZ);
	util_strlcpy(ifr.ifr_newname, event->name, IFNAMSIZ);
	err = ioctl(sk, SIOCSIFNAME, &ifr);
	if (err == 0)
		rename_netif_kernel_log(ifr);
	else {
		int loop;

		/* see if the destination interface name already exists */
		if (errno != EEXIST) {
			err(event->udev, "error changing netif name %s to %s: %m\n",
			    ifr.ifr_name, ifr.ifr_newname);
			goto exit;
		}

		/* free our own name, another process may wait for us */
		util_strlcpy(ifr.ifr_newname, udev_device_get_sysname(dev), IFNAMSIZ);
		util_strlcat(ifr.ifr_newname, "_rename", IFNAMSIZ);
		err = ioctl(sk, SIOCSIFNAME, &ifr);
		if (err != 0) {
			err(event->udev, "error changing netif name %s to %s: %m\n",
			    ifr.ifr_name, ifr.ifr_newname);
			goto exit;
		}

		/* wait 30 seconds for our target to become available */
		util_strlcpy(ifr.ifr_name, ifr.ifr_newname, IFNAMSIZ);
		util_strlcpy(ifr.ifr_newname, udev_device_get_devnode(dev), IFNAMSIZ);
		loop = 30 * 20;
		while (loop--) {
			err = ioctl(sk, SIOCSIFNAME, &ifr);
			if (err == 0) {
				rename_netif_kernel_log(ifr);
				break;
			}

			if (errno != EEXIST) {
				err(event->udev, "error changing net interface name %s to %s: %m\n",
				    ifr.ifr_name, ifr.ifr_newname);
				break;
			}
			dbg(event->udev, "wait for netif '%s' to become free, loop=%i\n",
			    udev_device_get_devnode(dev), (30 * 20) - loop);
			usleep(1000 * 1000 / 20);
		}
	}
exit:
	close(sk);
	return err;
}

int udev_event_execute_rules(struct udev_event *event, struct udev_rules *rules)
{
	struct udev_device *dev = event->dev;
	int err = 0;

	if (udev_device_get_devpath_old(dev) != NULL) {
		if (udev_device_rename_db(dev, udev_device_get_devpath(dev)) == 0)
			info(event->udev, "moved database from '%s' to '%s'\n",
			     udev_device_get_devpath_old(dev), udev_device_get_devpath(dev));
	}

	/* add device node */
	if (major(udev_device_get_devnum(dev)) != 0 &&
	    (strcmp(udev_device_get_action(dev), "add") == 0 || strcmp(udev_device_get_action(dev), "change") == 0)) {
		char filename[UTIL_PATH_SIZE];
		struct udev_device *dev_old;

		dbg(event->udev, "device node add '%s'\n", udev_device_get_devpath(dev));

		udev_rules_get_name(rules, event);
		if (event->ignore_device) {
			info(event->udev, "device event will be ignored\n");
			goto exit;
		}

		if (event->name[0] == '\0') {
			info(event->udev, "device node creation supressed\n");
			goto exit;
		}

		/* set device node name */
		util_strlcpy(filename, udev_get_dev_path(event->udev), sizeof(filename));
		util_strlcat(filename, "/", sizeof(filename));
		util_strlcat(filename, event->name, sizeof(filename));
		udev_device_set_devnode(dev, filename);

		/* read current database entry; cleanup, if it is known device */
		dev_old = udev_device_new_from_syspath(event->udev, udev_device_get_syspath(dev));
		if (dev_old != NULL) {
			info(event->udev, "device '%s' already in database, updating\n",
			     udev_device_get_devpath(dev));
			udev_node_update_old_links(dev, dev_old, event->test);
			udev_device_unref(dev_old);
		}

		udev_device_update_db(dev);

		err = udev_node_add(dev, event->mode, event->owner, event->group, event->test);
		if (err != 0)
			goto exit;

		goto exit;
	}

	/* add netif */
	if (strcmp(udev_device_get_subsystem(dev), "net") == 0 && strcmp(udev_device_get_action(dev), "add") == 0) {
		dbg(event->udev, "netif add '%s'\n", udev_device_get_devpath(dev));

		udev_rules_get_name(rules, event);
		if (event->ignore_device) {
			info(event->udev, "device event will be ignored\n");
			goto exit;
		}
		if (event->name[0] == '\0') {
			info(event->udev, "device renaming supressed\n");
			goto exit;
		}

		/* look if we want to change the name of the netif */
		if (strcmp(event->name, udev_device_get_sysname(dev)) != 0) {
			char syspath[UTIL_PATH_SIZE];
			char *pos;

			err = rename_netif(event);
			if (err != 0)
				goto exit;
			info(event->udev, "renamed netif to '%s'\n", event->name);

			/* remember old name */
			udev_device_add_property(dev, "INTERFACE_OLD", udev_device_get_sysname(dev));

			/* now change the devpath, because the kernel device name has changed */
			util_strlcpy(syspath, udev_device_get_syspath(dev), sizeof(syspath));
			pos = strrchr(syspath, '/');
			if (pos != NULL) {
				pos[1] = '\0';
				util_strlcat(syspath, event->name, sizeof(syspath));
				udev_device_set_syspath(event->dev, syspath);
				udev_device_add_property(dev, "INTERFACE", udev_device_get_sysname(dev));
				info(event->udev, "changed devpath to '%s'\n", udev_device_get_devpath(dev));
			}
		}
		goto exit;
	}

	/* remove device node */
	if (major(udev_device_get_devnum(dev)) != 0 && strcmp(udev_device_get_action(dev), "remove") == 0) {
		/* import database entry and delete it */
		udev_device_read_db(dev);
		if (!event->test)
			udev_device_delete_db(dev);

		if (udev_device_get_devnode(dev) == NULL) {
			char devnode[UTIL_PATH_SIZE];

			info(event->udev, "'%s' not found in database, using kernel name '%s'\n",
			     udev_device_get_syspath(dev), udev_device_get_sysname(dev));
			util_strlcpy(devnode, udev_get_dev_path(event->udev), sizeof(devnode));
			util_strlcat(devnode, "/", sizeof(devnode));
			util_strlcat(devnode, udev_device_get_sysname(dev), sizeof(devnode));
			udev_device_set_devnode(dev, devnode);
		}

		udev_rules_get_run(rules, event);
		if (event->ignore_device) {
			info(event->udev, "device event will be ignored\n");
			goto exit;
		}

		if (udev_device_get_ignore_remove(dev)) {
			info(event->udev, "ignore_remove for '%s'\n", udev_device_get_devnode(dev));
			goto exit;
		}

		err = udev_node_remove(dev, event->test);
		goto exit;
	}

	/* default devices */
	udev_rules_get_run(rules, event);
	if (event->ignore_device)
		info(event->udev, "device event will be ignored\n");
exit:
	return err;
}

int udev_event_execute_run(struct udev_event *event)
{
	struct udev_list_entry *list_entry;
	int err = 0;

	dbg(event->udev, "executing run list\n");
	udev_list_entry_foreach(list_entry, udev_list_get_entry(&event->run_list)) {
		const char *cmd = udev_list_entry_get_name(list_entry);

		if (strncmp(cmd, "socket:", strlen("socket:")) == 0) {
			struct udev_monitor *monitor;

			monitor = udev_monitor_new_from_socket(event->udev, &cmd[strlen("socket:")]);
			if (monitor == NULL)
				continue;
			udev_monitor_send_device(monitor, event->dev);
			udev_monitor_unref(monitor);
		} else {
			char program[UTIL_PATH_SIZE];
			char **envp;

			util_strlcpy(program, cmd, sizeof(program));
			udev_event_apply_format(event, program, sizeof(program));
			envp = udev_device_get_properties_envp(event->dev);
			if (util_run_program(event->udev, program, envp, NULL, 0, NULL) != 0) {
				if (!udev_list_entry_get_flag(list_entry))
					err = -1;
			}
		}
	}
	return err;
}