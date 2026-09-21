/* sw_backend.cpp -- the software back end's side of the seam: the cvars, start-up and shutdown,
   and SW_ExecuteBackEndCommands, which takes the place of RB_ExecuteBackEndCommands in
   R_IssueRenderCommands. The front end does not know which of the two runs.

   While GL is alive (until Stage 9) it still owns the window: the finished frame goes to the
   screen through sw_present.cpp, and r_swCompare 1 runs the GL back end on the same command list
   instead, in the same process, with the same images: the reference for every comparison. */
#include "../../idlib/precompiled.h"
#pragma hdrstop

#include "../tr_local.h"
#include "sw_local.h"

idCVar r_swRenderer( "r_swRenderer", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_INIT, "render with the CPU rasterizer (AVX-512). Command line only. 1 = OpenGL stays alive next to it (r_swCompare, the reference image), 2 = no OpenGL at all: no context, opengl32.dll never loaded, the frame presented through D3D12 or GDI", 0, 2 );
idCVar r_swCompare( "r_swCompare", "0", CVAR_RENDERER | CVAR_INTEGER, "with r_swRenderer 1: 0 = software, 1 = the GL back end on the same command list, 2 = both: GL left, software right, 3 = both: difference x8, 4 = both: software shown, numbers only", 0, 4, idCmdSystem::ArgCompletion_Integer<0,4> );
idCVar r_swComparePrint( "r_swComparePrint", "0", CVAR_RENDERER | CVAR_INTEGER, "r_swCompare 2..4: print PSNR / largest difference every N frames, 0 = never" );
idCVar r_swThreads( "r_swThreads", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_INIT, "threads of the software rasterizer, 0 = all logical processors" );
idCVar r_swStatFrames( "r_swStatFrames", "2148", CVAR_RENDERER | CVAR_INTEGER, "frames the shutdown report (and swReportStats) covers; demo1 has 2148" );
idCVar r_swDebugEqual( "r_swDebugEqual", "0", CVAR_RENDERER | CVAR_BOOL, "count the covered pixels of depth-EQUAL draws whose depth is NOT equal to the stored one (invariant I2: opaque light passes must find the depth fill's bits); shown by r_swShowStats" );
idCVar r_swProfile( "r_swProfile", "0", CVAR_RENDERER | CVAR_BOOL, "time the tile phase by category (load, depth, zrange, shadow, light, colour, store): thread-summed ms / threads, in swReportStats" );
idCVar r_swDepthBounds( "r_swDepthBounds", "1", CVAR_RENDERER | CVAR_BOOL, "shadow volumes honour the light's depth bounds (r_useDepthBoundsTest) and skip cells and tiles outside them" );
idCVar r_swKernelCut( "r_swKernelCut", "0", CVAR_RENDERER | CVAR_INTEGER, "ORACLE, only in a core built with SW_ORACLES=1 (wrong image, right cost): k_interaction returns dark after stage N: 1 entry, 2 falloff, 3 projection, 4 bump and N.L" );
idCVar r_swStatFile( "r_swStatFile", "", CVAR_RENDERER, "at shutdown, write the per-frame records the report is made of to this CSV (in fs_savepath/base)" );
idCVar r_swShadowCull( "r_swShadowCull", "1", CVAR_RENDERER | CVAR_BOOL, "a tile skips the shadow volumes of a light that draws no stencil-tested surface in it" );
idCVar r_swHier( "r_swHier", "1", CVAR_RENDERER | CVAR_BOOL, "rasterizer: hierarchical 16x16 cell classification of big triangle walks (exact; off = A/B and bisecting)" );
idCVar r_swZrange( "r_swZrange", "1", CVAR_RENDERER | CVAR_BOOL, "rasterizer: reject triangles and cells against the tile's stored z ranges (exact)" );
idCVar r_swCellFast( "r_swCellFast", "1", CVAR_RENDERER | CVAR_BOOL, "rasterizer: settle whole 16x16 cells of a shadow volume without a block walk (exact)" );
idCVar r_swLightCells( "r_swLightCells", "1", CVAR_RENDERER | CVAR_BOOL, "rasterizer: skip 16x16 cells proven outside a light's volume (exact)" );
idCVar r_swTexLevel0( "r_swTexLevel0", "1", CVAR_RENDERER | CVAR_BOOL, "sampler: a 4x4 block whose footprint proves mip level 0 for every lane skips the level rule and the per-lane level tables (exact)" );
idCVar r_swTexWindow( "r_swTexWindow", "1", CVAR_RENDERER | CVAR_INTEGER, "sampler: a 4x4 block whose texels lie in a 16 x 5 texel window fetches them with five row loads instead of four gathers (exact)" );
idCVar r_swBinExact( "r_swBinExact", "1", CVAR_RENDERER | CVAR_BOOL, "rasterizer: a triangle is binned only into the tiles it can cover, not into every tile of its bounding box (exact)" );
idCVar r_swCaptureRegion( "r_swCaptureRegion", "1", CVAR_RENDERER | CVAR_BOOL, "a _currentRender capture copies only the part of the frame its heat haze / colorProcess draws can reach (exact; 0 = the whole view)" );
idCVar r_swAsync( "r_swAsync", "1", CVAR_RENDERER | CVAR_BOOL, "with r_swRenderer 2: a frame's tile pass and its present run on the rasterizer's coordinator thread while the game and the front end work on the next frame" );
idCVar r_swShowStats("r_swShowStats", "0", CVAR_RENDERER | CVAR_INTEGER, "1 = print the rasterizer's per-frame timings and counters" );

