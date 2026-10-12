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
// sw_alias.c: routines for setting up to draw alias models

/*
** use a real variable to control lerping
*/
#include <stdint.h>
#include "header/local.h"

#define LIGHT_MIN	5	// lowest light value we'll allow, to avoid the
				//  need for inner-loop light clamping

int				r_amodels_drawn;

affinetridesc_t	r_affinetridesc;

static vec3_t	r_plightvec;
static vec3_t	r_lerp_frontv, r_lerp_backv, r_lerp_move;

static light3_t	r_ambientlight;
int		r_aliasblendcolor;
static vec3_t		r_shadelight;


static daliasframe_t	*r_thisframe, *r_lastframe;
static dmdl_t	*s_pmdl;

static float	aliastransform[3][4];
static float	aliasworldtransform[3][4];
static float	aliasoldworldtransform[3][4];

static float	s_ziscale;
static vec3_t	s_alias_forward, s_alias_right, s_alias_up;


#define NUMVERTEXNORMALS	162

static const float	r_avertexnormals[NUMVERTEXNORMALS][3] = {
#include "../constants/anorms.h"
};


static qboolean r_alias_mesh;	// the engine draws the unclipped triangles (MESH)
static void R_AliasTransformVector(const vec3_t in, vec3_t out, const float xf[3][4]);
static void R_AliasTransformFinalVerts(const entity_t *currententity, int numpoints, finalvert_t *fv, dtrivertx_t *oldv, dtrivertx_t *newv );

void R_AliasProjectAndClipTestFinalVert(finalvert_t *fv);
#if defined(__MRISC32_VECTOR_OPS__)
static void R_AliasTransformFinalVertsVec(int numpoints, finalvert_t *fv, const dtrivertx_t *oldv,
	const dtrivertx_t *newv);
#endif


/*
================
R_AliasCheckBBox
================
*/

#define BBOX_TRIVIAL_ACCEPT 0
#define BBOX_MUST_CLIP_XY   1
#define BBOX_MUST_CLIP_Z    2
#define BBOX_TRIVIAL_REJECT 8

/*
** R_AliasCheckFrameBBox
**
** Checks a specific alias frame bounding box
*/
static unsigned long
R_AliasCheckFrameBBox( daliasframe_t *frame, float worldxf[3][4] )
{
	// FIXME: should this really be using long and not int32_t or sth?
	unsigned long aggregate_and_clipcode = ~0U,
		          aggregate_or_clipcode = 0;
	int           i;
	vec3_t        mins, maxs;
	vec3_t        transformed_min, transformed_max;
	qboolean      zfullyclipped = true;

	/*
	** get the exact frame bounding box
	*/
	for (i=0 ; i<3 ; i++)
	{
		mins[i] = frame->translate[i];
		maxs[i] = mins[i] + frame->scale[i]*255;
	}

	/*
	** transform the min and max values into view space
	*/
	R_AliasTransformVector( mins, transformed_min, aliastransform );
	R_AliasTransformVector( maxs, transformed_max, aliastransform );

	if ( transformed_min[2] >= ALIAS_Z_CLIP_PLANE )
		zfullyclipped = false;
	if ( transformed_max[2] >= ALIAS_Z_CLIP_PLANE )
		zfullyclipped = false;

	if ( zfullyclipped )
	{
		return BBOX_TRIVIAL_REJECT;
	}

	/*
	** build a transformed bounding box from the given min and max
	*/
	for ( i = 0; i < 8; i++ )
	{
		int      j;
		vec3_t   tmp, transformed;
		unsigned long clipcode = 0;

		if ( i & 1 )
			tmp[0] = mins[0];
		else
			tmp[0] = maxs[0];

		if ( i & 2 )
			tmp[1] = mins[1];
		else
			tmp[1] = maxs[1];

		if ( i & 4 )
			tmp[2] = mins[2];
		else
			tmp[2] = maxs[2];

		R_AliasTransformVector( tmp, transformed, worldxf );

		for ( j = 0; j < 4; j++ )
		{
			float dp = DotProduct( transformed, view_clipplanes[j].normal );

			if ( ( dp - view_clipplanes[j].dist ) < 0.0F )
				clipcode |= 1 << j;
		}

		aggregate_and_clipcode &= clipcode;
		aggregate_or_clipcode  |= clipcode;
	}

	if ( aggregate_and_clipcode )
	{
		return BBOX_TRIVIAL_REJECT;
	}
	if ( !aggregate_or_clipcode )
	{
		return BBOX_TRIVIAL_ACCEPT;
	}

	return BBOX_MUST_CLIP_XY;
}

static int
R_AliasCheckBBox (const entity_t *currententity)
{
	unsigned long ccodes[2] = { 0, 0 };

	ccodes[0] = R_AliasCheckFrameBBox( r_thisframe, aliasworldtransform );

	/*
	** non-lerping model
	*/
	if ( currententity->backlerp == 0 )
	{
		if ( ccodes[0] == BBOX_TRIVIAL_ACCEPT )
			return BBOX_TRIVIAL_ACCEPT;
		else if ( ccodes[0] & BBOX_TRIVIAL_REJECT )
			return BBOX_TRIVIAL_REJECT;
		else
			return ( ccodes[0] & ~BBOX_TRIVIAL_REJECT );
	}

	ccodes[1] = R_AliasCheckFrameBBox( r_lastframe, aliasoldworldtransform );

	if ( ( ccodes[0] | ccodes[1] ) == BBOX_TRIVIAL_ACCEPT )
		return BBOX_TRIVIAL_ACCEPT;
	else if ( ( ccodes[0] & ccodes[1] ) & BBOX_TRIVIAL_REJECT )
		return BBOX_TRIVIAL_REJECT;
	else
		return ( ccodes[0] | ccodes[1] ) & ~BBOX_TRIVIAL_REJECT;
}


/*
================
R_AliasTransformVector
================
*/
static void
R_AliasTransformVector(const vec3_t in, vec3_t out, const float xf[3][4] )
{
	out[0] = DotProduct(in, xf[0]) + xf[0][3];
	out[1] = DotProduct(in, xf[1]) + xf[1][3];
	out[2] = DotProduct(in, xf[2]) + xf[2][3];
}


