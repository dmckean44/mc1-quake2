/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
#include "header/local.h"

static vec3_t r_pright, r_pup, r_ppn;
extern cvar_t	*sw_custom_particles;

#define PARTICLE_33     0
#define PARTICLE_66     1
#define PARTICLE_OPAQUE 2

/*
** R_DrawParticle
**
** Yes, this is amazingly slow, but it's the C reference
** implementation and should be both robust and vaguely
** understandable.  The only time this path should be
** executed is if we're debugging on x86 or if we're
** recompiling and deploying on a non-x86 platform.
**
** To minimize error and improve readability I went the
** function pointer route.  This exacts some overhead, but
** it pays off in clean and easy to understand code.
*/
static void
R_DrawParticle(particle_t *pparticle, int level)
{
	vec3_t		local, transformed;
	float		zi;
	byte		*pdest;
	zbufvalue_t	*pz;
	int		color = pparticle->color;
	int		i, pix, count, u, v;
	int 		custom_particle = (int)sw_custom_particles->value;

	/*
	** transform the particle
	*/
	VectorSubtract (pparticle->origin, r_origin, local);

	transformed[0] = DotProduct(local, r_pright);
	transformed[1] = DotProduct(local, r_pup);
	transformed[2] = DotProduct(local, r_ppn);

	if (transformed[2] < PARTICLE_Z_CLIP)
		return;

	/*
	** project the point
	*/
	// FIXME: preadjust xcenter and ycenter
	zi = 1.0F / transformed[2];
	u = (int)(xcenter + zi * transformed[0] + 0.5F);
	v = (int)(ycenter - zi * transformed[1] + 0.5F);

	if ((v > d_vrectbottom_particle) ||
		(u > d_vrectright_particle) ||
		(v < d_vrecty) ||
		(u < d_vrectx))
	{
		return;
	}

	/*
	** compute addresses of zbuffer, framebuffer, and
	** compute the Z-buffer reference value.
	*/
	pz = d_pzbuffer + (vid_buffer_width * v) + u;
	pdest = d_viewbuffer + vid_buffer_width * v + u;
	const zbufvalue_t izi = (zbufvalue_t)(zi * 0x8000);

	/*
	** determine the screen area covered by the particle,
	** which also means clamping to a min and max
	*/
	pix = (izi * d_pix_mul) >> 7;
	if (pix < d_pix_min)
		pix = d_pix_min;
	else if (pix > d_pix_max)
		pix = d_pix_max;

	/*
	** render the appropriate pixels
	*/
	if (pe_active && custom_particle == 0)
	{
		PE_Begin(PE_PARTICLE, 6);
		PE_Word(u);
		PE_Word(v);
		PE_Word(izi);
		PE_Word(pix);
		PE_Word(color);
		PE_Word(level == PARTICLE_OPAQUE ? PE_PARTICLE_OPAQUE :
			level == PARTICLE_66 ? PE_PARTICLE_66 : PE_PARTICLE_33);
		return;
	}

	count = pix;
	if ((pz[(vid_buffer_width * count / 2) + (count / 2)]) > izi)
	{
		// looks like under some object
		return;
	}

	// zbuffer particles damage
	VID_DamageZBuffer(u, v);
	VID_DamageZBuffer(u + count, v + count);

	if (custom_particle == 0)
	{
		switch (level) {
		case PARTICLE_33 :
			for ( ; count ; count--, pz += vid_buffer_width, pdest += vid_buffer_width)
			{
				//FIXME--do it in blocks of 8?
				for (i=0 ; i<pix ; i++)
				{
					if (pz[i] <= izi)
					{
						pz[i]	= izi;
						pdest[i] = vid_alphamap[color + ((int)pdest[i]<<8)];
					}
				}
			}
			break;

		case PARTICLE_66 :
		{
			int color_part = (color<<8);
			for ( ; count ; count--, pz += vid_buffer_width, pdest += vid_buffer_width)
			{
				for (i=0 ; i<pix ; i++)
				{
					if (pz[i] <= izi)
					{
						pz[i]	= izi;
						pdest[i] = vid_alphamap[color_part + (int)pdest[i]];
					}
				}
			}
			break;
		}

		default:  //100
			for ( ; count ; count--, pz += vid_buffer_width, pdest += vid_buffer_width)
			{
				for (i=0 ; i<pix ; i++)
				{
					if (pz[i] <= izi)
					{
						pz[i]	= izi;
						pdest[i] = color;
					}
				}
			}
			break;
		}
	}
	else
	{
		int min_int, max_int;
		min_int = pix / 2;
		max_int = (pix * 2) - min_int;

		switch (level) {
		case PARTICLE_33 :
			for ( ; count ; count--, pz += vid_buffer_width, pdest += vid_buffer_width)
			{
				//FIXME--do it in blocks of 8?
				for (i=0 ; i<pix ; i++)
				{
					int pos = i + count;
					if (pos >= min_int && pos <= max_int && pz[i] <= izi)
					{
						pz[i]	= izi;
						pdest[i] = vid_alphamap[color + ((int)pdest[i]<<8)];
					}
				}
			}
			break;

		case PARTICLE_66 :
		{
			int color_part = (color<<8);
			for ( ; count ; count--, pz += vid_buffer_width, pdest += vid_buffer_width)
			{
				for (i=0 ; i<pix ; i++)
				{
					int pos = i + count;
					if (pos >= min_int && pos <= max_int && pz[i] <= izi)
					{
						pz[i]	= izi;
						pdest[i] = vid_alphamap[color_part + (int)pdest[i]];
					}
				}
			}
			break;
		}

		default:  //100
			for ( ; count ; count--, pz += vid_buffer_width, pdest += vid_buffer_width)
			{
				for (i=0 ; i<pix ; i++)
				{
					int pos = i + count;
					if (pos >= min_int && pos <= max_int && pz[i] <= izi)
					{
						pz[i]	= izi;
						pdest[i] = color;
					}
				}
			}
			break;
		}
	}
}