static bool	sw_active;
static bool	sw_suppressPresent;		// compare mode: the composite is presented, not the frame
static bool	sw_suppressGLSwap;
static bool	sw_executing;
static idStr sw_compareShot;		// swCompareShot: base name of the pair to write from the next frame

// what the views of one frame add up to (r_swShowStats)
struct swFrameStats_t {
	int		views;
	double	submitMs, xformMs, setupMs, sortMs, tilesMs, presentMs, totalMs;
	int		draws, tris;
	double	equalFailures, equalInFront, pxColor, pxStencil, pxDepth;
	double	tileMs[SW_PROF_COUNT];
	double	mpxDepth, mpxStencil, mpxColor;		// lanes written, in megapixels
	double	mpxLightAsked, mpxLightDark, kLightCellsDark, kCellsRejected, lightLanesPerCall;
	double	kShadowBinsCulled, kShadowBinsDrawn;
	double	mpxOtherAsked, kOtherCalls;			// every kernel that is not a light pass: lanes asked, calls (one call = one 4x4 block)
	double	kShTris, kShTrisWalked, kShBlocksVisited, kShBlocksCovered, kShBlocksWritten, kCellsFast;	// SW_ORACLES builds, thousands
	double	ltCallsByFlags[16], mpxLtLit, mpxLtFacing, mpxLtSpec;	// SW_ORACLES builds
	double	kTexWin[8];							// SW_ORACLES builds
	double	stMpx[6], stKCalls[6], stKWin[6], stKVertexColor;	// SW_ORACLES builds: the stage kernel by class
	double	kBinPushes, kBinSkips;				// (triangle, tile) pairs binned / left out by r_swBinExact, thousands
	double	captureMpx;							// ...and what they copied, in megapixels
	double	captureMs;							// the _currentRender copies, inside tilesMs
	double	lateMs, earlyMs;					// r_swProfile: a pool thread's average idle time before its first and after its last tile
	double	waitMs;								// r_swAsync: the main thread stood waiting for the LAST frame's tile pass (inside totalMs)
	double	periodMs;							// from this back end's start to the next one's: the frame as the player gets it
};
static swFrameStats_t sw_frame;
static bool	sw_frameOpen;		// between sw_begin_frame and SW_EndFrame
static double	sw_viewWalkMs;		// the views' walks since the frame began (submit + their geometry phases)

// the last SW_STAT_RING frames, reported at shutdown: after `timeDemoQuit demo1 twice` the last
// 2148 of them are the timed pass
static const int		SW_STAT_RING = 4096;
static swFrameStats_t	sw_ring[SW_STAT_RING];
static int				sw_ringCount;

static int SW_CompareDoubles( const void *a, const void *b ) {
	const double d = *(const double *)a - *(const double *)b;
	return d < 0 ? -1 : ( d > 0 ? 1 : 0 );
}

static void SW_ReportColumn( const char *name, size_t offset, int frames ) {
	static double column[SW_STAT_RING];
	double sum = 0.0;
	for ( int i = 0; i < frames; i++ ) {
		const swFrameStats_t &f = sw_ring[ ( sw_ringCount - 1 - i ) & ( SW_STAT_RING - 1 ) ];
		column[i] = *(const double *)( (const byte *)&f + offset );
		sum += column[i];
	}
	qsort( column, frames, sizeof( double ), SW_CompareDoubles );
	common->Printf( "swstats %-8s mean %6.2f  median %6.2f  p95 %6.2f  max %6.2f ms\n", name, sum / frames,
		column[frames / 2], column[(int)( frames * 0.95 )], column[frames - 1] );
}

