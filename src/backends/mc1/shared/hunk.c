/*
 * Copyright (C) 1997-2001 Id Software, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * =======================================================================
 *
 * This file implements the low level part of the Hunk_* memory system
 *
 * =======================================================================
 */

#include <stdlib.h>
#include <sys/time.h>

#include "../../../common/header/common.h"

byte *membase;
size_t maxhunksize;
size_t curhunksize;

static const size_t CACHE_LINE_SIZE = 32;

void *
Hunk_Begin(int maxsize)
{
	/* reserve a huge chunk of memory, but don't commit any yet */
	/* plus some bytes for cacheline */
	maxhunksize = maxsize + sizeof(size_t) + CACHE_LINE_SIZE;
	curhunksize = 0;

	membase = malloc(maxhunksize);
	if (membase == NULL)
	{
		Sys_Error("unable to virtual allocate %d bytes", maxsize);
	}

	*((size_t *)membase) = curhunksize;

	return membase + sizeof(size_t);
}

void *
Hunk_Alloc(int size)
{
	byte *buf;

	/* round to cacheline */
	size = (size + (CACHE_LINE_SIZE - 1)) & ~(CACHE_LINE_SIZE-1);

	if (curhunksize + size > maxhunksize)
	{
		Sys_Error("Hunk_Alloc overflow");
	}

	buf = membase + sizeof(size_t) + curhunksize;
	curhunksize += size;
	return buf;
}

int
Hunk_End(void)
{
	byte *n = NULL;

	// realloc()??
	n = (byte *)realloc(membase, curhunksize + sizeof(size_t));
	if (n == NULL)
	{
		Sys_Error("Hunk_End: Could not realloc hunk");
	}
	membase = n;
	*((size_t *)membase) = curhunksize + sizeof(size_t);

	return curhunksize;
}

void
Hunk_Free(void *base)
{
	if (base)
	{
		byte *m = ((byte *)base) - sizeof(size_t);
		free(m);
	}
}

