/*
 * volume_id - reads filesystem label and uuid
 *
 * Copyright (C) 2004 Kay Sievers <kay.sievers@vrfy.org>
 *
 *	This program is free software; you can redistribute it and/or modify it
 *	under the terms of the GNU General Public License as published by the
 *	Free Software Foundation version 2 of the License.
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

#include "volume_id.h"
#include "logging.h"
#include "util.h"

struct swap_header_v1_2 {
	uint8_t		bootbits[1024];
	uint32_t	version;
	uint32_t	last_page;
	uint32_t	nr_badpages;
	uint8_t		uuid[16];
	uint8_t		volume_name[16];
} __attribute__((__packed__)) *sw;

#define LARGEST_PAGESIZE			0x4000

int volume_id_probe_linux_swap(struct volume_id *id, uint64_t off)
{
	const uint8_t *buf;
	unsigned int page;

	dbg("probing at offset 0x%llx", (unsigned long long) off);

	/* the swap signature is at the end of the PAGE_SIZE */
	for (page = 0x1000; page <= LARGEST_PAGESIZE; page <<= 1) {
			buf = volume_id_get_buffer(id, off + page-10, 10);
			if (buf == NULL)
				return -1;

			if (memcmp(buf, "SWAP-SPACE", 10) == 0) {
				strcpy(id->type_version, "1");
				goto found;
			}

			if (memcmp(buf, "SWAPSPACE2", 10) == 0) {
				sw = (struct swap_header_v1_2 *) volume_id_get_buffer(id, off, sizeof(struct swap_header_v1_2));
				if (sw == NULL)
					return -1;
				strcpy(id->type_version, "2");
				volume_id_set_label_raw(id, sw->volume_name, 16);
				volume_id_set_label_string(id, sw->volume_name, 16);
				volume_id_set_uuid(id, sw->uuid, UUID_DCE);
				goto found;
			}
	}
	return -1;

found:
	volume_id_set_usage(id, VOLUME_ID_OTHER);
	id->type = "swap";

	return 0;
}