static void SW_ReportStats_f( const idCmdArgs &args ) {
	int frames = args.Argc() > 1 ? atoi( args.Argv( 1 ) ) : r_swStatFrames.GetInteger();
	frames = Min( frames, Min( sw_ringCount, SW_STAT_RING ) );
	if ( frames <= 0 ) {
		return;
	}
	common->Printf( "swstats: the last %d frames at %dx%d, %d threads\n", frames, glConfig.vidWidth, glConfig.vidHeight, sw_num_threads() );
	SW_ReportColumn( "total", offsetof( swFrameStats_t, totalMs ), frames );
	SW_ReportColumn( "submit", offsetof( swFrameStats_t, submitMs ), frames );
	SW_ReportColumn( "xform", offsetof( swFrameStats_t, xformMs ), frames );
	SW_ReportColumn( "setup", offsetof( swFrameStats_t, setupMs ), frames );
	SW_ReportColumn( "sort", offsetof( swFrameStats_t, sortMs ), frames );
	SW_ReportColumn( "tiles", offsetof( swFrameStats_t, tilesMs ), frames );
	SW_ReportColumn( "present", offsetof( swFrameStats_t, presentMs ), frames );
	SW_ReportColumn( "capture", offsetof( swFrameStats_t, captureMs ), frames );
	SW_ReportColumn( "captMpx", offsetof( swFrameStats_t, captureMpx ), frames );
	SW_ReportColumn( "wait", offsetof( swFrameStats_t, waitMs ), frames );
	SW_ReportColumn( "period", offsetof( swFrameStats_t, periodMs ), frames );
	if ( r_swProfile.GetBool() ) {
		static const char *names[SW_PROF_COUNT] = { "t.load", "t.depth", "t.zrange", "t.shadow", "t.light", "t.colour", "t.store", "cpuSetup", "t.busy" };
		for ( int k = 0; k < SW_PROF_COUNT; k++ ) {
			SW_ReportColumn( names[k], offsetof( swFrameStats_t, tileMs ) + k * sizeof( double ), frames );
		}
	}
	if ( r_swProfile.GetBool() ) {
		SW_ReportColumn( "t.late", offsetof( swFrameStats_t, lateMs ), frames );
		SW_ReportColumn( "t.early", offsetof( swFrameStats_t, earlyMs ), frames );
	}
	SW_ReportColumn( "MpxLtLit", offsetof( swFrameStats_t, mpxLtLit ), frames );
	SW_ReportColumn( "MpxLtFac", offsetof( swFrameStats_t, mpxLtFacing ), frames );
	SW_ReportColumn( "MpxLtSpc", offsetof( swFrameStats_t, mpxLtSpec ), frames );
	for ( int k = 0; k < 16; k++ ) {
		SW_ReportColumn( va( "kLtF%02d", k ), offsetof( swFrameStats_t, ltCallsByFlags ) + k * sizeof( double ), frames );
	}
	{
		static const char *cls[6] = { "2dRep", "2dAdd", "2dBlend", "3dRep", "3dAdd", "3dBlend" };
		for ( int k = 0; k < 6; k++ ) {
			SW_ReportColumn( va( "Mpx%s", cls[k] ), offsetof( swFrameStats_t, stMpx ) + k * sizeof( double ), frames );
			SW_ReportColumn( va( "kC%s", cls[k] ), offsetof( swFrameStats_t, stKCalls ) + k * sizeof( double ), frames );
			SW_ReportColumn( va( "kW%s", cls[k] ), offsetof( swFrameStats_t, stKWin ) + k * sizeof( double ), frames );
		}
		SW_ReportColumn( "kStVtxCol", offsetof( swFrameStats_t, stKVertexColor ), frames );
	}
	SW_ReportColumn( "kBinPush", offsetof( swFrameStats_t, kBinPushes ), frames );
	SW_ReportColumn( "kBinSkip", offsetof( swFrameStats_t, kBinSkips ), frames );
	SW_ReportColumn( "kShTris", offsetof( swFrameStats_t, kShTris ), frames );
	SW_ReportColumn( "kShWalk", offsetof( swFrameStats_t, kShTrisWalked ), frames );
	SW_ReportColumn( "kShBlkV", offsetof( swFrameStats_t, kShBlocksVisited ), frames );
	SW_ReportColumn( "kShBlkC", offsetof( swFrameStats_t, kShBlocksCovered ), frames );
	SW_ReportColumn( "kShBlkW", offsetof( swFrameStats_t, kShBlocksWritten ), frames );
	SW_ReportColumn( "kCellFst", offsetof( swFrameStats_t, kCellsFast ), frames );
	SW_ReportColumn( "MpxDepth", offsetof( swFrameStats_t, mpxDepth ), frames );
	SW_ReportColumn( "MpxStenc", offsetof( swFrameStats_t, mpxStencil ), frames );
	SW_ReportColumn( "MpxColor", offsetof( swFrameStats_t, mpxColor ), frames );
	SW_ReportColumn( "MpxLtAsk", offsetof( swFrameStats_t, mpxLightAsked ), frames );
	SW_ReportColumn( "MpxLtDrk", offsetof( swFrameStats_t, mpxLightDark ), frames );
	SW_ReportColumn( "kLtCells", offsetof( swFrameStats_t, kLightCellsDark ), frames );
	SW_ReportColumn( "kZCells", offsetof( swFrameStats_t, kCellsRejected ), frames );
	SW_ReportColumn( "LtLanes", offsetof( swFrameStats_t, lightLanesPerCall ), frames );
	SW_ReportColumn( "kShCull", offsetof( swFrameStats_t, kShadowBinsCulled ), frames );
	SW_ReportColumn( "kShDrawn", offsetof( swFrameStats_t, kShadowBinsDrawn ), frames );

	if ( r_swStatFile.GetString()[0] ) {
		idFile *f = fileSystem->OpenFileWrite( r_swStatFile.GetString() );
		if ( f ) {
			f->Printf( "frame,total,submit,xform,setup,sort,tiles,present,load,depth,zrange,shadow,light,colour,store,draws,tris,mpxDepth,mpxStencil,mpxColor,mpxLightAsked,mpxOtherAsked,kOtherCalls,period,wait,st2dRep,st2dAdd,st2dBlend,st3dRep,st3dAdd,st3dBlend,stKCalls,stKWin,stKVertexColor\n" );
			for ( int i = frames - 1; i >= 0; i-- ) {
				const swFrameStats_t &r = sw_ring[ ( sw_ringCount - 1 - i ) & ( SW_STAT_RING - 1 ) ];
				f->Printf( "%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.1f,%.3f,%.3f", frames - 1 - i,
					r.totalMs, r.submitMs, r.xformMs, r.setupMs, r.sortMs, r.tilesMs, r.presentMs,
					r.tileMs[SW_PROF_LOAD], r.tileMs[SW_PROF_DEPTH], r.tileMs[SW_PROF_ZRANGE], r.tileMs[SW_PROF_SHADOW], r.tileMs[SW_PROF_LIGHT],
					r.tileMs[SW_PROF_COLOR], r.tileMs[SW_PROF_STORE], r.draws, r.tris, r.mpxDepth, r.mpxStencil, r.mpxColor, r.mpxLightAsked,
					r.mpxOtherAsked, r.kOtherCalls, r.periodMs, r.waitMs );
				f->Printf( ",%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.1f,%.1f,%.1f\n", r.stMpx[0], r.stMpx[1], r.stMpx[2], r.stMpx[3], r.stMpx[4], r.stMpx[5],
					r.stKCalls[0] + r.stKCalls[1] + r.stKCalls[2] + r.stKCalls[3] + r.stKCalls[4] + r.stKCalls[5],
					r.stKWin[0] + r.stKWin[1] + r.stKWin[2] + r.stKWin[3] + r.stKWin[4] + r.stKWin[5], r.stKVertexColor );
			}
			fileSystem->CloseFile( f );
		}
	}
}