/*
================
R_AliasPreparePoints

General clipped case
================
*/

/*
 * Blended vertices lie in the box from r_lerp_move to r_lerp_move + 255 *
 * (backv + frontv). If its corners are in front of the z clip plane and inside
 * the clip rectangle (the guard band, when the engine draws the model), no
 * vertex gets a clip flag.
 */
static qboolean
R_AliasLerpBoxUnclipped(void)
{
	int i, k;

	for (i = 0; i < 8; i++)
	{
		vec3_t c;
		float x, y, z, zi, u, v;

		for (k = 0; k < 3; k++)
		{
			c[k] = r_lerp_move[k] + ((i >> k) & 1 ? 255.0F * (r_lerp_backv[k] + r_lerp_frontv[k]) : 0.0F);
		}
		x = DotProduct(c, aliastransform[0]) + aliastransform[0][3];
		y = DotProduct(c, aliastransform[1]) + aliastransform[1][3];
		z = DotProduct(c, aliastransform[2]) + aliastransform[2][3];
		if (z < ALIAS_Z_CLIP_PLANE)
		{
			return false;
		}
		zi = 1.0F / z;
		u = (x * aliasxscale * zi) + aliasxcenter;
		v = (y * aliasyscale * zi) + aliasycenter;
		if (u < r_refdef.aliasvrect.x || v < r_refdef.aliasvrect.y ||
			u > r_refdef.aliasvrectright || v > r_refdef.aliasvrectbottom)
		{
			return false;
		}
	}
	return true;
}

/* the engine blends, transforms, lights and projects the vertices (version 9) */
static void
PE_AliasVerts2(finalvert_t *verts)
{
	int i, j;

	PE_Begin(PE_VERTS2, 36);
	PE_Ptr(r_lastframe->verts);
	PE_Ptr(r_thisframe->verts);
	PE_Word(s_pmdl->num_xyz);
	PE_Ptr(verts);
	for (i = 0; i < 3; i++)
	{
		for (j = 0; j < 4; j++)
		{
			PE_Float(aliastransform[i][j]);
		}
	}
	for (i = 0; i < 3; i++)
	{
		PE_Float(r_lerp_move[i]);
	}
	for (i = 0; i < 3; i++)
	{
		PE_Float(r_lerp_backv[i]);
	}
	for (i = 0; i < 3; i++)
	{
		PE_Float(r_lerp_frontv[i]);
	}
	PE_Float(aliasxscale);
	PE_Float(aliasyscale);
	PE_Float(aliasxcenter);
	PE_Float(aliasycenter);
	PE_Float(s_ziscale);
	PE_Ptr(r_avertexnormals);
	PE_Float(r_plightvec[0]);
	PE_Float(r_plightvec[1]);
	PE_Float(r_plightvec[2]);
	PE_Word(r_ambientlight[0]);
	PE_Float(r_shadelight[0]);
}

