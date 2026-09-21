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
#include <dbghelp.h>

/*
==============================================================================

Crash report for unattended runs: an unhandled exception writes crash.txt
(exception, registers, symbolized stack) into the working directory, which
is the exe's directory for every launch script. Scripted runs have no
debugger attached and no one to read a dialog.

Uses nothing from the engine: the heap and the console may be the things
that are broken.

==============================================================================
*/

static const char *Sys_CrashCodeName( DWORD code ) {
	switch ( code ) {
		case EXCEPTION_ACCESS_VIOLATION:		return "ACCESS_VIOLATION";
		case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:	return "ARRAY_BOUNDS_EXCEEDED";
		case EXCEPTION_ILLEGAL_INSTRUCTION:		return "ILLEGAL_INSTRUCTION";
		case EXCEPTION_PRIV_INSTRUCTION:		return "PRIV_INSTRUCTION";
		case EXCEPTION_INT_DIVIDE_BY_ZERO:		return "INT_DIVIDE_BY_ZERO";
		case EXCEPTION_FLT_DIVIDE_BY_ZERO:		return "FLT_DIVIDE_BY_ZERO";
		case EXCEPTION_FLT_INVALID_OPERATION:	return "FLT_INVALID_OPERATION";
		case EXCEPTION_STACK_OVERFLOW:			return "STACK_OVERFLOW";
		case EXCEPTION_IN_PAGE_ERROR:			return "IN_PAGE_ERROR";
		default:								return "exception";
	}
}

static LONG WINAPI Sys_UnhandledExceptionFilter( EXCEPTION_POINTERS *info ) {
	static LONG entered;
	if ( InterlockedExchange( &entered, 1 ) ) {
		return EXCEPTION_EXECUTE_HANDLER;
	}

	FILE *f = fopen( "crash.txt", "w" );
	if ( !f ) {
		return EXCEPTION_EXECUTE_HANDLER;
	}

	const EXCEPTION_RECORD *rec = info->ExceptionRecord;
	fprintf( f, "%s (0x%08lx) at %p, thread %lu\n", Sys_CrashCodeName( rec->ExceptionCode ),
		rec->ExceptionCode, rec->ExceptionAddress, GetCurrentThreadId() );
	if ( rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2 ) {
		fprintf( f, "%s address %p\n", rec->ExceptionInformation[0] == 0 ? "reading" :
			( rec->ExceptionInformation[0] == 1 ? "writing" : "executing" ), (void *)rec->ExceptionInformation[1] );
	}
	fprintf( f, "command line: %s\n\n", GetCommandLineA() );

	// StackWalk64 modifies the context it is given
	CONTEXT ctx = *info->ContextRecord;
	STACKFRAME64 frame;
	memset( &frame, 0, sizeof( frame ) );
	frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
#if defined( _M_X64 )
	const DWORD machine = IMAGE_FILE_MACHINE_AMD64;
	frame.AddrPC.Offset = ctx.Rip;
	frame.AddrFrame.Offset = ctx.Rbp;
	frame.AddrStack.Offset = ctx.Rsp;
	fprintf( f, "rax %016llx rbx %016llx rcx %016llx rdx %016llx\nrsi %016llx rdi %016llx rbp %016llx rsp %016llx\n\n",
		ctx.Rax, ctx.Rbx, ctx.Rcx, ctx.Rdx, ctx.Rsi, ctx.Rdi, ctx.Rbp, ctx.Rsp );
#else
	const DWORD machine = IMAGE_FILE_MACHINE_I386;
	frame.AddrPC.Offset = ctx.Eip;
	frame.AddrFrame.Offset = ctx.Ebp;
	frame.AddrStack.Offset = ctx.Esp;
	fprintf( f, "eax %08lx ebx %08lx ecx %08lx edx %08lx\nesi %08lx edi %08lx ebp %08lx esp %08lx\n\n",
		ctx.Eax, ctx.Ebx, ctx.Ecx, ctx.Edx, ctx.Esi, ctx.Edi, ctx.Ebp, ctx.Esp );
#endif

	HANDLE process = GetCurrentProcess();
	SymSetOptions( SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS );
	SymInitialize( process, NULL, TRUE );

	for ( int i = 0; i < 48; i++ ) {
		if ( !StackWalk64( machine, process, GetCurrentThread(), &frame, &ctx, NULL,
							SymFunctionTableAccess64, SymGetModuleBase64, NULL ) || frame.AddrPC.Offset == 0 ) {
			break;
		}

		char symBuffer[ sizeof( SYMBOL_INFO ) + 512 ];
		SYMBOL_INFO *sym = (SYMBOL_INFO *)symBuffer;
		memset( sym, 0, sizeof( SYMBOL_INFO ) );
		sym->SizeOfStruct = sizeof( SYMBOL_INFO );
		sym->MaxNameLen = 511;

		IMAGEHLP_MODULE64 module;
		memset( &module, 0, sizeof( module ) );
		module.SizeOfStruct = sizeof( module );
		const char *moduleName = SymGetModuleInfo64( process, frame.AddrPC.Offset, &module ) ? module.ModuleName : "?";

		DWORD64 symOffset = 0;
		DWORD lineOffset = 0;
		IMAGEHLP_LINE64 line;
		memset( &line, 0, sizeof( line ) );
		line.SizeOfStruct = sizeof( line );

		fprintf( f, "%2i  %p  %s!", i, (void *)(UINT_PTR)frame.AddrPC.Offset, moduleName );
		if ( SymFromAddr( process, frame.AddrPC.Offset, &symOffset, sym ) ) {
			fprintf( f, "%s+0x%x", sym->Name, (unsigned int)symOffset );
		} else {
			fprintf( f, "?" );
		}
		if ( SymGetLineFromAddr64( process, frame.AddrPC.Offset, &lineOffset, &line ) ) {
			fprintf( f, "  %s:%lu", line.FileName, line.LineNumber );
		}
		fprintf( f, "\n" );
	}

	fclose( f );
	return EXCEPTION_EXECUTE_HANDLER;
}

/*
================
Sys_InstallCrashHandler
================
*/
void Sys_InstallCrashHandler( void ) {
	SetUnhandledExceptionFilter( Sys_UnhandledExceptionFilter );
}
