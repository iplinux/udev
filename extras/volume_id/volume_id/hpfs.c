/*
 * volume_id - reads filesystem label and uuid
 *
 * Copyright (C) 2005 Kay Sievers <kay.sievers@vrfy.org>
 *
 *	This library is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU Lesser General Public
 *	License as published by the Free Software Foundation; either
 *	version 2.1 of the License, or (at your option) any later version.
 *
 *	This library is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 *	Lesser General Public License for more details.
 *
 *	You should have received a copy of the GNU Lesser General Public
 *	License along with this library; if not, write to the Free Software
 *	Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <asm/types.h>

#include "volume_id.h"
#include "logging.h"
#include "util.h"
#include "hpfs.h"

struct hpfs_super
{
	__u8	magic[4];
	__u8	version;
} __attribute__((__packed__));

#define HPFS_SUPERBLOCK_OFFSET			0x2000

int volume_id_probe_hpfs(struct volume_id *id, __u64 off)
{
	struct hpfs_super *hs;

	dbg("probing at offset 0x%llx", (unsigned long long) off);

	hs = (struct hpfs_super *) volume_id_get_buffer(id, off + HPFS_SUPERBLOCK_OFFSET, 0x200);
	if (hs == NULL)
		return -1;

	if (memcmp(hs->magic, "\x49\xe8\x95\xf9", 4) == 0) {
		sprintf(id->type_version, "%u", hs->version);

		volume_id_set_usage(id, VOLUME_ID_FILESYSTEM);
		id->type = "hpfs";
		return 0;
	}

	return -1;
}