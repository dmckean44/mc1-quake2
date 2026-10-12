#include "header/local.h"

qboolean pe_present;
int pe_version;
qboolean pe_active;
qboolean pe_alias;
unsigned pe_submitted;
unsigned pe_synced;
unsigned pe_spans_fence;
qboolean pe_world;
pe_surfedge_t *pe_surfedges;
int pe_numsurfedges;
int pe_maxsurfedges;

#if !defined(MC1)
volatile unsigned pe_noreg;
#endif

static cvar_t *sw_pe;
static const pixel_t *pe_skin;
static int pe_skinwidth;

void
PE_Init(void)
{
	sw_pe = ri.Cvar_Get("sw_pe", "2", CVAR_ARCHIVE);
#if defined(MC1)
	{
		unsigned id = PE_REG(PE_ID);

		pe_present = (id >> 16) == PE_ID_MAGIC;
		pe_version = id & 0xffff;
	}
#endif
	if (pe_present)
	{
		R_Printf(PRINT_ALL, "Pixel engine version %d found.\n", pe_version);
	}
}

void
PE_BeginFrame(void)
{
	PE_Finish();
	pe_active = pe_present && sw_pe->value;
	pe_world = pe_active && pe_version >= 2 && sw_pe->value >= 2 &&
		!sw_draworder->value && !sw_drawflat->value;
	pe_alias = false;
	pe_numsurfedges = 0;
	if (!pe_active)
	{
		return;
	}

	if (pe_maxsurfedges < 2 * (edge_max - r_edges) + 64)
	{
		free(pe_surfedges);
		pe_maxsurfedges = 2 * (edge_max - r_edges) + 64;
		pe_surfedges = malloc(pe_maxsurfedges * sizeof(*pe_surfedges));
	}

	PE_Begin(PE_TARGET, 5);
	PE_Ptr(d_viewbuffer);
	PE_Word(vid_buffer_width);
	PE_Ptr(d_pzbuffer);
	PE_Ptr(vid_colormap);
	PE_Ptr(vid_alphamap);
	pe_skin = NULL;
}

void
PE_SetSkin(const pixel_t *pskin, int skinwidth)
{
	if (pskin == pe_skin && skinwidth == pe_skinwidth)
	{
		return;
	}
	pe_skin = pskin;
	pe_skinwidth = skinwidth;
	PE_Begin(PE_SKIN, 2);
	PE_Ptr(pskin);
	PE_Word(skinwidth);
}

void
PE_CoverBegin(void)
{
	PE_Begin(PE_COVER, 5);
	PE_Word(r_refdef.vrect.x);
	PE_Word(r_refdef.vrect.y);
	PE_Word(r_refdef.vrectright);
	PE_Word(r_refdef.vrectbottom);
	PE_Word(shift_size);
}

void
PE_DrawSurf(const surf_t *s, int flags, const int *turb,
	float d_ziorigin, float d_zistepu, float d_zistepv,
	float zorigin, float zstepu, float zstepv)
{
	int i, n = 0;

	if (!pe_world)
	{
		pe_spans_fence = PE_Begin(PE_SPANS, 21);
	}
	else if (s->flags & SURF_DRAWBACKGROUND)
	{
		PE_Begin(PE_FILL, 21);
	}
	else
	{
		n = s->pe_count;
		PE_Begin(PE_POLY, 21 + 3 * n);
	}
	PE_Word(flags);
	if (pe_world)
		PE_Word(n);
	else
		PE_Ptr(s->spans);
	PE_Ptr(cacheblock);
	PE_Word(cachewidth);
	PE_Word(sadjust);
	PE_Word(tadjust);
	PE_Word(bbextents);
	PE_Word(bbextentt);
	PE_Float(d_sdivzorigin);
	PE_Float(d_sdivzstepu);
	PE_Float(d_sdivzstepv);
	PE_Float(d_tdivzorigin);
	PE_Float(d_tdivzstepu);
	PE_Float(d_tdivzstepv);
	PE_Float(d_ziorigin);
	PE_Float(d_zistepu);
	PE_Float(d_zistepv);
	PE_Float(zorigin);
	PE_Float(zstepu);
	PE_Float(zstepv);
	PE_Ptr(turb);

	for (i = 0; i < n; i++)
	{
		const pe_surfedge_t *e = &pe_surfedges[s->pe_first + i];

		PE_Word((e->edge->v_first & 0x7fff) | (e->left << 15) | (e->edge->v_last << 16));
		PE_Word(e->edge->u);
		PE_Word(e->edge->u_step);
	}
}

/* model vertices the engine reads after the model is done (MESH) */
#define PE_VERTPOOL	8192
#define PE_MAXTRIS	4096
static finalvert_t pe_vertpool[PE_VERTPOOL];
static int pe_vertpos;
static unsigned pe_mesh_fence;
static unsigned short pe_order[PE_MAXTRIS];

finalvert_t *
PE_AllocVerts(int n)
{
	finalvert_t *v;

	if (n > PE_VERTPOOL)
	{
		return NULL;
	}
	if (pe_vertpos + n > PE_VERTPOOL)
	{
		PE_WaitFor(pe_mesh_fence);
		pe_vertpos = 0;
	}
	v = &pe_vertpool[pe_vertpos];
	pe_vertpos += n;
	return v;
}

/* every triangle of a model whose vertices carry no clip flag (the rest are the CPU's) */
void
PE_DrawMesh(const finalvert_t *verts, const dtriangle_t *tris, int numtris, const dstvert_t *st)
{
	int i;

	if (!pe_order[1])
	{
		for (i = 0; i < PE_MAXTRIS; i++)
		{
			pe_order[i] = i;
		}
	}
	pe_mesh_fence = PE_Begin(PE_MESH, 6);
	PE_Ptr(verts);
	PE_Word(sizeof(finalvert_t));
	PE_Ptr(tris);
	PE_Ptr(pe_order);
	PE_Word(numtris);
	PE_Ptr(st);
}