void SW_AddCommands( void );
static void SW_CollectAsyncFrame( void );

bool SW_Active( void ) {
	return sw_active;
}

bool SW_GLFree( void ) {
	return r_swRenderer.GetInteger() == 2;
}

bool SW_Presenting( void ) {
	return sw_active && ( r_swCompare.GetInteger() == 0 || SW_GLFree() );
}

bool SW_SuppressGLSwap( void ) {
	return sw_suppressGLSwap;
}

bool SW_Executing( void ) {
	return sw_executing;
}

/*
================
SW_Init

Called when the GL context is up and before the images load, so that every image gets its
software twin as it is generated.
================
*/
bool SW_Init( void ) {
	sw_active = false;
	if ( !r_swRenderer.GetBool() ) {
		return false;
	}
	if ( !sw_init( r_swThreads.GetInteger() ) ) {
		if ( SW_GLFree() ) {
			common->FatalError( "r_swRenderer 2 needs a CPU and OS with AVX-512 F/BW/DQ/VL, and there is no OpenGL to fall back to" );
		}
		common->Printf( "^1r_swRenderer 1 refused: this CPU or OS lacks AVX-512 F/BW/DQ/VL. OpenGL renders.\n" );
		return false;
	}
	sw_active = true;
	SW_AddCommands();
	common->Printf( "software renderer: %d threads, AVX-512\n", sw_num_threads() );
	return true;
}

void SW_Shutdown( void ) {
	if ( !sw_active ) {
		return;
	}
	sw_wait();
	SW_CollectAsyncFrame();
	SW_ReportStats_f( idCmdArgs() );
	if ( SW_GLFree() ) {
		QGL_NullReport( false );		// Stage 9's criterion: empty, and opengl32.dll not in the process
	}
	SW_EndFrameImages();
	SW_PresentShutdown();
	sw_shutdown();
	sw_active = false;
}

/*
================
SW_SetBuffer

RB_SetBuffer: the draw buffer means nothing here; the debug clear does.
================
*/
static void SW_SetBuffer( const setBufferCommand_t *cmd ) {
	backEnd.frameCount = cmd->frameCount;

	if ( r_clear.GetFloat() || idStr::Length( r_clear.GetString() ) != 1 || r_lockSurfaces.GetBool() || r_singleArea.GetBool() ) {
		float c[3];
		if ( sscanf( r_clear.GetString(), "%f %f %f", &c[0], &c[1], &c[2] ) != 3 ) {
			if ( r_clear.GetInteger() == 2 ) {
				c[0] = c[1] = c[2] = 0.0f;
			} else {
				c[0] = 0.4f; c[1] = 0.0f; c[2] = 0.25f;
			}
		}
		const uint32_t r = (uint32_t)idMath::ClampInt( 0, 255, (int)( c[0] * 255.0f + 0.5f ) );
		const uint32_t g = (uint32_t)idMath::ClampInt( 0, 255, (int)( c[1] * 255.0f + 0.5f ) );
		const uint32_t b = (uint32_t)idMath::ClampInt( 0, 255, (int)( c[2] * 255.0f + 0.5f ) );
		sw_clear_framebuffer( r | ( g << 8 ) | ( b << 16 ) | 0xFF000000u );
	}
}

/*
================
SW_DrawViewCommand

RB_DrawView
================
*/
static void SW_DrawViewCommand( const drawSurfsCommand_t *cmd ) {
	backEnd.viewDef = cmd->viewDef;
	backEnd.currentRenderCopied = false;

	if ( !backEnd.viewDef->numDrawSurfs ) {
		return;
	}
	// skip render bypasses everything that has models, assuming them to be 3D views
	if ( r_skipRender.GetBool() && backEnd.viewDef->viewEntitys ) {
		return;
	}
	backEnd.pc.c_surfaces += backEnd.viewDef->numDrawSurfs;

	const double start = Sys_GetClockTicks();
	SW_DrawView( backEnd.viewDef );
	const double total = ( Sys_GetClockTicks() - start ) / Sys_ClockTicksPerSecond() * 1000.0;

	sw_frame.views++;
	sw_viewWalkMs += total;
}