static void
R_AliasPreparePoints (const entity_t *currententity, finalvert_t *verts, const finalvert_t *verts_max)
{
	int		i;
	dstvert_t	*pstverts;
	dtriangle_t	*ptri;
	finalvert_t	*pfv[3];

	if ((verts + s_pmdl->num_xyz) >= verts_max)
	{
		r_outofverts = true;
		return;
	}

	if (r_alias_mesh && pe_world && pe_version >= 9 &&
		!(currententity->flags & (RF_SHELL_RED | RF_SHELL_GREEN | RF_SHELL_BLUE | RF_SHELL_DOUBLE | RF_SHELL_HALF_DAM)) &&
		R_AliasLerpBoxUnclipped())
	{
		PE_AliasVerts2(verts);
		PE_DrawMesh(verts, (dtriangle_t *)((byte *)s_pmdl + s_pmdl->ofs_tris), s_pmdl->num_tris,
			(dstvert_t *)((byte *)s_pmdl + s_pmdl->ofs_st));
		return;
	}

#if defined(__MRISC32_VECTOR_OPS__) && !defined(ENABLE_SW_COLORLIGHT)
	if (s_pmdl->num_xyz > 0 && !(currententity->flags &
		(RF_SHELL_RED | RF_SHELL_GREEN | RF_SHELL_BLUE | RF_SHELL_DOUBLE | RF_SHELL_HALF_DAM)))
	{
		R_AliasTransformFinalVertsVec(s_pmdl->num_xyz, verts, r_lastframe->verts, r_thisframe->verts);
	}
	else
#endif
	R_AliasTransformFinalVerts(currententity,
				   s_pmdl->num_xyz,
				   verts,	// destination for transformed verts
				   r_lastframe->verts,	// verts from the last frame
				   r_thisframe->verts	// verts from this frame
				);

	// clip and draw all triangles
	//
	pstverts = (dstvert_t *)((byte *)s_pmdl + s_pmdl->ofs_st);
	ptri = (dtriangle_t *)((byte *)s_pmdl + s_pmdl->ofs_tris);

	if (r_alias_mesh)
	{
		// the engine draws the unclipped triangles, the CPU clips the others
		PE_DrawMesh(verts, ptri, s_pmdl->num_tris, pstverts);
		for (i=0 ; i<s_pmdl->num_tris ; i++, ptri++)
		{
			pfv[0] = &verts[ptri->index_xyz[0]];
			pfv[1] = &verts[ptri->index_xyz[1]];
			pfv[2] = &verts[ptri->index_xyz[2]];

			if ( !(pfv[0]->flags | pfv[1]->flags | pfv[2]->flags) ||
				( pfv[0]->flags & pfv[1]->flags & pfv[2]->flags ) )
				continue;

			pfv[0]->cv.s = pstverts[ptri->index_st[0]].s << SHIFT16XYZ;
			pfv[0]->cv.t = pstverts[ptri->index_st[0]].t << SHIFT16XYZ;
			pfv[1]->cv.s = pstverts[ptri->index_st[1]].s << SHIFT16XYZ;
			pfv[1]->cv.t = pstverts[ptri->index_st[1]].t << SHIFT16XYZ;
			pfv[2]->cv.s = pstverts[ptri->index_st[2]].s << SHIFT16XYZ;
			pfv[2]->cv.t = pstverts[ptri->index_st[2]].t << SHIFT16XYZ;

			R_AliasClipTriangle(currententity, pfv[0], pfv[1], pfv[2]);
		}
		return;
	}

	if ( ( currententity->flags & RF_WEAPONMODEL ) && ( r_lefthand->value == 1.0F ) )
	{
		for (i=0 ; i<s_pmdl->num_tris ; i++, ptri++)
		{
			pfv[0] = &verts[ptri->index_xyz[0]];
			pfv[1] = &verts[ptri->index_xyz[1]];
			pfv[2] = &verts[ptri->index_xyz[2]];

			if ( pfv[0]->flags & pfv[1]->flags & pfv[2]->flags )
				continue;	// completely clipped

			// insert s/t coordinates
			pfv[0]->cv.s = pstverts[ptri->index_st[0]].s << SHIFT16XYZ;
			pfv[0]->cv.t = pstverts[ptri->index_st[0]].t << SHIFT16XYZ;

			pfv[1]->cv.s = pstverts[ptri->index_st[1]].s << SHIFT16XYZ;
			pfv[1]->cv.t = pstverts[ptri->index_st[1]].t << SHIFT16XYZ;

			pfv[2]->cv.s = pstverts[ptri->index_st[2]].s << SHIFT16XYZ;
			pfv[2]->cv.t = pstverts[ptri->index_st[2]].t << SHIFT16XYZ;

			if ( ! (pfv[0]->flags | pfv[1]->flags | pfv[2]->flags) )
			{
				// totally unclipped
				R_DrawTriangle(currententity, pfv[2], pfv[1], pfv[0]);
			}
			else
			{
				R_AliasClipTriangle(currententity, pfv[2], pfv[1], pfv[0]);
			}
		}
	}
	else
	{
		for (i=0 ; i<s_pmdl->num_tris ; i++, ptri++)
		{
			pfv[0] = &verts[ptri->index_xyz[0]];
			pfv[1] = &verts[ptri->index_xyz[1]];
			pfv[2] = &verts[ptri->index_xyz[2]];

			if ( pfv[0]->flags & pfv[1]->flags & pfv[2]->flags )
				continue;	// completely clipped

			// insert s/t coordinates
			pfv[0]->cv.s = pstverts[ptri->index_st[0]].s << SHIFT16XYZ;
			pfv[0]->cv.t = pstverts[ptri->index_st[0]].t << SHIFT16XYZ;

			pfv[1]->cv.s = pstverts[ptri->index_st[1]].s << SHIFT16XYZ;
			pfv[1]->cv.t = pstverts[ptri->index_st[1]].t << SHIFT16XYZ;

			pfv[2]->cv.s = pstverts[ptri->index_st[2]].s << SHIFT16XYZ;
			pfv[2]->cv.t = pstverts[ptri->index_st[2]].t << SHIFT16XYZ;

			if ( ! (pfv[0]->flags | pfv[1]->flags | pfv[2]->flags) )
			{
				// totally unclipped
				R_DrawTriangle(currententity, pfv[0], pfv[1], pfv[2]);
			}
			else
			{	// partially clipped
				R_AliasClipTriangle(currententity, pfv[0], pfv[1], pfv[2]);
			}
		}
	}
}


/*
================
R_AliasSetUpTransform
================
*/
static void
R_AliasSetUpTransform(const entity_t *currententity)
{
	int				i;
	static float	viewmatrix[3][4];

	// TODO: should really be stored with the entity instead of being reconstructed
	// TODO: should use a look-up table
	// TODO: could cache lazily, stored in the entity
	//
	// AngleVectors never change angles, we can convert from const
	AngleVectors((float *)currententity->angles, s_alias_forward, s_alias_right, s_alias_up );

	// TODO: can do this with simple matrix rearrangement
	memset( aliasworldtransform, 0, sizeof( aliasworldtransform ) );
	memset( aliasoldworldtransform, 0, sizeof( aliasworldtransform ) );

	for (i=0 ; i<3 ; i++)
	{
		aliasoldworldtransform[i][0] = aliasworldtransform[i][0] =  s_alias_forward[i];
		aliasoldworldtransform[i][0] = aliasworldtransform[i][1] = -s_alias_right[i];
		aliasoldworldtransform[i][0] = aliasworldtransform[i][2] =  s_alias_up[i];
	}

	aliasworldtransform[0][3] = currententity->origin[0]-r_origin[0];
	aliasworldtransform[1][3] = currententity->origin[1]-r_origin[1];
	aliasworldtransform[2][3] = currententity->origin[2]-r_origin[2];

	aliasoldworldtransform[0][3] = currententity->oldorigin[0]-r_origin[0];
	aliasoldworldtransform[1][3] = currententity->oldorigin[1]-r_origin[1];
	aliasoldworldtransform[2][3] = currententity->oldorigin[2]-r_origin[2];

	// FIXME: can do more efficiently than full concatenation
	// memcpy( rotationmatrix, t2matrix, sizeof( rotationmatrix ) );

	// R_ConcatTransforms (t2matrix, tmatrix, rotationmatrix);

	// TODO: should be global, set when vright, etc., set
	VectorCopy (vright, viewmatrix[0]);
	VectorCopy (vup, viewmatrix[1]);
	VectorInverse (viewmatrix[1]);
	VectorCopy (vpn, viewmatrix[2]);

	if ( currententity->flags & RF_WEAPONMODEL )
	{
		viewmatrix[0][3] = 0;
		viewmatrix[1][3] = 0;
		viewmatrix[2][3] = sw_gunzposition->value;
	}
	else
	{
		viewmatrix[0][3] = 0;
		viewmatrix[1][3] = 0;
		viewmatrix[2][3] = 0;
	}

	// memcpy( aliasworldtransform, rotationmatrix, sizeof( aliastransform ) );

	R_ConcatTransforms (viewmatrix, aliasworldtransform, aliastransform);

	aliasworldtransform[0][3] = currententity->origin[0];
	aliasworldtransform[1][3] = currententity->origin[1];
	aliasworldtransform[2][3] = currententity->origin[2];

	aliasoldworldtransform[0][3] = currententity->oldorigin[0];
	aliasoldworldtransform[1][3] = currententity->oldorigin[1];
	aliasoldworldtransform[2][3] = currententity->oldorigin[2];
}


