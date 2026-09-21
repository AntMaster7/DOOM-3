/* core/base.h -- the system headers every piece of the core uses.
   The core is one translation unit (sw_core.c includes the pieces in order), derived from
   CRenderer (C:\Source\CRenderer, commit fec904d). It sees sw_api.h and nothing of the engine. */
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <immintrin.h>
#include <intrin.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