/*
================
SW_EndFrame

The frame's one tile pass (every view of the command list so far), then its numbers. The views'
walks contained their geometry phases: what is left of them is the serial submit.
================
*/
static void SW_AddCoreStats( swFrameStats_t &sw_frame, double walkMs ) {
	SwStats s;
	sw_get_stats( &s );
	sw_frame.xformMs += s.xformMs;
	sw_frame.setupMs += s.setupMs;
	sw_frame.sortMs += s.sortMs;
	sw_frame.tilesMs += s.tilesMs;
	sw_frame.submitMs += walkMs - ( s.xformMs + s.setupMs );
	sw_frame.draws += s.draws;
	sw_frame.tris += s.trisIn;
	sw_frame.equalFailures += (double)s.equalFailures;
	sw_frame.equalInFront += (double)s.equalInFront;
	sw_frame.pxColor += (double)s.pxColor;
	sw_frame.pxStencil += (double)s.pxStencil;
	sw_frame.pxDepth += (double)s.pxDepth;
	for ( int k = 0; k < SW_PROF_COUNT; k++ ) {
		sw_frame.tileMs[k] += s.tileMs[k];
	}
	sw_frame.kShTris += (double)s.shTris * 1e-3;
	sw_frame.kShTrisWalked += (double)s.shTrisWalked * 1e-3;
	sw_frame.kShBlocksVisited += (double)s.shBlocksVisited * 1e-3;
	sw_frame.kShBlocksCovered += (double)s.shBlocksCovered * 1e-3;
	sw_frame.kShBlocksWritten += (double)s.shBlocksWritten * 1e-3;
	sw_frame.kCellsFast += (double)s.cellsFast * 1e-3;
	for ( int k = 0; k < 16; k++ ) {
		sw_frame.ltCallsByFlags[k] += (double)s.ltCallsByFlags[k] * 1e-3;
	}
	sw_frame.mpxLtLit += (double)s.ltLanesLit * 1e-6;
	sw_frame.mpxLtFacing += (double)s.ltLanesFacing * 1e-6;
	sw_frame.mpxLtSpec += (double)s.ltLanesSpec * 1e-6;
	for ( int k = 0; k < 8; k++ ) {
		sw_frame.kTexWin[k] += (double)s.texWin[k] * 1e-3;
	}
	for ( int k = 0; k < 6; k++ ) {
		sw_frame.stMpx[k] += (double)s.stLanes[k] * 1e-6;
		sw_frame.stKCalls[k] += (double)s.stCalls[k] * 1e-3;
		sw_frame.stKWin[k] += (double)s.stWinHits[k] * 1e-3;
	}
	sw_frame.stKVertexColor += (double)s.stVertexColorCalls * 1e-3;
	sw_frame.kBinPushes += (double)s.binPushes * 1e-3;
	sw_frame.kBinSkips += (double)s.binSkips * 1e-3;
	sw_frame.captureMs += s.captureMs;
	sw_frame.captureMpx += s.captureMpx;
	sw_frame.lateMs += s.lateMs;
	sw_frame.earlyMs += s.earlyMs;
	sw_frame.mpxDepth += (double)s.pxDepth * 1e-6;
	sw_frame.mpxStencil += (double)s.pxStencil * 1e-6;
	sw_frame.mpxColor += (double)s.pxColor * 1e-6;
	sw_frame.mpxLightAsked += (double)s.lightLanes * 1e-6;
	sw_frame.mpxOtherAsked += (double)( s.kernelLanes - s.lightLanes ) * 1e-6;
	sw_frame.kOtherCalls += (double)( s.kernelCalls - s.lightCalls ) * 1e-3;
	sw_frame.mpxLightDark += (double)s.lightLanesDark * 1e-6;
	sw_frame.kLightCellsDark += (double)s.lightCellsDark * 1e-3;
	sw_frame.kCellsRejected += (double)s.cellsRejected * 1e-3;
	sw_frame.kShadowBinsCulled += (double)s.shadowBinsCulled * 1e-3;
	sw_frame.kShadowBinsDrawn += (double)s.shadowBinsDrawn * 1e-3;
	if ( s.lightCalls ) {
		sw_frame.lightLanesPerCall = (double)s.lightLanes / (double)s.lightCalls;
	}
}

static int		sw_pendingRing = -1;	// the ring entry of a frame whose tile pass was handed to the coordinator
static double	sw_pendingWalkMs;

static void SW_EndFrame( void ) {
	if ( !sw_frameOpen ) {
		return;
	}
	sw_frameOpen = false;
	sw_end_frame();
	SW_AddCoreStats( sw_frame, sw_viewWalkMs );
	sw_viewWalkMs = 0.0;
}

/*
================
SW_EndFrameAsync

The tile pass and the present leave for the coordinator thread; this thread returns to the game.
The frame's numbers are read when the next command list begins (SW_CollectAsyncFrame).
================
*/
static void SW_EndFrameAsync( void ) {
	sw_frameOpen = false;
	sw_pendingRing = sw_ringCount;		// where this frame's record is about to go
	sw_pendingWalkMs = sw_viewWalkMs;
	sw_viewWalkMs = 0.0;
	SW_PresentAsync();
}

static void SW_CollectAsyncFrame( void ) {
	if ( sw_pendingRing < 0 ) {
		return;
	}
	const double start = Sys_GetClockTicks();
	sw_wait();
	sw_frame.waitMs += ( Sys_GetClockTicks() - start ) / Sys_ClockTicksPerSecond() * 1000.0;
	SW_AddCoreStats( sw_ring[ sw_pendingRing & ( SW_STAT_RING - 1 ) ], sw_pendingWalkMs );
	sw_pendingRing = -1;
}

/*
================
SW_SwapBuffers
================
*/
static void SW_SwapBuffers( void ) {
	if ( sw_frameOpen && SW_GLFree() && r_swAsync.GetBool() && !sw_suppressPresent ) {
		SW_EndFrameAsync();
		return;
	}
	SW_EndFrame();
	if ( sw_suppressPresent ) {
		return;
	}
	const double start = Sys_GetClockTicks();
	int pitch;
	const uint32_t *pixels = sw_framebuffer( &pitch );
	SW_Present( pixels, pitch, glConfig.vidWidth, glConfig.vidHeight );
	sw_frame.presentMs += ( Sys_GetClockTicks() - start ) / Sys_ClockTicksPerSecond() * 1000.0;
}