/*
================
R_AliasTransformFinalVerts
================
*/
static void
R_AliasTransformFinalVerts(const entity_t *currententity, int numpoints, finalvert_t *fv, dtrivertx_t *oldv, dtrivertx_t *newv )
{
	int i;

	for ( i = 0; i < numpoints; i++, fv++, oldv++, newv++ )
	{
		float	lightcos;
		const float	*plightnormal;
		vec3_t  lerped_vert;

		lerped_vert[0] = r_lerp_move[0] + oldv->v[0]*r_lerp_backv[0] + newv->v[0]*r_lerp_frontv[0];
		lerped_vert[1] = r_lerp_move[1] + oldv->v[1]*r_lerp_backv[1] + newv->v[1]*r_lerp_frontv[1];
		lerped_vert[2] = r_lerp_move[2] + oldv->v[2]*r_lerp_backv[2] + newv->v[2]*r_lerp_frontv[2];

		plightnormal = r_avertexnormals[newv->lightnormalindex];

		// added double damage shell
		if ( currententity->flags & ( RF_SHELL_RED | RF_SHELL_GREEN | RF_SHELL_BLUE | RF_SHELL_DOUBLE | RF_SHELL_HALF_DAM) )
		{
			lerped_vert[0] += plightnormal[0] * POWERSUIT_SCALE;
			lerped_vert[1] += plightnormal[1] * POWERSUIT_SCALE;
			lerped_vert[2] += plightnormal[2] * POWERSUIT_SCALE;
		}

		fv->xyz[0] = DotProduct(lerped_vert, aliastransform[0]) + aliastransform[0][3];
		fv->xyz[1] = DotProduct(lerped_vert, aliastransform[1]) + aliastransform[1][3];
		fv->xyz[2] = DotProduct(lerped_vert, aliastransform[2]) + aliastransform[2][3];

		fv->flags = 0;

		// lighting
		lightcos = DotProduct (plightnormal, r_plightvec);

		if (lightcos < 0)
		{
			int		j;

			for(j=0; j<3; j++)
			{
				int temp;

				temp = r_ambientlight[j];

				temp += (r_shadelight[j] * lightcos);

				// clamp; because we limited the minimum ambient and shading light, we
				// don't have to clamp low light, just bright
				if (temp < 0)
					temp = 0;

				fv->cv.l[j] = temp;
			}
		}
		else
			memcpy(fv->cv.l, r_ambientlight, sizeof(light3_t));

		if ( fv->xyz[2] < ALIAS_Z_CLIP_PLANE )
		{
			fv->flags |= ALIAS_Z_CLIP;
		}
		else
		{
			R_AliasProjectAndClipTestFinalVert( fv );
		}
	}

}

#if defined(__MRISC32_VECTOR_OPS__)
/*
 * The same arithmetic as R_AliasTransformFinalVerts and
 * R_AliasProjectAndClipTestFinalVert, VL vertices at a time. Only l[0] of the
 * light is used without colour light, so it is stored in all three. Vertices
 * behind the z clip plane get projected too; those values are never read.
 */
