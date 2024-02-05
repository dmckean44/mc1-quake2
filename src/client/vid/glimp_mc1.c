/*
 * Copyright (C) 2024 Marcus Geelnard
 * Copyright (C) 2010 Yamagi Burmeister
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
 * This is the client side of the MC1 render backend.
 *
 * =======================================================================
 */

#include "../../common/header/common.h"
#include "header/ref.h"

#include <mc1/framebuffer.h>
#include <mc1/mmio.h>

fb_t* s_fb;

/*
 * Shuts the render backend down
 */
static void
ShutdownGraphics(void)
{
	if (s_fb)
	{
		fb_destroy(s_fb);
		s_fb = NULL;
	}
}

// --------

/*
 * Initializes the video subsystem. Must
 * be called before anything else.
 */
qboolean
GLimp_Init(void)
{
	Com_Printf("-------- vid initialization --------\n");

	/* TODO(m): Implement me! */

	Com_Printf("------------------------------------\n\n");

	return true;
}

void
GLimp_Shutdown(void)
{
	ShutdownGraphics();
}

/* (Re)init graphics */
qboolean
GLimp_InitGraphics(int fullscreen, int *pwidth, int *pheight)
{
	int width = *pwidth;
	int height = *pheight;
	int flags;

	re.ShutdownContext();
	ShutdownGraphics();

	/* Probably not necessary for anything */
	flags = re.PrepareForWindow();
	(void)flags;

	/* Set up the MC1 framebuffer */
	s_fb = fb_create(width, height, CMODE_PAL8);
	if (!s_fb)
	{
		Com_Printf("Failed to create framebuffer for %dx%d\n", width, height);
		return false;
	}
	Com_Printf("Framebuffer: %dx%d, 0x%08x (palette 0x%08x)\n", width, height, (unsigned)s_fb->pixels, (unsigned)s_fb->palette);

	if (!re.InitContext((void*)s_fb))
	{
		/* InitContext() should have logged an error. */
		return false;
	}

	/* Use re.GetDrawableSize(&viddef.width, &viddef.height) ? */
	viddef.width = *pwidth;
	viddef.height = *pheight;
	Com_Printf("Drawable size: %ix%i\n", viddef.width, viddef.height);

	return true;
}

void
GLimp_ShutdownGraphics(void)
{
	ShutdownGraphics();
}

void
GLimp_GrabInput(qboolean)
{
}

int
GLimp_GetRefreshRate(void)
{
	return (int)((MMIO(VIDFPS) + 32768U) >> 16);
}

qboolean
GLimp_GetDesktopMode(int *pwidth, int *pheight)
{
	/* TODO(m): Should we calculate an optimal resolution based on the
	   amount of available VRAM? */
	*pwidth = 320;
	*pheight = 180;
	return true;
}

const char**
GLimp_GetDisplayIndices(void)
{
	static const char *displayindices[1] = { NULL };
	return displayindices;
}

int
GLimp_GetNumVideoDisplays(void)
{
	return 1;
}

int
GLimp_GetWindowDisplayIndex(void)
{
	return 0;
}