/*
================
SW_ExecuteBackEndCommands
================
*/
void SW_ExecuteBackEndCommands( const emptyCommand_t *cmds ) {
	if ( cmds->commandId == RC_NOP && !cmds->next ) {
		return;
	}

	const int startTime = Sys_Milliseconds();
	const double startTicks = Sys_GetClockTicks();
	{
		// the frame before this one ends here, as far as the player is concerned
		static double lastStart;
		if ( sw_ringCount > 0 && lastStart > 0.0 ) {
			sw_ring[ ( sw_ringCount - 1 ) & ( SW_STAT_RING - 1 ) ].periodMs = ( startTicks - lastStart ) / Sys_ClockTicksPerSecond() * 1000.0;
		}
		lastStart = startTicks;
	}

	// image loads that finished in the background come back through the idImage hooks
	globalImages->CompleteBackgroundImageLoads();

	// the last frame's tile pass may still run on the coordinator: everything below touches what it reads.
	// The wait is THIS frame's cost, and is counted as such (the first call that would wait is this one).
	const double asyncWaitStart = Sys_GetClockTicks();
	sw_wait();
	const double asyncWaitMs = ( Sys_GetClockTicks() - asyncWaitStart ) / Sys_ClockTicksPerSecond() * 1000.0;

	SW_PresentBeforeResize( glConfig.vidWidth, glConfig.vidHeight );
	sw_resize( glConfig.vidWidth, glConfig.vidHeight );
	sw_debug_count_equal_failures( r_swDebugEqual.GetBool() );
	sw_set_option( SW_OPT_PROFILE, r_swProfile.GetBool() );
	sw_set_option( SW_OPT_KERNEL_CUT, r_swKernelCut.GetInteger() );
	sw_set_option( SW_OPT_DEPTH_BOUNDS, r_swDepthBounds.GetBool() );
	sw_set_option( SW_OPT_SHADOW_CULL, r_swShadowCull.GetBool() );
	sw_set_option( SW_OPT_CAPTURE_REGION, r_swCaptureRegion.GetBool() );
	sw_set_option( SW_OPT_BIN_EXACT, r_swBinExact.GetBool() );
	sw_set_option( SW_OPT_TEX_WINDOW, r_swTexWindow.GetInteger() );
	sw_set_option( SW_OPT_TEX_LEVEL0, r_swTexLevel0.GetBool() );
	// the core's "exact" rejects, each with its kill switch: an image that changes with one of them is a bug in it
	sw_set_option( SW_OPT_HIER, r_swHier.GetBool() );
	sw_set_option( SW_OPT_ZRANGE, r_swZrange.GetBool() );
	sw_set_option( SW_OPT_CELL_FAST, r_swCellFast.GetBool() );
	sw_set_option( SW_OPT_LIGHT_CELLS, r_swLightCells.GetBool() );
	memset( &sw_frame, 0, sizeof( sw_frame ) );
	sw_executing = true;
	sw_frame.waitMs = asyncWaitMs;
	SW_CollectAsyncFrame();
	SW_EndFrameImages();		// of the LAST frame: its tile pass may have run until a moment ago

	// a presenter that lets the GPU read the framebuffer in place must be done with the last frame
	// before this one's first tile is stored; the wait is present time, and is counted as such
	const double waitStart = Sys_GetClockTicks();
	SW_PresentBeginFrame();
	sw_frame.presentMs += ( Sys_GetClockTicks() - waitStart ) / Sys_ClockTicksPerSecond() * 1000.0;

	// the views of this command list are ONE frame for the core: one tile pass, when it ends
	sw_begin_frame();
	sw_frameOpen = true;
	sw_viewWalkMs = 0.0;

	for ( ; cmds; cmds = (const emptyCommand_t *)cmds->next ) {
		switch ( cmds->commandId ) {
		case RC_NOP:
			break;
		case RC_DRAW_VIEW:
			SW_DrawViewCommand( (const drawSurfsCommand_t *)cmds );
			break;
		case RC_SET_BUFFER:
			SW_SetBuffer( (const setBufferCommand_t *)cmds );
			break;
		case RC_SWAP_BUFFERS:
			SW_SwapBuffers();
			break;
		case RC_COPY_RENDER: {
			// RB_CopyRender; the image's hook reads the software frame while sw_executing is set (I5: the
			// views before this command have ended, so the frame is whole)
			const copyRenderCommand_t *copy = (const copyRenderCommand_t *)cmds;
			if ( copy->image && !r_skipCopyTexture.GetBool() ) {
				copy->image->CopyFramebuffer( copy->x, copy->y, copy->imageWidth, copy->imageHeight, false );
			}
			break;
		}
		default:
			common->Error( "SW_ExecuteBackEndCommands: bad commandId" );
			break;
		}
	}

	SW_EndFrame();		// a command list without a swap (a capture to a file, a tiled screenshot)
	sw_executing = false;

	backEnd.pc.msec = Sys_Milliseconds() - startTime;
	sw_frame.totalMs = ( Sys_GetClockTicks() - startTicks ) / Sys_ClockTicksPerSecond() * 1000.0;
	sw_ring[ sw_ringCount++ & ( SW_STAT_RING - 1 ) ] = sw_frame;

	if ( r_swShowStats.GetInteger() ) {
		common->Printf( "sw: %d views %4d draws %6d tris | submit %5.2f xform %5.2f setup %5.2f sort %5.2f tiles %5.2f present %5.2f ms | Mpx depth %.1f stencil %.1f colour %.1f | EQUAL failures %.0f, in front %.0f\n",
			sw_frame.views, sw_frame.draws, sw_frame.tris, sw_frame.submitMs, sw_frame.xformMs, sw_frame.setupMs,
			sw_frame.sortMs, sw_frame.tilesMs, sw_frame.presentMs, sw_frame.pxDepth * 1e-6, sw_frame.pxStencil * 1e-6, sw_frame.pxColor * 1e-6,
			sw_frame.equalFailures, sw_frame.equalInFront );
	}
}

