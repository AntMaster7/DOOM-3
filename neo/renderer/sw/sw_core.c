/* sw_core.c -- the unity translation unit of the software rasterizer core (plan 6.2).
   Compile as C with /O2 /Oi /Ot /fp:fast /arch:AVX512 and no precompiled header; nothing else in
   the engine gets /arch:AVX512. The order below is the order the pieces compile in.

   The core sees sw_api.h and the C runtime, never the engine (invariant I7). */
#include "core/base.h"
#include "sw_api.h"
#include "core/config.h"
#include "core/simd.h"
#include "core/types.h"

#include "core/pool.c"
#include "tex/texfetch.c"
#include "geom/xform.c"
#include "geom/vertex.c"
#include "geom/setup.c"
#include "kern/blend.c"
#include "kern/k_stage.c"
#include "kern/k_interaction.c"
#include "kern/k_misc.c"
#include "rast/raster.c"
#include "rast/tile.c"
#include "sw_frame.c"
