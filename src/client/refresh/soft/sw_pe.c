#include "header/local.h"

qboolean pe_present;
qboolean pe_active;
qboolean pe_alias;
unsigned pe_submitted;
unsigned pe_synced;
unsigned pe_spans_fence;

#if !defined(MC1)
volatile unsigned pe_noreg;
#endif

static cvar_t *sw_pe;
static const pixel_t *pe_skin;
static int pe_skinwidth;

void
PE_Init(void)
{
	sw_pe = ri.Cvar_Get("sw_pe", "1", CVAR_ARCHIVE);
#if defined(MC1)
	pe_present = PE_REG(PE_ID) == PE_ID_VALUE;
#endif
	if (pe_present)
	{
		R_Printf(PRINT_ALL, "Pixel engine found.\n");
	}
}

void
PE_BeginFrame(void)
{
	PE_Finish();
	pe_active = pe_present && sw_pe->value;
	pe_alias = false;
	if (!pe_active)
	{
		return;
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
PE_DrawSpans(const espan_t *pspan, int flags, const int *turb,
	float d_ziorigin, float d_zistepu, float d_zistepv,
	float zorigin, float zstepu, float zstepv)
{
	pe_spans_fence = PE_Begin(PE_SPANS, 21);
	PE_Word(flags);
	PE_Ptr(pspan);
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
}