/*
================
SW_ReadPixels
================
*/
void SW_ReadPixels( int x, int y, int width, int height, byte *rgb ) {
	int pitch;
	const uint32_t *pixels = sw_framebuffer( &pitch );
	const int rowBytes = ( width * 3 + 3 ) & ~3;		// GL_PACK_ALIGNMENT 4
	for ( int row = 0; row < height; row++ ) {
		const int fy = glConfig.vidHeight - 1 - ( y + row );
		byte *out = rgb + row * rowBytes;
		if ( fy < 0 || fy >= glConfig.vidHeight || !pixels ) {
			memset( out, 0, rowBytes );
			continue;
		}
		const uint32_t *in = pixels + (size_t)fy * pitch;
		for ( int col = 0; col < width; col++ ) {
			const int fx = x + col;
			const uint32_t p = ( fx >= 0 && fx < glConfig.vidWidth ) ? in[fx] : 0;
			out[col * 3 + 0] = (byte)p;
			out[col * 3 + 1] = (byte)( p >> 8 );
			out[col * 3 + 2] = (byte)( p >> 16 );
		}
	}
}

/*
================
SW_CompareFrame

Both back ends on one command list. GL runs second and keeps its frame in the back buffer
(RB_SwapBuffers is told not to flip); it is read back, measured against the software frame, and
what goes to the screen is a composite. Same process, same images, same registers, same frame
of every animation: any difference is the renderer's.
================
*/
static void SW_CompareFrame( const emptyCommand_t *cmds, int mode ) {
	const int width = glConfig.vidWidth, height = glConfig.vidHeight;

	sw_suppressPresent = true;
	SW_ExecuteBackEndCommands( cmds );
	sw_suppressPresent = false;

	sw_suppressGLSwap = true;
	RB_ExecuteBackEndCommands( cmds );
	sw_suppressGLSwap = false;

	static idList<byte>		glFrame;
	static idList<uint32_t>	composite;
	glFrame.SetNum( width * height * 4, false );
	composite.SetNum( width * height, false );
	qglReadBuffer( GL_BACK );
	qglPixelStorei( GL_PACK_ALIGNMENT, 1 );
	qglReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, glFrame.Ptr() );
	qglPixelStorei( GL_PACK_ALIGNMENT, 4 );

	int pitch;
	const uint32_t *swFrame = sw_framebuffer( &pitch );

	double sumSq = 0.0;
	int maxDiff = 0, differing = 0, over8 = 0;
	for ( int y = 0; y < height; y++ ) {
		const byte *g = glFrame.Ptr() + (size_t)( height - 1 - y ) * width * 4;		// GL rows run bottom-up
		const uint32_t *w = swFrame + (size_t)y * pitch;
		uint32_t *out = composite.Ptr() + (size_t)y * width;
		for ( int x = 0; x < width; x++, g += 4 ) {
			const int dr = abs( (int)g[0] - (int)( w[x] & 255 ) );
			const int dg = abs( (int)g[1] - (int)( ( w[x] >> 8 ) & 255 ) );
			const int db = abs( (int)g[2] - (int)( ( w[x] >> 16 ) & 255 ) );
			const int m = Max( dr, Max( dg, db ) );
			sumSq += (double)( dr * dr + dg * dg + db * db );
			if ( m ) {
				differing++;
				if ( m > 8 ) {
					over8++;
				}
				if ( m > maxDiff ) {
					maxDiff = m;
				}
			}
			const uint32_t glPixel = g[0] | ( g[1] << 8 ) | ( g[2] << 16 ) | 0xFF000000u;
			if ( mode == 2 ) {
				out[x] = x < width / 2 ? glPixel : w[x];
			} else if ( mode == 3 ) {
				out[x] = Min( dr * 8, 255 ) | ( Min( dg * 8, 255 ) << 8 ) | ( Min( db * 8, 255 ) << 16 ) | 0xFF000000u;
			} else {
				out[x] = w[x];
			}
		}
	}
	const double mse = sumSq / ( 3.0 * width * height );
	const double psnr = mse > 0.0 ? 10.0 * log10( 255.0 * 255.0 / mse ) : 99.0;

	static int frame;
	frame++;
	const int every = r_swComparePrint.GetInteger();
	const bool shot = sw_compareShot.Length() > 0;
	if ( shot || ( every > 0 && frame % every == 0 ) ) {
		common->Printf( "swcompare: PSNR %.2f dB  max %d  differing %.3f%%  over 8 codes %.3f%%\n", psnr, maxDiff,
			100.0 * differing / ( (double)width * height ), 100.0 * over8 / ( (double)width * height ) );
	}
	if ( shot ) {
		// R_WriteTGA: flipVertical false = the rows are top-down (the software frame), true = bottom-up (GL's)
		static idList<byte> rows;
		rows.SetNum( width * height * 4, false );
		for ( int y = 0; y < height; y++ ) {
			memcpy( rows.Ptr() + (size_t)y * width * 4, swFrame + (size_t)y * pitch, (size_t)width * 4 );
		}
		for ( int i = 0; i < width * height; i++ ) {
			rows[i * 4 + 3] = 255;
			glFrame[i * 4 + 3] = 255;
		}
		R_WriteTGA( va( "%s_sw.tga", sw_compareShot.c_str() ), rows.Ptr(), width, height, false );
		R_WriteTGA( va( "%s_gl.tga", sw_compareShot.c_str() ), glFrame.Ptr(), width, height, true );
		common->Printf( "wrote %s_gl.tga and %s_sw.tga\n", sw_compareShot.c_str(), sw_compareShot.c_str() );
		sw_compareShot.Clear();
	}

	SW_Present( composite.Ptr(), width, width, height );
}

