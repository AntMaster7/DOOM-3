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

#include "../idlib/precompiled.h"
#pragma hdrstop

#include "tr_local.h"
#include "../framework/Session_local.h"

/*
==============================================================================

FRAME CENSUS

r_frameLog <file> writes one CSV row per frame: what the front end handed
to the back end, counted from the command list itself. It does not depend
on which back end runs, or on whether one runs at all (r_skipBackEnd), so
the GL path and the software path log comparable rows.

The counts follow the pass structure of RB_STD_DrawView: depth fill,
per-light shadow and interaction chains, ambient stages, fog and blend
lights. Pixel columns are scissor rectangle areas: an upper bound of the
fill work of a pass, not a measurement of it.

==============================================================================
*/

idCVar r_frameLog( "r_frameLog", "", CVAR_RENDERER, "write a per-frame CSV census of the back end command list to this file (under fs_savepath)" );

typedef struct {
	int		views3D, views2D, subviews;
	int		copyRenders;

	int		surfs, tris;					// every drawSurf of the 3D views
	int		depthSurfs, depthTris;			// opaque + perforated: the depth fill
	int		perforatedTris;					// the alpha tested part of it
	int		ambientSurfs, ambientTris;		// surfaces with ambient stages, not post process
	int		ambientStages;
	int		translucentTris;
	int		postSurfs, postTris;			// SS_POST_PROCESS: need _currentRender
	int		guiSurfs, guiTris;				// the 2D views

	int		lights, fogLights, blendLights, ambientLights;
	int		shadowLights;					// lights with a shadow chain: stencil clear + test
	int		litSurfs, litTris;				// local + global interaction chains
	int		litTransSurfs, litTransTris;	// translucent interaction chain
	int		fogSurfs, fogTris;				// chains of fog and blend lights
	int		shadowSurfs, shadowTris;		// index count chosen as RB_T_Shadow does
	int		shadowTrisInside;				// of those: view inside the volume (the depth fail kind)

	double	lightPx;						// sum of light scissor areas (lit lights only)
	double	litSurfPx;						// sum of interaction surface scissor areas
	double	shadowPx;						// sum of shadow surface scissor areas
	double	viewPx;							// sum of 3D view scissor areas
} frameCensus_t;

static frameCensus_t	census;
static idFile *			censusFile;
static idStr			censusFileName;
static int				censusRows;
static double			censusFrontEndTicks;
static double			censusBackEndTicks;
static double			censusLastFrameTicks;

static double R_CensusRectArea( const idScreenRect &r ) {
	if ( r.x2 < r.x1 || r.y2 < r.y1 ) {
		return 0.0;
	}
	return (double)( r.x2 - r.x1 + 1 ) * ( r.y2 - r.y1 + 1 );
}

/*
================
R_CensusShadowIndexes

The index count selection of RB_T_Shadow
================
*/
static int R_CensusShadowIndexes( const drawSurf_t *surf, const viewLight_t *vLight, bool &inside ) {
	const srfTriangles_t *tri = surf->geo;

	inside = false;
	if ( !r_useExternalShadows.GetInteger() ) {
		inside = true;
		return tri->numIndexes;
	}
	if ( r_useExternalShadows.GetInteger() == 2 || !( surf->dsFlags & DSF_VIEW_INSIDE_SHADOW ) ) {
		return tri->numShadowIndexesNoCaps;
	}
	if ( !vLight->viewInsideLight && !( tri->shadowCapPlaneBits & SHADOW_CAP_INFINITE ) ) {
		if ( vLight->viewSeesShadowPlaneBits & tri->shadowCapPlaneBits ) {
			return tri->numShadowIndexesNoFrontCaps;
		}
		return tri->numShadowIndexesNoCaps;
	}
	inside = true;
	return tri->numIndexes;
}

static void R_CensusChain( const drawSurf_t *chain, int &surfs, int &tris, double *px ) {
	for ( const drawSurf_t *surf = chain; surf; surf = surf->nextOnLight ) {
		surfs++;
		tris += surf->geo->numIndexes / 3;
		if ( px ) {
			*px += R_CensusRectArea( surf->scissorRect );
		}
	}
}