static void
R_AliasTransformFinalVertsVec(int numpoints, finalvert_t *fv, const dtrivertx_t *oldv,
	const dtrivertx_t *newv)
{
	const float k1[21] = {
		r_lerp_move[0], r_lerp_move[1], r_lerp_move[2],
		r_lerp_backv[0], r_lerp_backv[1], r_lerp_backv[2],
		r_lerp_frontv[0], r_lerp_frontv[1], r_lerp_frontv[2],
		aliastransform[0][0], aliastransform[0][1], aliastransform[0][2], aliastransform[0][3],
		aliastransform[1][0], aliastransform[1][1], aliastransform[1][2], aliastransform[1][3],
		aliastransform[2][0], aliastransform[2][1], aliastransform[2][2], aliastransform[2][3]
	};
	union { float f; int i; const void *p; } k2[18];
	int n;
	const dtrivertx_t *o, *nv;
	finalvert_t *f;

	n = numpoints; o = oldv; nv = newv; f = fv;
	__asm__ volatile(
		"    getsr   vl, #0x10\n"
		"    ldw     r9, [%[k], #0]\n   mov v9, r9\n"
		"    ldw     r9, [%[k], #4]\n   mov v10, r9\n"
		"    ldw     r9, [%[k], #8]\n   mov v11, r9\n"
		"    ldw     r9, [%[k], #12]\n  mov v12, r9\n"
		"    ldw     r9, [%[k], #16]\n  mov v13, r9\n"
		"    ldw     r9, [%[k], #20]\n  mov v14, r9\n"
		"    ldw     r9, [%[k], #24]\n  mov v15, r9\n"
		"    ldw     r9, [%[k], #28]\n  mov v16, r9\n"
		"    ldw     r9, [%[k], #32]\n  mov v17, r9\n"
		"    ldw     r9, [%[k], #36]\n  mov v18, r9\n"
		"    ldw     r9, [%[k], #40]\n  mov v19, r9\n"
		"    ldw     r9, [%[k], #44]\n  mov v20, r9\n"
		"    ldw     r9, [%[k], #48]\n  mov v21, r9\n"
		"    ldw     r9, [%[k], #52]\n  mov v22, r9\n"
		"    ldw     r9, [%[k], #56]\n  mov v23, r9\n"
		"    ldw     r9, [%[k], #60]\n  mov v24, r9\n"
		"    ldw     r9, [%[k], #64]\n  mov v25, r9\n"
		"    ldw     r9, [%[k], #68]\n  mov v26, r9\n"
		"    ldw     r9, [%[k], #72]\n  mov v27, r9\n"
		"    ldw     r9, [%[k], #76]\n  mov v28, r9\n"
		"    ldw     r9, [%[k], #80]\n  mov v29, r9\n"
		"1:\n"
		"    min     vl, vl, %[n]\n"
		"    sub     %[n], %[n], vl\n"
		"    ldub    v1, [%[o], #4]\n"
		"    add     r9, %[o], #1\n"
		"    ldub    v2, [r9, #4]\n"
		"    add     r9, %[o], #2\n"
		"    ldub    v3, [r9, #4]\n"
		"    ldub    v4, [%[nv], #4]\n"
		"    add     r9, %[nv], #1\n"
		"    ldub    v5, [r9, #4]\n"
		"    add     r9, %[nv], #2\n"
		"    ldub    v6, [r9, #4]\n"
		"    itof    v1, v1, z\n"
		"    itof    v2, v2, z\n"
		"    itof    v3, v3, z\n"
		"    itof    v4, v4, z\n"
		"    itof    v5, v5, z\n"
		"    itof    v6, v6, z\n"
		"    fmul    v1, v1, v12\n"
		"    fadd    v1, v9, v1\n"
		"    fmul    v4, v4, v15\n"
		"    fadd    v1, v1, v4\n"
		"    fmul    v2, v2, v13\n"
		"    fadd    v2, v10, v2\n"
		"    fmul    v5, v5, v16\n"
		"    fadd    v2, v2, v5\n"
		"    fmul    v3, v3, v14\n"
		"    fadd    v3, v11, v3\n"
		"    fmul    v6, v6, v17\n"
		"    fadd    v3, v3, v6\n"
		"    fmul    v4, v1, v18\n"
		"    fmul    v5, v2, v19\n"
		"    fadd    v4, v4, v5\n"
		"    fmul    v5, v3, v20\n"
		"    fadd    v4, v4, v5\n"
		"    fadd    v4, v4, v21\n"
		"    add     r9, %[f], #36\n"
		"    stw     v4, [r9, #48]\n"
		"    fmul    v5, v1, v22\n"
		"    fmul    v6, v2, v23\n"
		"    fadd    v5, v5, v6\n"
		"    fmul    v6, v3, v24\n"
		"    fadd    v5, v5, v6\n"
		"    fadd    v5, v5, v25\n"
		"    add     r9, %[f], #40\n"
		"    stw     v5, [r9, #48]\n"
		"    fmul    v6, v1, v26\n"
		"    fmul    v7, v2, v27\n"
		"    fadd    v6, v6, v7\n"
		"    fmul    v7, v3, v28\n"
		"    fadd    v6, v6, v7\n"
		"    fadd    v6, v6, v29\n"
		"    add     r9, %[f], #44\n"
		"    stw     v6, [r9, #48]\n"
		"    ldea    %[o], [%[o], vl*4]\n"
		"    ldea    %[nv], [%[nv], vl*4]\n"
		"    mul     r9, vl, #48\n"
		"    add     %[f], %[f], r9\n"
		"    bnz     %[n], 1b\n"
		: [n] "+r"(n), [o] "+r"(o), [nv] "+r"(nv), [f] "+r"(f)
		: [k] "r"(k1)
		: "vl", "r9", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v9", "v10", "v11", "v12", "v13",
		  "v14", "v15", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25",
		  "v26", "v27", "v28", "v29", "memory"
	);

	k2[0].f = r_plightvec[0];
	k2[1].f = r_plightvec[1];
	k2[2].f = r_plightvec[2];
	k2[3].f = r_shadelight[0];
	k2[4].f = (float)r_ambientlight[0];
	k2[5].i = r_ambientlight[0];
	k2[6].f = aliasxscale;
	k2[7].f = aliasyscale;
	k2[8].f = aliasxcenter;
	k2[9].f = aliasycenter;
	k2[10].f = s_ziscale;
	k2[11].i = r_refdef.aliasvrect.x;
	k2[12].i = r_refdef.aliasvrect.y;
	k2[13].i = r_refdef.aliasvrectright;
	k2[14].i = r_refdef.aliasvrectbottom;
	k2[15].f = ALIAS_Z_CLIP_PLANE;
	k2[16].f = 1.0F;
	k2[17].p = r_avertexnormals;

	n = numpoints; nv = newv; f = fv;
	__asm__ volatile(
		"    getsr   vl, #0x10\n"
		"    ldw     r9, [%[k], #0]\n"
		"    ldw     r10, [%[k], #4]\n"
		"    ldw     r11, [%[k], #8]\n"
		"    ldw     r12, [%[k], #12]\n"
		"    ldw     r13, [%[k], #16]\n"
		"    ldw     r14, [%[k], #20]\n"
		"    ldw     r15, [%[k], #24]\n"
		"    ldw     r16, [%[k], #28]\n"
		"    ldw     r17, [%[k], #32]\n"
		"    ldw     r18, [%[k], #36]\n"
		"    ldw     r19, [%[k], #40]\n"
		"    ldw     r20, [%[k], #44]\n"
		"    ldw     r21, [%[k], #48]\n"
		"    ldw     r22, [%[k], #52]\n"
		"    ldw     r23, [%[k], #56]\n"
		"    ldw     r24, [%[k], #60]\n"
		"    ldw     r25, [%[k], #64]\n"
		"    mov     v20, r25\n"
		"    ldw     r25, [%[k], #68]\n"
		"1:\n"
		"    min     vl, vl, %[n]\n"
		"    sub     %[n], %[n], vl\n"
		"    add     r26, %[nv], #3\n"
		"    ldub    v1, [r26, #4]\n"
		"    mul     v1, v1, #12\n"
		"    ldw     v2, [r25, v1]\n"
		"    add     r26, r25, #4\n"
		"    ldw     v3, [r26, v1]\n"
		"    add     r26, r25, #8\n"
		"    ldw     v4, [r26, v1]\n"
		"    fmul    v2, v2, r9\n"
		"    fmul    v3, v3, r10\n"
		"    fadd    v2, v2, v3\n"
		"    fmul    v4, v4, r11\n"
		"    fadd    v2, v2, v4\n"
		"    fslt    v3, v2, z\n"
		"    fmul    v4, v2, r12\n"
		"    fadd    v4, v4, r13\n"
		"    ftoi    v4, v4, z\n"
		"    max     v4, v4, z\n"
		"    sel     v3, v4, r14\n"
		"    add     r26, %[f], #16\n"
		"    stw     v3, [r26, #48]\n"
		"    add     r26, %[f], #20\n"
		"    stw     v3, [r26, #48]\n"
		"    add     r26, %[f], #24\n"
		"    stw     v3, [r26, #48]\n"
		"    add     r26, %[f], #36\n"
		"    ldw     v5, [r26, #48]\n"
		"    add     r26, %[f], #40\n"
		"    ldw     v6, [r26, #48]\n"
		"    add     r26, %[f], #44\n"
		"    ldw     v7, [r26, #48]\n"
		"    fslt    v8, v7, r24\n"
		"    fdiv    v9, v20, v7\n"
		"    fmul    v10, v9, r19\n"
		"    ftoi    v10, v10, z\n"
		"    add     r26, %[f], #28\n"
		"    stw     v10, [r26, #48]\n"
		"    fmul    v11, v5, r15\n"
		"    fmul    v11, v11, v9\n"
		"    fadd    v11, v11, r17\n"
		"    ftoi    v11, v11, z\n"
		"    stw     v11, [%[f], #48]\n"
		"    fmul    v12, v6, r16\n"
		"    fmul    v12, v12, v9\n"
		"    fadd    v12, v12, r18\n"
		"    ftoi    v12, v12, z\n"
		"    add     r26, %[f], #4\n"
		"    stw     v12, [r26, #48]\n"
		"    slt     v13, v11, r20\n"
		"    and     v13, v13, #1\n"
		"    slt     v14, v12, r21\n"
		"    and     v14, v14, #2\n"
		"    or      v13, v13, v14\n"
		"    sle     v14, v11, r22\n"
		"    xor     v14, v14, #-1\n"
		"    and     v14, v14, #4\n"
		"    or      v13, v13, v14\n"
		"    sle     v14, v12, r23\n"
		"    xor     v14, v14, #-1\n"
		"    and     v14, v14, #8\n"
		"    or      v13, v13, v14\n"
		"    and     v14, v8, #16\n"
		"    sel     v8, v14, v13\n"
		"    add     r26, %[f], #32\n"
		"    stw     v8, [r26, #48]\n"
		"    ldea    %[nv], [%[nv], vl*4]\n"
		"    mul     r26, vl, #48\n"
		"    add     %[f], %[f], r26\n"
		"    bnz     %[n], 1b\n"
		: [n] "+r"(n), [nv] "+r"(nv), [f] "+r"(f)
		: [k] "r"(k2)
		: "vl", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "r16", "r17", "r18", "r19", "r20",
		  "r21", "r22", "r23", "r24", "r25", "r26", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8",
		  "v9", "v10", "v11", "v12", "v13", "v14", "v20", "memory"
	);
}
#endif