/*
================
SW_RunFrame
================
*/
void SW_RunFrame( const emptyCommand_t *cmds ) {
	if ( SW_GLFree() ) {
		SW_ExecuteBackEndCommands( cmds );		// there is nothing to compare with
		return;
	}
	int mode = r_swCompare.GetInteger();
	if ( sw_compareShot.Length() && mode < 2 ) {
		mode = 4;		// one compared frame for the pair, whatever is on screen otherwise
	}
	if ( mode >= 2 ) {
		if ( cmds->commandId == RC_NOP && !cmds->next ) {
			return;
		}
		SW_CompareFrame( cmds, mode );
	} else if ( mode == 1 ) {
		RB_ExecuteBackEndCommands( cmds );
	} else {
		SW_ExecuteBackEndCommands( cmds );
	}
}

/*
================
SW_CompareShot_f

swCompareShot <name>: the next frame goes through both back ends; <name>_gl.tga and <name>_sw.tga
are written and the difference is printed.
================
*/
static void SW_CompareShot_f( const idCmdArgs &args ) {
	if ( !sw_active ) {
		common->Printf( "the software renderer is not running (+set r_swRenderer 1)\n" );
		return;
	}
	if ( SW_GLFree() ) {
		common->Printf( "r_swRenderer 2 has no OpenGL to compare with: +set r_swRenderer 1\n" );
		return;
	}
	sw_compareShot = args.Argc() > 1 ? args.Argv( 1 ) : "screenshots/swcompare";
	sw_compareShot.StripFileExtension();
}

/*
================
SW_GLImageInfo_f

swGLImageInfo <image>: what the GL side of an image REALLY is: size, internal format, filters, wrap
modes and the last texels of its first row, read back from the driver. For the cases where GL
does not behave as the code that uploaded the image suggests.
================
*/
static void SW_GLImageInfo_f( const idCmdArgs &args ) {
	if ( args.Argc() < 2 ) {
		common->Printf( "usage: swGLImageInfo <image name>\n" );
		return;
	}
	idImage *image = globalImages->GetImage( args.Argv( 1 ) );
	if ( SW_GLFree() || !image || image->texnum == idImage::TEXTURE_NOT_LOADED ) {
		common->Printf( "'%s' is not loaded in GL\n", args.Argv( 1 ) );
		return;
	}
	GLint w = 0, h = 0, fmt = 0, minF = 0, magF = 0, wrapS = 0, wrapT = 0, maxLevel = 0;
	qglBindTexture( GL_TEXTURE_2D, image->texnum );
	qglGetTexLevelParameteriv( GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w );
	qglGetTexLevelParameteriv( GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h );
	qglGetTexLevelParameteriv( GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &fmt );
	qglGetTexParameteriv( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minF );
	qglGetTexParameteriv( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &magF );
	qglGetTexParameteriv( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &wrapS );
	qglGetTexParameteriv( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &wrapT );
	qglGetTexParameteriv( GL_TEXTURE_2D, 0x813D /* GL_TEXTURE_MAX_LEVEL */, &maxLevel );
	common->Printf( "%s: %i x %i, internal format 0x%04X, min 0x%04X mag 0x%04X, wrap S 0x%04X T 0x%04X, max level %i\n",
		image->imgName.c_str(), w, h, fmt, minF, magF, wrapS, wrapT, maxLevel );
	if ( w > 0 && h > 0 && w * h <= 4096 * 4096 ) {
		byte *texels = (byte *)R_StaticAlloc( w * h * 4 );
		qglGetTexImage( GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels );
		idStr line;
		for ( int x = Max( 0, w - 8 ); x < w; x++ ) {
			line += va( " %i:(%i %i %i %i)", x, texels[x * 4], texels[x * 4 + 1], texels[x * 4 + 2], texels[x * 4 + 3] );
		}
		common->Printf( "  last texels of row 0:%s\n", line.c_str() );
		R_StaticFree( texels );
	}
	backEnd.glState.tmu[0].current2DMap = -1;
}

static void SW_GLCalls_f( const idCmdArgs &args ) {
	QGL_NullReport( args.Argc() > 1 );
}

void SW_AddCommands( void ) {
	cmdSystem->AddCommand( "swGLCalls", SW_GLCalls_f, CMD_FL_RENDERER, "with r_swRenderer 2: the stray OpenGL calls so far, by entry point (any argument: the expected ones too)" );
	cmdSystem->AddCommand( "swGLImageInfo", SW_GLImageInfo_f, CMD_FL_RENDERER, "what GL really holds for an image: size, format, filters, wrap modes, texels" );
	cmdSystem->AddCommand( "swReportStats", SW_ReportStats_f, CMD_FL_RENDERER, "per-phase frame times of the software renderer over the last N frames" );
	cmdSystem->AddCommand( "swCompareShot", SW_CompareShot_f, CMD_FL_RENDERER, "writes the next frame as rendered by GL and by the software renderer, prints their difference" );
}
