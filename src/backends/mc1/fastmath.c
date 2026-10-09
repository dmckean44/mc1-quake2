/*
 * The MRISC32 FPU has no square root instruction, so newlib falls back to
 * a slow bit-by-bit loop. Override it with the reciprocal square root
 * estimate refined by Newton-Raphson, which is accurate to about 2 ulp.
 */

#include "../../common/header/shared.h"

#ifdef __MRISC32__
float
sqrtf(float x)
{
	if (!(x > 0.0f))
	{
		return 0.0f;
	}
	return x * Q_rsqrt(x);
}
#endif