/*
================
R_AliasProjectAndClipTestFinalVert
================
*/
void
R_AliasProjectAndClipTestFinalVert( finalvert_t *fv )
{
	float	zi;
	float	x, y, z;

	// project points
	x = fv->xyz[0];
	y = fv->xyz[1];
	z = fv->xyz[2];
	zi = 1.0F / z;

	fv->cv.zi = zi * s_ziscale;

	fv->cv.u = (x * aliasxscale * zi) + aliasxcenter;
	fv->cv.v = (y * aliasyscale * zi) + aliasycenter;

	if (fv->cv.u < r_refdef.aliasvrect.x)
		fv->flags |= ALIAS_LEFT_CLIP;
	if (fv->cv.v < r_refdef.aliasvrect.y)
		fv->flags |= ALIAS_TOP_CLIP;
	if (fv->cv.u > r_refdef.aliasvrectright)
		fv->flags |= ALIAS_RIGHT_CLIP;
	if (fv->cv.v > r_refdef.aliasvrectbottom)
		fv->flags |= ALIAS_BOTTOM_CLIP;
}

/*
===============
R_AliasSetupSkin
===============
*/
static qboolean
R_AliasSetupSkin(const entity_t *currententity, const model_t *currentmodel)
{
	image_t *pskindesc;

	if (currententity->skin)
	{
		pskindesc = currententity->skin;
	}
	else
	{
		int skinnum;

		skinnum = currententity->skinnum;
		if ((skinnum >= s_pmdl->num_skins) || (skinnum < 0))
		{
			R_Printf(PRINT_ALL, "%s %s: no such skin # %d\n",
				__func__, currentmodel->name, skinnum);
			skinnum = 0;
		}

		pskindesc = currentmodel->skins[skinnum];
	}

	if ( !pskindesc )
		return false;

	r_affinetridesc.pskin = pskindesc->pixels[0];
	r_affinetridesc.skinwidth = pskindesc->width;
	r_affinetridesc.skinheight = pskindesc->height;

	R_PolysetUpdateTables (); // FIXME: precalc edge lookups

	return true;
}


/*
================
R_AliasSetupLighting

  FIXME: put lighting into tables
================
*/
static void
R_AliasSetupLighting(entity_t *currententity)
{
	const vec3_t lightvec = {-1, 0, 0};
	vec3_t light;
	int i;

	// all components of light should be identical in software
	if ( currententity->flags & RF_FULLBRIGHT )
	{
		for (i=0 ; i<3 ; i++)
			light[i] = 1.0F;
	}
	else
	{
		R_LightPoint (currententity, currententity->origin, light);
	}

	// save off light value for server to look at (BIG HACK!)
	if ( currententity->flags & RF_WEAPONMODEL )
		r_lightlevel->value = 150.0F * light[0];


	if ( currententity->flags & RF_MINLIGHT )
	{
		for (i=0 ; i<3 ; i++)
			if (light[i] < 0.1F)
				light[i] = 0.1F;
	}

	if ( currententity->flags & RF_GLOW )
	{	// bonus items will pulse with time
		float	scale;

		scale = 0.1F * sinf(r_newrefdef.time*7);
		for (i=0 ; i<3 ; i++)
		{
			float min;

			min = light[i] * 0.8F;
			light[i] += scale;
			if (light[i] < min)
				light[i] = min;
		}
	}

	if(r_colorlight->value == 0)
	{
		float temp = (light[0] + light[1] + light[2]) * (1.0F / 3.0F);

		light[0] = light[1] = light[2] = temp;
	}

	for(i=0; i<3; i++)
	{
		int j;

		j = light[i] * 255;

		r_ambientlight[i] = j;
		r_shadelight[i] = j;

		// clamp lighting so it doesn't overbright as much
		if (r_ambientlight[i] > 128)
			r_ambientlight[i] = 128;
		if (r_ambientlight[i] + r_shadelight[i] > 192)
			r_shadelight[i] = 192 - r_ambientlight[i];

		// guarantee that no vertex will ever be lit below LIGHT_MIN, so we don't have
		// to clamp off the bottom
		if (r_ambientlight[i] < LIGHT_MIN)
			r_ambientlight[i] = LIGHT_MIN;

		r_ambientlight[i] = (255 - r_ambientlight[i]) << VID_CBITS;

		if (r_shadelight[i] < 0)
			r_shadelight[i] = 0;

		r_shadelight[i] *= VID_GRADES;
	}

	// rotate the lighting vector into the model's frame of reference
	r_plightvec[0] =  DotProduct( lightvec, s_alias_forward );
	r_plightvec[1] = -DotProduct( lightvec, s_alias_right );
	r_plightvec[2] =  DotProduct( lightvec, s_alias_up );
}