static void R_CensusView( const viewDef_t *viewDef ) {
	int i;

	if ( !viewDef->viewEntitys ) {
		census.views2D++;
		for ( i = 0; i < viewDef->numDrawSurfs; i++ ) {
			census.guiSurfs++;
			census.guiTris += viewDef->drawSurfs[i]->geo->numIndexes / 3;
		}
		return;
	}

	census.views3D++;
	if ( viewDef->isSubview ) {
		census.subviews++;
	}
	census.viewPx += R_CensusRectArea( viewDef->scissor );

	for ( i = 0; i < viewDef->numDrawSurfs; i++ ) {
		const drawSurf_t *surf = viewDef->drawSurfs[i];
		const idMaterial *shader = surf->material;
		const int tris = surf->geo->numIndexes / 3;

		census.surfs++;
		census.tris += tris;

		if ( shader->GetSort() >= SS_POST_PROCESS ) {
			census.postSurfs++;
			census.postTris += tris;
			continue;
		}
		if ( shader->Coverage() == MC_TRANSLUCENT ) {
			census.translucentTris += tris;
		} else {
			census.depthSurfs++;
			census.depthTris += tris;
			if ( shader->Coverage() == MC_PERFORATED ) {
				census.perforatedTris += tris;
			}
		}
		if ( shader->HasAmbient() ) {
			census.ambientSurfs++;
			census.ambientTris += tris;
			for ( int s = 0; s < shader->GetNumStages(); s++ ) {
				const shaderStage_t *stage = shader->GetStage( s );
				if ( stage->lighting == SL_AMBIENT && surf->shaderRegisters[ stage->conditionRegister ] != 0 ) {
					census.ambientStages++;
				}
			}
		}
	}

	for ( const viewLight_t *vLight = viewDef->viewLights; vLight; vLight = vLight->next ) {
		census.lights++;

		if ( vLight->lightShader->IsFogLight() || vLight->lightShader->IsBlendLight() ) {
			if ( vLight->lightShader->IsFogLight() ) {
				census.fogLights++;
			} else {
				census.blendLights++;
			}
			R_CensusChain( vLight->globalInteractions, census.fogSurfs, census.fogTris, NULL );
			R_CensusChain( vLight->localInteractions, census.fogSurfs, census.fogTris, NULL );
			continue;
		}
		if ( vLight->lightShader->IsAmbientLight() ) {
			census.ambientLights++;
		}
		if ( !vLight->localInteractions && !vLight->globalInteractions && !vLight->translucentInteractions ) {
			continue;		// RB_ARB2_DrawInteractions skips it, shadows included
		}

		census.lightPx += R_CensusRectArea( vLight->scissorRect );
		if ( vLight->globalShadows || vLight->localShadows ) {
			census.shadowLights++;
		}

		R_CensusChain( vLight->localInteractions, census.litSurfs, census.litTris, &census.litSurfPx );
		R_CensusChain( vLight->globalInteractions, census.litSurfs, census.litTris, &census.litSurfPx );
		R_CensusChain( vLight->translucentInteractions, census.litTransSurfs, census.litTransTris, &census.litSurfPx );

		const drawSurf_t *shadowChains[2] = { vLight->globalShadows, vLight->localShadows };
		for ( int c = 0; c < 2; c++ ) {
			for ( const drawSurf_t *surf = shadowChains[c]; surf; surf = surf->nextOnLight ) {
				bool inside;
				const int tris = R_CensusShadowIndexes( surf, vLight, inside ) / 3;
				census.shadowSurfs++;
				census.shadowTris += tris;
				if ( inside ) {
					census.shadowTrisInside += tris;
				}
				census.shadowPx += R_CensusRectArea( surf->scissorRect );
			}
		}
	}
}