/*
** R_DrawParticles
**
** Responsible for drawing all of the particles in the particle list
** throughout the world.  Doesn't care if we're using the C path or
** if we're using the asm path, it simply assigns a function pointer
** and goes.
*/
/*
 * With engine version 13 the particles go out as one batch (PARTICLES): x, y, z and
 * colour | level << 8 per particle, transformed by the engine. Two buffers, each
 * reused once the engine is done with it.
 */
#define PE_MAXPART	4096
static float pe_part[2][PE_MAXPART][4];
static unsigned pe_part_fence[2];
static int pe_part_buf;

static void
R_SendParticles(void)
{
	float (*rec)[4] = pe_part[pe_part_buf];
	const particle_t *p = r_newrefdef.particles;
	int i, n = r_newrefdef.num_particles < PE_MAXPART ? r_newrefdef.num_particles : PE_MAXPART;

	if (!n)
	{
		return;
	}
	PE_WaitFor(pe_part_fence[pe_part_buf]);
	for (i = 0; i < n; i++, p++)
	{
		int level = p->alpha > 0.66F ? PE_PARTICLE_OPAQUE :
			p->alpha > 0.33F ? PE_PARTICLE_66 : PE_PARTICLE_33;

		rec[i][0] = p->origin[0];
		rec[i][1] = p->origin[1];
		rec[i][2] = p->origin[2];
		((int *)rec[i])[3] = (p->color & 0xff) | (level << 8);
	}
	pe_part_fence[pe_part_buf] = PE_Begin(PE_PARTICLES, 25);
	PE_Ptr(rec);
	PE_Word(n);
	for (i = 0; i < 3; i++)
		PE_Float(r_origin[i]);
	for (i = 0; i < 3; i++)
		PE_Float(r_pright[i]);
	for (i = 0; i < 3; i++)
		PE_Float(r_pup[i]);
	for (i = 0; i < 3; i++)
		PE_Float(r_ppn[i]);
	PE_Float(xcenter);
	PE_Float(ycenter);
	PE_Word(d_vrectx);
	PE_Word(d_vrecty);
	PE_Word(d_vrectright_particle);
	PE_Word(d_vrectbottom_particle);
	PE_Word(7);
	PE_Word(d_pix_min);
	PE_Word(d_pix_max);
	PE_Float(PARTICLE_Z_CLIP);
	PE_Word(d_pix_mul);
	pe_part_buf ^= 1;
}

void
R_DrawParticles (void)
{
	particle_t *p;
	int         i;

	if (sw_custom_particles->value)
		PE_Finish();

	VectorScale( vright, xscaleshrink, r_pright );
	VectorScale( vup, yscaleshrink, r_pup );
	VectorCopy( vpn, r_ppn );

	if (pe_active && pe_version >= 13 && !sw_custom_particles->value)
	{
		R_SendParticles();
		return;
	}

	for (p=r_newrefdef.particles, i=0 ; i<r_newrefdef.num_particles ; i++,p++)
	{
		int level;

		if ( p->alpha > 0.66F )
			level = PARTICLE_OPAQUE;
		else if ( p->alpha > 0.33F )
			level = PARTICLE_66;
		else
			level = PARTICLE_33;

		R_DrawParticle(p, level);
	}
}