/*
=================
R_AliasSetupFrames

=================
*/
static void
R_AliasSetupFrames(const entity_t *currententity, const model_t *currentmodel, dmdl_t *pmdl)
{
	int thisframe = currententity->frame;
	int lastframe = currententity->oldframe;

	if ( ( thisframe >= pmdl->num_frames ) || ( thisframe < 0 ) )
	{
		R_Printf(PRINT_ALL, "%s %s: no such thisframe %d\n",
			__func__, currentmodel->name, thisframe);
		thisframe = 0;
	}
	if ( ( lastframe >= pmdl->num_frames ) || ( lastframe < 0 ) )
	{
		R_Printf(PRINT_ALL, "%s %s: no such lastframe %d\n",
			__func__, currentmodel->name, lastframe);
		lastframe = 0;
	}

	r_thisframe = (daliasframe_t *)((byte *)pmdl + pmdl->ofs_frames
		+ thisframe * pmdl->framesize);

	r_lastframe = (daliasframe_t *)((byte *)pmdl + pmdl->ofs_frames
		+ lastframe * pmdl->framesize);
}

/*
** R_AliasSetUpLerpData
**
** Precomputes lerp coefficients used for the whole frame.
*/
static void
R_AliasSetUpLerpData(entity_t *currententity, dmdl_t *pmdl, float backlerp)
{
	float	frontlerp;
	vec3_t	translation, vectors[3];
	int		i;

	frontlerp = 1.0F - backlerp;

	/*
	** convert entity's angles into discrete vectors for R, U, and F
	*/
	AngleVectors (currententity->angles, vectors[0], vectors[1], vectors[2]);

	/*
	** translation is the vector from last position to this position
	*/
	VectorSubtract (currententity->oldorigin, currententity->origin, translation);

	/*
	** move should be the delta back to the previous frame * backlerp
	*/
	r_lerp_move[0] =  DotProduct(translation, vectors[0]);	// forward
	r_lerp_move[1] = -DotProduct(translation, vectors[1]);	// left
	r_lerp_move[2] =  DotProduct(translation, vectors[2]);	// up

	VectorAdd( r_lerp_move, r_lastframe->translate, r_lerp_move );

	for (i=0 ; i<3 ; i++)
	{
		r_lerp_move[i] = backlerp*r_lerp_move[i] + frontlerp * r_thisframe->translate[i];
	}

	for (i=0 ; i<3 ; i++)
	{
		r_lerp_frontv[i] = frontlerp * r_thisframe->scale[i];
		r_lerp_backv[i]  = backlerp  * r_lastframe->scale[i];
	}
}

finalvert_t *finalverts = NULL, *finalverts_max = NULL;

extern void (*d_pdrawspans)(const entity_t *currententity, spanpackage_t *pspanpackage);
void R_PolysetDrawSpans8_Opaque(const entity_t *currententity, spanpackage_t *pspanpackage);
void R_PolysetDrawSpans8_33(const entity_t *currententity, spanpackage_t *pspanpackage);
void R_PolysetDrawSpans8_66(const entity_t *currententity, spanpackage_t *pspanpackage);
void R_PolysetDrawSpansConstant8_33(const entity_t *currententity, spanpackage_t *pspanpackage);
void R_PolysetDrawSpansConstant8_66(const entity_t *currententity, spanpackage_t *pspanpackage);

/*
================
R_AliasDrawModel
================
*/
static dmdl_t *
R_AliasSelectLod(const entity_t *currententity, const model_t *currentmodel)
{
	vec3_t delta;

	if (currentmodel->lodextradata && sw_lodsize->value > 0 &&
		!(currententity->flags & RF_WEAPONMODEL))
	{
		VectorSubtract(currententity->origin, r_origin, delta);
		if (DotProduct(delta, vpn) * sw_lodsize->value > 64.0f * xscale)
		{
			return (dmdl_t *)currentmodel->lodextradata;
		}
	}
	return (dmdl_t *)currentmodel->extradata;
}

