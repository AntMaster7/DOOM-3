/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 GPL Source Code (?Doom 3 Source Code?).

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#include "../../idlib/precompiled.h"
#pragma hdrstop

#include "win_local.h"
#include <intrin.h>
#include <float.h>

/*
==============================================================================

win_cpu.cpp for x64: the same interface through intrinsics. The 32-bit file
is inline x87 and cpuid assembly, which the x64 compiler does not take.
x64 code has no x87 state worth managing: floating point is SSE, so the FPU
stack functions are trivial and precision control does not exist.

==============================================================================
*/

/*
================
Sys_GetClockTicks
================
*/
double Sys_GetClockTicks( void ) {
	return (double)__rdtsc();
}

/*
================
Sys_ClockTicksPerSecond
================
*/
double Sys_ClockTicksPerSecond( void ) {
	static double ticks = 0;

	if ( !ticks ) {
		HKEY hKey;
		DWORD procSpeed = 0;
		DWORD buflen = sizeof( procSpeed );

		if ( !RegOpenKeyEx( HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &hKey ) ) {
			if ( RegQueryValueEx( hKey, "~MHz", NULL, NULL, (LPBYTE) &procSpeed, &buflen ) == ERROR_SUCCESS ) {
				ticks = (double) procSpeed * 1000000;
			}
			RegCloseKey( hKey );
		}
		if ( !ticks ) {
			ticks = 3000000000.0;
		}
	}
	return ticks;
}

/*
================
Sys_GetCPUId
================
*/
cpuid_t Sys_GetCPUId( void ) {
	int regs[4];
	int flags;
	char vendor[13];

	__cpuid( regs, 0 );
	memcpy( vendor, &regs[1], 4 );
	memcpy( vendor + 4, &regs[3], 4 );
	memcpy( vendor + 8, &regs[2], 4 );
	vendor[12] = 0;
	flags = !strcmp( vendor, "AuthenticAMD" ) ? CPUID_AMD : CPUID_INTEL;

	__cpuid( regs, 1 );
	if ( regs[3] & ( 1 << 23 ) ) {
		flags |= CPUID_MMX;
	}
	if ( regs[3] & ( 1 << 25 ) ) {
		flags |= CPUID_SSE | CPUID_FTZ;
	}
	if ( regs[3] & ( 1 << 26 ) ) {
		flags |= CPUID_SSE2 | CPUID_DAZ;		// every x64 processor has DAZ
	}
	if ( regs[2] & ( 1 << 0 ) ) {
		flags |= CPUID_SSE3;
	}
	if ( regs[3] & ( 1 << 15 ) ) {
		flags |= CPUID_CMOV;
	}
	if ( regs[3] & ( 1 << 28 ) ) {
		flags |= CPUID_HTT;
	}
	return (cpuid_t)flags;
}

/*
===============
Sys_FPU_PrintStateFlags
===============
*/
int Sys_FPU_PrintStateFlags( char *ptr, int ctrl, int stat, int tags, int inof, int inse, int opof, int opse ) {
	ptr[0] = '\0';
	return 0;
}

bool Sys_FPU_StackIsEmpty( void ) {
	return true;
}

void Sys_FPU_ClearStack( void ) {
}

const char *Sys_FPU_GetState( void ) {
	static char state[64];
	sprintf( state, "MXCSR = 0x%08x\n", _mm_getcsr() );
	return state;
}

void Sys_FPU_EnableExceptions( int exceptions ) {
}

void Sys_FPU_SetPrecision( int precision ) {
	// SSE arithmetic has the precision of its operand type
}

/*
===============
Sys_FPU_SetRounding
===============
*/
void Sys_FPU_SetRounding( int rounding ) {
	static const unsigned int modes[4] = { _RC_NEAR, _RC_DOWN, _RC_UP, _RC_CHOP };
	unsigned int current;
	_controlfp_s( &current, modes[ rounding & 3 ], _MCW_RC );
}

void Sys_FPU_SetDAZ( bool enable ) {
	_mm_setcsr( ( _mm_getcsr() & ~0x0040u ) | ( enable ? 0x0040u : 0 ) );
}

void Sys_FPU_SetFTZ( bool enable ) {
	_mm_setcsr( ( _mm_getcsr() & ~0x8000u ) | ( enable ? 0x8000u : 0 ) );
}