/*
================
R_CensusCommands

Called with every command list that is issued, before the back end runs.
A frame can issue more than one list.
================
*/
void R_CensusCommands( const emptyCommand_t *cmds ) {
	if ( !r_frameLog.GetString()[0] ) {
		return;
	}
	for ( ; cmds; cmds = (const emptyCommand_t *)cmds->next ) {
		switch ( cmds->commandId ) {
		case RC_DRAW_VIEW:
			R_CensusView( ( (const drawSurfsCommand_t *)cmds )->viewDef );
			break;
		case RC_COPY_RENDER:
			census.copyRenders++;
			break;
		default:
			break;
		}
	}
}

void R_CensusAddFrontEndTicks( double ticks ) {
	censusFrontEndTicks += ticks;
}

void R_CensusAddBackEndTicks( double ticks ) {
	censusBackEndTicks += ticks;
}

/*
================
R_CensusEndFrame

Called once per frame after the last command list was issued
================
*/
void R_CensusEndFrame( void ) {
	const char *name = r_frameLog.GetString();

	if ( censusFile && censusFileName != name ) {
		common->Printf( "r_frameLog: wrote %i rows to %s\n", censusRows, censusFileName.c_str() );
		fileSystem->CloseFile( censusFile );
		censusFile = NULL;
	}

	const double now = Sys_GetClockTicks();

	if ( name[0] ) {
		if ( !censusFile ) {
			censusFile = fileSystem->OpenFileWrite( name );
			censusFileName = name;
			censusRows = 0;
			if ( !censusFile ) {
				common->Warning( "r_frameLog: cannot write %s", name );
				r_frameLog.SetString( "" );
			} else {
				censusFile->Printf( "frameCount,demoFrame,frameMs,frontEndMs,backEndMs,"
					"views3D,views2D,subviews,copyRenders,viewPx,"
					"surfs,tris,depthSurfs,depthTris,perforatedTris,ambientSurfs,ambientTris,ambientStages,"
					"translucentTris,postSurfs,postTris,guiSurfs,guiTris,"
					"lights,fogLights,blendLights,ambientLights,shadowLights,"
					"litSurfs,litTris,litTransSurfs,litTransTris,fogSurfs,fogTris,"
					"shadowSurfs,shadowTris,shadowTrisInside,lightPx,litSurfPx,shadowPx\n" );
			}
		}
		if ( censusFile ) {
			const double toMs = 1000.0 / Sys_ClockTicksPerSecond();
			const frameCensus_t &c = census;
			censusFile->Printf( "%i,%i,%.3f,%.3f,%.3f,"
				"%i,%i,%i,%i,%.0f,"
				"%i,%i,%i,%i,%i,%i,%i,%i,"
				"%i,%i,%i,%i,%i,"
				"%i,%i,%i,%i,%i,"
				"%i,%i,%i,%i,%i,%i,"
				"%i,%i,%i,%.0f,%.0f,%.0f\n",
				tr.frameCount, sessLocal.readDemo ? sessLocal.numDemoFrames : 0,
				censusLastFrameTicks ? ( now - censusLastFrameTicks ) * toMs : 0.0,
				censusFrontEndTicks * toMs, censusBackEndTicks * toMs,
				c.views3D, c.views2D, c.subviews, c.copyRenders, c.viewPx,
				c.surfs, c.tris, c.depthSurfs, c.depthTris, c.perforatedTris, c.ambientSurfs, c.ambientTris, c.ambientStages,
				c.translucentTris, c.postSurfs, c.postTris, c.guiSurfs, c.guiTris,
				c.lights, c.fogLights, c.blendLights, c.ambientLights, c.shadowLights,
				c.litSurfs, c.litTris, c.litTransSurfs, c.litTransTris, c.fogSurfs, c.fogTris,
				c.shadowSurfs, c.shadowTris, c.shadowTrisInside, c.lightPx, c.litSurfPx, c.shadowPx );
			censusRows++;
		}
	}

	censusLastFrameTicks = now;
	censusFrontEndTicks = 0.0;
	censusBackEndTicks = 0.0;
	memset( &census, 0, sizeof( census ) );
}

/*
================
R_CensusShutdown
================
*/
void R_CensusShutdown( void ) {
	if ( censusFile ) {
		fileSystem->CloseFile( censusFile );
		censusFile = NULL;
	}
}
