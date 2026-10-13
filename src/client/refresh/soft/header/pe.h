/*
 * Driver for the MC1 pixel engine: a command FIFO at 0xc0000200 that draws
 * world spans, surface cache blocks, opaque alias triangles and particles.
 * See doc/mc1_pixel_engine.md.
 */

#ifndef SW_PE_H
#define SW_PE_H

#define PE_ID_MAGIC	0x5045

enum {
	PE_ID = 0x00,
	PE_CMD = 0x04,
	PE_SYNC = 0x08,
	PE_WAIT = 0x0c,
	PE_DONE = 0x10,
	PE_BUSY_LO = 0x18,
	PE_BUSY_HI = 0x1c,
	PE_STALL_LO = 0x20,
	PE_STALL_HI = 0x24
};

enum {
	PE_SPANS = 1,
	PE_SURFACE = 2,
	PE_TRIANGLE = 3,
	PE_PARTICLE = 4,
	PE_TARGET = 5,
	PE_SKIN = 6,
	PE_COVER = 7,
	PE_POLY = 8,
	PE_FILL = 9,
	PE_LITSURF = 10,
	PE_DEFER = 11,
	PE_MESH = 12,
	PE_VERTS2 = 15,
	PE_VIEW = 17,
	PE_FACE = 18,
	PE_PARTICLES = 19,
	PE_FACETEX = 20
};

#define PE_MAX_LIGHT_SAMPLES	1024

enum {
	PE_SPAN_TEX = 1,
	PE_SPAN_Z = 2,
	PE_SPAN_TURB = 4,
	PE_SPAN_FLAT = 8,
	PE_SPAN_NOCOVER = 64	/* depth-tested, not front to back (version 9) */
};

enum {
	PE_PARTICLE_OPAQUE = 0,
	PE_PARTICLE_33 = 1,
	PE_PARTICLE_66 = 2
};

#if defined(MC1)
#define PE_REG(off) (*(volatile unsigned *)(0xc0000200 + (off)))
#else
extern volatile unsigned pe_noreg;
#define PE_REG(off) pe_noreg
#endif

extern qboolean pe_present;
extern int pe_version;
extern qboolean pe_active;
extern qboolean pe_alias;
extern unsigned pe_submitted;
extern unsigned pe_synced;
extern unsigned pe_spans_fence;
extern qboolean pe_world;
extern qboolean pe_face;	/* world faces go out whole, as FACE (version 10) */
extern qboolean pe_faceclip;	/* FACE mip levels from the face clipped to the screen */

typedef struct
{
	edge_t		*edge;
	qboolean	left;
} pe_surfedge_t;

extern pe_surfedge_t *pe_surfedges;
extern int pe_numsurfedges;
extern int pe_maxsurfedges;

static inline void
PE_AddEdge(edge_t *edge, qboolean left)
{
	if (pe_numsurfedges < pe_maxsurfedges)
	{
		pe_surfedges[pe_numsurfedges].edge = edge;
		pe_surfedges[pe_numsurfedges].left = left;
		pe_numsurfedges++;
	}
}

static inline unsigned
PE_Begin(unsigned op, unsigned words)
{
	PE_REG(PE_CMD) = (op << 24) | words;
	return ++pe_submitted;
}

static inline void
PE_Word(unsigned x)
{
	PE_REG(PE_CMD) = x;
}

static inline void
PE_Float(float x)
{
	union { float f; unsigned u; } c;

	c.f = x;
	PE_Word(c.u);
}

static inline void
PE_Ptr(const void *p)
{
	PE_Word((unsigned)(size_t)p);
}

/* Commands finish in order, so waiting for one waits for all before it. */
static inline void
PE_WaitFor(unsigned id)
{
	if (id > pe_synced)
	{
		PE_REG(PE_WAIT) = id;
		pe_synced = id;
	}
}

static inline void
PE_Finish(void)
{
	PE_WaitFor(pe_submitted);
}

void PE_Init(void);
void PE_BeginFrame(void);
void PE_DrawFaceTex(const surf_t *s, int flags, int mip, const float *tmo);
/* solid faces go out as FACETEX: the engine computes their gradients (version 15) */
#define PE_FACETEX_OK(fa)	(pe_face && pe_version >= 15 && \
		!((fa)->flags & (SURF_DRAWSKY | SURF_DRAWBACKGROUND | SURF_DRAWTURB)) && \
		!((fa)->texinfo->flags & SURF_FLOWING))
void R_BmodelViewOrigin(const entity_t *e, vec3_t org);
void PE_SetSkin(const pixel_t *pskin, int skinwidth);
void PE_DrawSurf(const surf_t *s, int flags, const int *turb,
	float d_ziorigin, float d_zistepu, float d_zistepv,
	float zorigin, float zstepu, float zstepv);
void PE_CoverBegin(void);
finalvert_t *PE_AllocVerts(int n);
void PE_DrawMesh(const finalvert_t *verts, const dtriangle_t *tris, int numtris, const dstvert_t *st);

/* guard band: the engine clips models to the screen up to this many times its
   half width or height from the centre (version 9) */
#define PE_GUARD	2.0f

#endif