void
R_AliasDrawModel(entity_t *currententity, const model_t *currentmodel)
{
	s_pmdl = R_AliasSelectLod(currententity, currentmodel);

	if ( r_lerpmodels->value == 0 )
		currententity->backlerp = 0;

	float oldAliasxscale = aliasxscale;
	float oldAliasyscale = aliasyscale;

	if ( currententity->flags & RF_WEAPONMODEL )
	{
		if ( r_lefthand->value == 2.0F )
		{
			return;
		}

		if (r_gunfov->value >= 0)
		{
			float gunfov = 2.0F * tanf((M_PI/360.0F) * (float)r_gunfov->value);
			aliasxscale = ((float)r_refdef.vrect.width / gunfov) * r_aliasuvscale;
			aliasyscale = aliasxscale;
		}

		if ( r_lefthand->value == 1.0F )
			aliasxscale = -aliasxscale;
	}

	/*
	** we have to set our frame pointers and transformations before
	** doing any real work
	*/
	R_AliasSetupFrames(currententity, currentmodel, s_pmdl);
	R_AliasSetUpTransform(currententity);

	// see if the bounding box lets us trivially reject, also sets
	// trivial accept status
	if ( R_AliasCheckBBox(currententity) == BBOX_TRIVIAL_REJECT )
	{
		if ( currententity->flags & RF_WEAPONMODEL )
		{
			aliasxscale = oldAliasxscale;
			aliasyscale = oldAliasyscale;
		}
		return;
	}

	// set up the skin and verify it exists
	if ( !R_AliasSetupSkin(currententity, currentmodel) )
	{
		R_Printf(PRINT_ALL, "R_AliasDrawModel %s: NULL skin found\n",
			currentmodel->name);
		aliasxscale = oldAliasxscale;
		aliasyscale = oldAliasyscale;
		return;
	}

	r_amodels_drawn++;
	R_AliasSetupLighting(currententity);

	/*
	** select the proper span routine based on translucency
	*/
	// added double damage shell
	// reordered to handle blending
	if ( currententity->flags & ( RF_SHELL_RED | RF_SHELL_GREEN | RF_SHELL_BLUE | RF_SHELL_DOUBLE | RF_SHELL_HALF_DAM) )
	{
		int		color;

		// added double
		color = currententity->flags & ( RF_SHELL_RED | RF_SHELL_GREEN | RF_SHELL_BLUE | RF_SHELL_DOUBLE | RF_SHELL_HALF_DAM);

		// reordered, new shells after old shells (so they get overriden)
		if ( color == RF_SHELL_RED )
			r_aliasblendcolor = SHELL_RED_COLOR;
		else if ( color == RF_SHELL_GREEN )
			r_aliasblendcolor = SHELL_GREEN_COLOR;
		else if ( color == RF_SHELL_BLUE )
			r_aliasblendcolor = SHELL_BLUE_COLOR;
		else if ( color == (RF_SHELL_RED | RF_SHELL_GREEN) )
			r_aliasblendcolor = SHELL_RG_COLOR;
		else if ( color == (RF_SHELL_RED | RF_SHELL_BLUE) )
			r_aliasblendcolor = SHELL_RB_COLOR;
		else if ( color == (RF_SHELL_BLUE | RF_SHELL_GREEN) )
			r_aliasblendcolor = SHELL_BG_COLOR;
		// added this .. it's yellowish
		else if ( color == (RF_SHELL_DOUBLE) )
			r_aliasblendcolor = SHELL_DOUBLE_COLOR;
		else if ( color == (RF_SHELL_HALF_DAM) )
			r_aliasblendcolor = SHELL_HALF_DAM_COLOR;
		else
			r_aliasblendcolor = SHELL_WHITE_COLOR;

		if ( currententity->alpha > 0.33F )
			d_pdrawspans = R_PolysetDrawSpansConstant8_66;
		else
			d_pdrawspans = R_PolysetDrawSpansConstant8_33;
	}
	else if ( currententity->flags & RF_TRANSLUCENT )
	{
		if ( currententity->alpha > 0.66F )
			d_pdrawspans = R_PolysetDrawSpans8_Opaque;
		else if ( currententity->alpha > 0.33F )
			d_pdrawspans = R_PolysetDrawSpans8_66;
		else
			d_pdrawspans = R_PolysetDrawSpans8_33;
	}
	else
	{
		d_pdrawspans = R_PolysetDrawSpans8_Opaque;
	}

	pe_alias = pe_active && d_pdrawspans == R_PolysetDrawSpans8_Opaque &&
		!((r_newrefdef.rdflags & RDF_IRGOGGLES) && (currententity->flags & RF_IR_VISIBLE));
	if (pe_alias)
		PE_SetSkin(r_affinetridesc.pskin, r_affinetridesc.skinwidth);
	else
		PE_Finish();

	/*
	** compute this_frame and old_frame addresses
	*/
	R_AliasSetUpLerpData(currententity, s_pmdl, currententity->backlerp);

	if (currententity->flags & RF_DEPTHHACK)
		s_ziscale = (float)(0x8000 * 3) * (float)SHIFT16XYZ_MULT;
	else
		s_ziscale = (float)0x8000 * (float)SHIFT16XYZ_MULT;

	{
		finalvert_t *verts = finalverts, *verts_max = finalverts_max;
		vrect_t save_vrect = r_refdef.aliasvrect;
		int save_right = r_refdef.aliasvrectright, save_bottom = r_refdef.aliasvrectbottom;

		/* the engine reads the vertices later, from a pool, and draws the triangles
		   without a clip flag itself: with its scissor (version 9) only those that
		   leave the guard band get one */
		r_alias_mesh = pe_alias && pe_version >= 5 && s_pmdl->num_tris <= 4096 &&
			!((currententity->flags & RF_WEAPONMODEL) && r_lefthand->value == 1.0F);
		if (r_alias_mesh && (verts = PE_AllocVerts(s_pmdl->num_xyz)) != NULL)
		{
			verts_max = verts + s_pmdl->num_xyz + 1;
			if (pe_world && pe_version >= 9)
			{
				const float cx = aliasxcenter, cy = aliasycenter;

				r_refdef.aliasvrect.x = cx - PE_GUARD * (cx - save_vrect.x);
				r_refdef.aliasvrect.y = cy - PE_GUARD * (cy - save_vrect.y);
				r_refdef.aliasvrectright = cx + PE_GUARD * (save_right - cx);
				r_refdef.aliasvrectbottom = cy + PE_GUARD * (save_bottom - cy);
			}
		}
		else
		{
			r_alias_mesh = false;
			verts = finalverts;
		}

		R_AliasPreparePoints(currententity, verts, verts_max);

		r_refdef.aliasvrect = save_vrect;
		r_refdef.aliasvrectright = save_right;
		r_refdef.aliasvrectbottom = save_bottom;
	}

	if ( currententity->flags & RF_WEAPONMODEL )
	{
		aliasxscale = oldAliasxscale;
		aliasyscale = oldAliasyscale;
	}
}
