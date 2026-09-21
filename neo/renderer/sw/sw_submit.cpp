/* sw_submit.cpp -- walks a viewDef in the order RB_STD_DrawView does and turns it into the
   core's segments and draws. Nothing is rasterized here: sw_end_view runs the whole view.

   What the GL back end does with state changes between draws, this does with values inside each
   draw: scissor, cull, polygon offset, depth range, blend, masks. Every GL rectangle (y up,
   relative to the viewport) becomes a framebuffer rectangle (y down) here and nowhere else.

   Clip positions: ONE transform per (vertex array, space). The depth hacks (weapon, model) are
   properties of the space, so the space also decides the projection variant, and every pass of
   a surface reads the same block (invariant I2). */
#include "../../idlib/precompiled.h"
#pragma hdrstop

#include "../tr_local.h"
#include "sw_local.h"

idCVar r_swSpecularTable( "r_swSpecularTable", "0", CVAR_RENDERER | CVAR_BOOL, "specular falloff: 0 = max(4 N.H - 3, 0)^2 with no upper end, as the GL driver of the reference machine renders DOOM 3; 1 = clamped at 1, as interaction.vfp's 256-texel table is written" );

// draw_arb2.cpp (ID_SW_RENDERER): the file name of a program, by which a new-style stage finds its kernel
const char *R_ARBProgramName( int ident );
// tr_render.cpp; not in tr_local.h
void R_SetDrawInteraction( const shaderStage_t *surfaceStage, const float *surfaceRegs, idImage **image, idVec4 matrix[2], float color[4] );

// ---------------------------------------------------------------------------------------------

struct swClipEntry_t {
	const void *		verts;
	const viewEntity_t *space;
	const viewLight_t *	light;		// NULL for surface vertices; shadow.vp's result depends on the light
	const float *		clip;
	int					stamp;
};

static const int SW_CLIP_HASH = 8192;		// demo1's worst frame has about 500 surfaces per view

static struct swView_t {
	const viewDef_t *	viewDef;
	bool				is3D;
	SwRect				viewport;			// framebuffer pixels, y down, not clamped
	SwRect				scissor;			// the view's scissor, clamped to the framebuffer
	int					fbWidth, fbHeight;
	int					stamp;
	bool				captured;			// the view's capture point has been passed: SW_KERN_SCREEN draws may go
	swClipEntry_t		clipHash[SW_CLIP_HASH];
} sw;

/*
================
SW_RectFromGL

A scissor rectangle of the engine: inclusive, y up, relative to the view's viewport.
================
*/
static SwRect SW_RectFromGL( const idScreenRect &r ) {
	const viewDef_t *v = sw.viewDef;
	SwRect out;
	out.x0 = tr.viewportOffset[0] + v->viewport.x1 + r.x1;
	out.x1 = tr.viewportOffset[0] + v->viewport.x1 + r.x2;
	const int yUp0 = tr.viewportOffset[1] + v->viewport.y1 + r.y1;
	const int yUp1 = tr.viewportOffset[1] + v->viewport.y1 + r.y2;
	out.y0 = sw.fbHeight - 1 - yUp1;
	out.y1 = sw.fbHeight - 1 - yUp0;
	return out;
}

static SwRect SW_ClampRect( SwRect r ) {
	if ( r.x0 < 0 ) r.x0 = 0;
	if ( r.y0 < 0 ) r.y0 = 0;
	if ( r.x1 > sw.fbWidth - 1 ) r.x1 = sw.fbWidth - 1;
	if ( r.y1 > sw.fbHeight - 1 ) r.y1 = sw.fbHeight - 1;
	return r;
}

static SwRect SW_SurfaceScissor( const drawSurf_t *surf ) {
	if ( !r_useScissor.GetBool() ) {
		return sw.scissor;
	}
	return SW_ClampRect( SW_RectFromGL( surf->scissorRect ) );
}

/*
================
SW_ClipPositions
================
*/
static const float *SW_TransformFor( const void *verts, int stride, int numVerts, const viewEntity_t *space, const viewLight_t *light ) {
	SwXform x;
	memset( &x, 0, sizeof( x ) );
	x.positions = verts;
	x.stride = stride;
	x.numVerts = numVerts;
	x.kind = SW_XF_POINTS;
	if ( light ) {
		// RB_T_Shadow: the light in the surface's space, for shadow.vp
		idVec3 localLight;
		R_GlobalPointToLocal( space->modelMatrix, light->globalLightOrigin, localLight );
		x.kind = SW_XF_SHADOW;
		x.lightOrigin[0] = localLight[0];
		x.lightOrigin[1] = localLight[1];
		x.lightOrigin[2] = localLight[2];
	}
	float projection[16];
	memcpy( projection, sw.viewDef->projectionMatrix, sizeof( projection ) );
	if ( space->modelDepthHack != 0.0f ) {			// RB_EnterModelDepthHack; entered last, so it wins over the weapon hack
		projection[14] -= space->modelDepthHack;
	} else if ( space->weaponDepthHack ) {			// RB_EnterWeaponDepthHack
		projection[14] *= 0.25f;
	}
	myGlMultMatrix( space->modelViewMatrix, projection, x.mvp );
	return sw_transform( &x );
}

static const float *SW_ClipPositions( const void *verts, int stride, int numVerts, const viewEntity_t *space, const viewLight_t *light = NULL ) {
	unsigned int h = (unsigned int)( ( (size_t)verts >> 4 ) * 2654435761u ) ^ (unsigned int)( ( (size_t)space >> 4 ) * 40503u )
		^ (unsigned int)( ( (size_t)light >> 4 ) * 69069u );
	for ( int probe = 0; probe < 16; probe++ ) {
		swClipEntry_t *e = &sw.clipHash[ ( h + probe ) & ( SW_CLIP_HASH - 1 ) ];
		if ( e->stamp == sw.stamp ) {
			if ( e->verts == verts && e->space == space && e->light == light ) {
				return e->clip;
			}
			continue;
		}
		e->verts = verts;
		e->space = space;
		e->light = light;
		e->clip = SW_TransformFor( verts, stride, numVerts, space, light );
		e->stamp = sw.stamp;
		return e->clip;
	}
	// sixteen collisions in a row: transform again. Same code, same input, same bits.
	return SW_TransformFor( verts, stride, numVerts, space, light );
}

/*
================
SW_BaseDraw

Everything about a draw that comes from the surface and its material, not from the pass.
Returns false when there is nothing to draw.
================
*/
static bool SW_BaseDraw( const drawSurf_t *surf, SwDraw &d ) {
	const srfTriangles_t *tri = surf->geo;
	if ( !tri->numIndexes || !tri->ambientCache || !tri->indexes ) {
		return false;
	}
	const idDrawVert *ac = (const idDrawVert *)vertexCache.Position( tri->ambientCache );

	memset( &d, 0, sizeof( d ) );
	d.clip = SW_ClipPositions( ac, sizeof( idDrawVert ), tri->numVerts, surf->space );
	if ( !d.clip ) {
		return false;
	}
	d.clipStride = 16;
	d.indexes = tri->indexes;
	d.numIndexes = tri->numIndexes;
	d.mirror = sw.viewDef->isMirror;
	d.scissor = SW_SurfaceScissor( surf );
	if ( d.scissor.x0 > d.scissor.x1 || d.scissor.y0 > d.scissor.y1 ) {
		return false;
	}

	switch ( surf->material->GetCullType() ) {
	case CT_TWO_SIDED:		d.cull = SW_CULL_TWO_SIDED; break;
	case CT_BACK_SIDED:		d.cull = SW_CULL_BACK_SIDED; break;
	default:				d.cull = SW_CULL_FRONT_SIDED; break;
	}
	if ( surf->material->TestMaterialFlag( MF_POLYGONOFFSET ) ) {
		d.offsetFactor = r_offsetFactor.GetFloat();
		d.offsetUnits = r_offsetUnits.GetFloat() * surf->material->GetPolygonOffset();
	}
	// RB_EnterWeaponDepthHack: glDepthRange( 0, 0.5 ). The model hack resets the range to 0..1.
	d.depthRangeMax = ( surf->space->weaponDepthHack && surf->space->modelDepthHack == 0.0f ) ? 0.5f : 1.0f;

	d.verts.base = ac;
	d.verts.stride = sizeof( idDrawVert );
	d.verts.ofsXyz = (int)offsetof( idDrawVert, xyz );
	d.verts.ofsSt = (int)offsetof( idDrawVert, st );
	d.verts.ofsNormal = (int)offsetof( idDrawVert, normal );
	d.verts.ofsTangent0 = (int)offsetof( idDrawVert, tangents );
	d.verts.ofsTangent1 = (int)offsetof( idDrawVert, tangents ) + (int)sizeof( idVec3 );
	d.verts.ofsColor = (int)offsetof( idDrawVert, color );

	d.color = 0xFF000000u;
	d.kernel = SW_KERN_FLAT;
	d.srcBlend = SW_BF_ONE;
	d.dstBlend = SW_BF_ZERO;
	d.writeMask = SW_WRITE_RGBA;
	return true;
}

/*
================
SW_SetStateBits

GL_State: blend factors, colour and depth masks, depth function, alpha test.
================
*/
static void SW_SetStateBits( SwDraw &d, SwStageParms *parms, int stateBits ) {
	switch ( stateBits & GLS_SRCBLEND_BITS ) {
	case GLS_SRCBLEND_ZERO:					d.srcBlend = SW_BF_ZERO; break;
	case GLS_SRCBLEND_DST_COLOR:			d.srcBlend = SW_BF_DST_COLOR; break;
	case GLS_SRCBLEND_ONE_MINUS_DST_COLOR:	d.srcBlend = SW_BF_ONE_MINUS_DST_COLOR; break;
	case GLS_SRCBLEND_SRC_ALPHA:			d.srcBlend = SW_BF_SRC_ALPHA; break;
	case GLS_SRCBLEND_ONE_MINUS_SRC_ALPHA:	d.srcBlend = SW_BF_ONE_MINUS_SRC_ALPHA; break;
	case GLS_SRCBLEND_DST_ALPHA:			d.srcBlend = SW_BF_DST_ALPHA; break;
	case GLS_SRCBLEND_ONE_MINUS_DST_ALPHA:	d.srcBlend = SW_BF_ONE_MINUS_DST_ALPHA; break;
	case GLS_SRCBLEND_ALPHA_SATURATE:		d.srcBlend = SW_BF_SRC_ALPHA_SATURATE; break;
	default:								d.srcBlend = SW_BF_ONE; break;
	}
	switch ( stateBits & GLS_DSTBLEND_BITS ) {
	case GLS_DSTBLEND_ONE:					d.dstBlend = SW_BF_ONE; break;
	case GLS_DSTBLEND_SRC_COLOR:			d.dstBlend = SW_BF_SRC_COLOR; break;
	case GLS_DSTBLEND_ONE_MINUS_SRC_COLOR:	d.dstBlend = SW_BF_ONE_MINUS_SRC_COLOR; break;
	case GLS_DSTBLEND_SRC_ALPHA:			d.dstBlend = SW_BF_SRC_ALPHA; break;
	case GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA:	d.dstBlend = SW_BF_ONE_MINUS_SRC_ALPHA; break;
	case GLS_DSTBLEND_DST_ALPHA:			d.dstBlend = SW_BF_DST_ALPHA; break;
	case GLS_DSTBLEND_ONE_MINUS_DST_ALPHA:	d.dstBlend = SW_BF_ONE_MINUS_DST_ALPHA; break;
	default:								d.dstBlend = SW_BF_ZERO; break;
	}

	int mask = 0;
	if ( !( stateBits & GLS_REDMASK ) )		mask |= SW_WRITE_R;
	if ( !( stateBits & GLS_GREENMASK ) )	mask |= SW_WRITE_G;
	if ( !( stateBits & GLS_BLUEMASK ) )	mask |= SW_WRITE_B;
	if ( !( stateBits & GLS_ALPHAMASK ) )	mask |= SW_WRITE_A;
	d.writeMask = mask ? mask : SW_WRITE_NONE;

	if ( !sw.is3D ) {
		// RB_BeginDrawingView disables the depth test for 2D views: no test, no write
		d.depthTest = SW_DEPTH_ALWAYS;
		d.depthWrite = 0;
	} else {
		if ( stateBits & GLS_DEPTHFUNC_ALWAYS ) {
			d.depthTest = SW_DEPTH_ALWAYS;
		} else if ( stateBits & GLS_DEPTHFUNC_EQUAL ) {
			d.depthTest = SW_DEPTH_EQUAL;
		} else {
			d.depthTest = SW_DEPTH_LEQUAL;
		}
		d.depthWrite = ( stateBits & GLS_DEPTHMASK ) ? 0 : 1;
	}

	if ( parms ) {
		switch ( stateBits & GLS_ATEST_BITS ) {
		case GLS_ATEST_EQ_255:	parms->alphaTest = SW_ATEST_EQ_255; break;
		case GLS_ATEST_LT_128:	parms->alphaTest = SW_ATEST_LT_128; break;
		case GLS_ATEST_GE_128:	parms->alphaTest = SW_ATEST_GE_128; break;
		default:				parms->alphaTest = SW_ATEST_NONE; break;
		}
	}
}

/*
================
SW_StageImage

RB_BindVariableStageImage
================
*/
static const SwImage *SW_StageImage( const textureStage_t *texture ) {
	if ( texture->cinematic ) {
		if ( r_skipDynamicTextures.GetBool() ) {
			return SW_ImageForDraw( globalImages->defaultImage );
		}
		cinData_t cin = texture->cinematic->ImageForTime( (int)( 1000 * ( sw.viewDef->floatTime + sw.viewDef->renderView.shaderParms[11] ) ) );
		if ( cin.image ) {
			return SW_ImageTransient( cin.image, cin.imageWidth, cin.imageHeight );
		}
		return SW_ImageForDraw( globalImages->blackImage );
	}
	return SW_ImageForDraw( texture->image );
}

/*
================
SW_StageTexturing

RB_PrepareStageTexturing for the old-style stages. False = a texgen this stage of the project
does not draw yet (screen, glass warp, reflect: Stage 7).
================
*/
static void SW_PlaneFromMatrixRow( const float *m, int row, float out[4] ) {
	out[0] = m[row]; out[1] = m[4 + row]; out[2] = m[8 + row]; out[3] = m[12 + row];
}

/*
================
SW_ReflectStage

TG_REFLECT_CUBE: environment.vfp, or bumpyEnvironment.vfp when the material has a bump stage.
================
*/
static bool SW_ReflectStage( const shaderStage_t *pStage, const drawSurf_t *surf, SwDraw &d, const float color[4] ) {
	SwEnvParms *parms = (SwEnvParms *)sw_view_alloc( sizeof( SwEnvParms ) );
	if ( !parms ) {
		return false;
	}
	memset( parms, 0, sizeof( *parms ) );
	parms->cube = SW_ImageForDraw( pStage->texture.image );
	const shaderStage_t *bumpStage = surf->material->GetBumpStage();
	parms->bump = bumpStage ? SW_ImageForDraw( bumpStage->texture.image ) : NULL;
	if ( !parms->cube ) {
		return false;
	}
	idVec3 localView;
	R_GlobalPointToLocal( surf->space->modelMatrix, sw.viewDef->renderView.vieworg, localView );
	for ( int i = 0; i < 3; i++ ) {
		parms->localViewOrigin[i] = localView[i];
		// program.env[6..8].xyz: rows of the model matrix (RB_SetProgramEnvironmentSpace)
		parms->modelRows[i][0] = surf->space->modelMatrix[0 + i];
		parms->modelRows[i][1] = surf->space->modelMatrix[4 + i];
		parms->modelRows[i][2] = surf->space->modelMatrix[8 + i];
	}
	for ( int i = 0; i < 4; i++ ) {
		parms->color[i] = idMath::ClampFloat( 0.0f, 1.0f, color[i] );
	}
	// with a colour array the program sees the vertex colours as they are: the texture
	// environment that would invert them is not part of a fragment program
	parms->vertexColorModulate = pStage->vertexColor == SVC_IGNORE ? 0.0f : 1.0f;
	d.kernel = SW_KERN_ENV;
	d.kernelParms = parms;
	return true;
}

/*
================
SW_ScreenTexgenStage

TG_SCREEN / TG_SCREEN2: s, t, q = the vertex's clip x, y, w, then the stage's texture matrix.
================
*/
static bool SW_ScreenTexgenStage( const shaderStage_t *pStage, const drawSurf_t *surf, SwDraw &d, const float color[4] ) {
	SwDualParms *parms = (SwDualParms *)sw_view_alloc( sizeof( SwDualParms ) );
	if ( !parms ) {
		return false;
	}
	memset( parms, 0, sizeof( *parms ) );
	parms->image0 = SW_ImageForDraw( pStage->texture.image );
	if ( !parms->image0 ) {
		return false;
	}
	float mat[16];
	myGlMultMatrix( surf->space->modelViewMatrix, sw.viewDef->projectionMatrix, mat );
	idPlane gen[3];
	SW_PlaneFromMatrixRow( mat, 0, gen[0].ToFloatPtr() );
	SW_PlaneFromMatrixRow( mat, 1, gen[1].ToFloatPtr() );
	SW_PlaneFromMatrixRow( mat, 3, gen[2].ToFloatPtr() );
	if ( pStage->texture.hasMatrix ) {
		float texMatrix[16];
		RB_GetShaderTextureMatrix( surf->shaderRegisters, &pStage->texture, texMatrix );
		RB_BakeTextureMatrixIntoTexgen( gen, texMatrix );
	}
	for ( int i = 0; i < 4; i++ ) {
		parms->s0[i] = gen[0][i];
		parms->t0[i] = gen[1][i];
		parms->q0[i] = gen[2][i];
		parms->color[i] = idMath::ClampFloat( 0.0f, 1.0f, color[i] );
	}
	d.kernel = SW_KERN_DUAL;
	d.kernelParms = parms;
	return true;
}

/*
================
SW_NewStage

A new-style stage = an ARB vertex / fragment program pair. The shipped ones that a material can
name are the heat haze family and colorProcess; each is a mode of SW_KERN_SCREEN. They read
_currentRender, so they only draw after the view's capture point.
================
*/
static bool SW_NewStage( const shaderStage_t *pStage, const drawSurf_t *surf, SwDraw &d ) {
	const newShaderStage_t *newStage = pStage->newStage;
	if ( !sw.captured || r_skipNewAmbient.GetBool() ) {
		return false;
	}
	idStr name = R_ARBProgramName( newStage->fragmentProgram );
	name.StripFileExtension();

	int program;
	if ( !name.Icmp( "heatHaze" ) ) {
		program = SW_SCREEN_HEATHAZE;
	} else if ( !name.Icmp( "heatHazeWithMask" ) ) {
		program = SW_SCREEN_HEATHAZE_MASK;
	} else if ( !name.Icmp( "heatHazeWithMaskAndVertex" ) ) {
		program = SW_SCREEN_HEATHAZE_MASK_VERTEX;
	} else if ( !name.Icmp( "colorProcess" ) ) {
		program = SW_SCREEN_COLORPROCESS;
	} else {
		return false;
	}

	SwScreenParms *parms = (SwScreenParms *)sw_view_alloc( sizeof( SwScreenParms ) );
	if ( !parms ) {
		return false;
	}
	memset( parms, 0, sizeof( *parms ) );
	parms->program = program;
	const float *regs = surf->shaderRegisters;
	for ( int i = 0; i < 4; i++ ) {
		parms->parm0[i] = newStage->numVertexParms > 0 ? regs[ newStage->vertexParms[0][i] ] : 0.0f;
		parms->parm1[i] = newStage->numVertexParms > 1 ? regs[ newStage->vertexParms[1][i] ] : 0.0f;
	}
	if ( program != SW_SCREEN_COLORPROCESS ) {
		// texture 0 is _currentRender, 1 the normal map, 2 the mask
		if ( newStage->numFragmentProgramImages < 2 || !newStage->fragmentProgramImages[1] ) {
			return false;
		}
		parms->bump = SW_ImageForDraw( newStage->fragmentProgramImages[1] );
		if ( newStage->numFragmentProgramImages > 2 && newStage->fragmentProgramImages[2] ) {
			parms->mask = SW_ImageForDraw( newStage->fragmentProgramImages[2] );
		}
		if ( !parms->bump || ( program != SW_SCREEN_HEATHAZE && !parms->mask ) ) {
			return false;
		}
	}
	SW_PlaneFromMatrixRow( surf->space->modelViewMatrix, 2, parms->mvRow2 );
	SW_PlaneFromMatrixRow( sw.viewDef->projectionMatrix, 0, parms->projRow0 );
	SW_PlaneFromMatrixRow( sw.viewDef->projectionMatrix, 3, parms->projRow3 );
	d.kernel = SW_KERN_SCREEN;
	d.kernelParms = parms;
	return true;
}

static bool SW_StageTexturing( const shaderStage_t *pStage, const drawSurf_t *surf, SwDraw &d, SwStageParms *parms ) {
	const float *regs = surf->shaderRegisters;

	if ( pStage->privatePolygonOffset ) {
		d.offsetFactor = r_offsetFactor.GetFloat();
		d.offsetUnits = r_offsetUnits.GetFloat() * pStage->privatePolygonOffset;
	}

	// identity
	parms->texMatrix[0][0] = 1.0f; parms->texMatrix[0][1] = 0.0f; parms->texMatrix[0][2] = 0.0f; parms->texMatrix[0][3] = 0.0f;
	parms->texMatrix[1][0] = 0.0f; parms->texMatrix[1][1] = 1.0f; parms->texMatrix[1][2] = 0.0f; parms->texMatrix[1][3] = 0.0f;
	if ( pStage->texture.hasMatrix ) {
		float m[16];
		RB_GetShaderTextureMatrix( regs, &pStage->texture, m );
		parms->texMatrix[0][0] = m[0]; parms->texMatrix[0][1] = m[4]; parms->texMatrix[0][3] = m[12];
		parms->texMatrix[1][0] = m[1]; parms->texMatrix[1][1] = m[5]; parms->texMatrix[1][3] = m[13];
	}

	switch ( pStage->texture.texgen ) {
	case TG_EXPLICIT:
		parms->texgen = SW_TG_EXPLICIT;
		return true;
	case TG_DIFFUSE_CUBE:
		parms->texgen = SW_TG_DIFFUSE_CUBE;
		return true;
	case TG_SKYBOX_CUBE:
	case TG_WOBBLESKY_CUBE:
		if ( !surf->dynamicTexCoords ) {
			return false;
		}
		parms->texgen = SW_TG_DYNAMIC3;
		d.verts.texCoords3 = (const float *)vertexCache.Position( surf->dynamicTexCoords );
		d.verts.texCoords3Stride = 3 * sizeof( float );
		return true;
	default:
		return false;
	}
}

/*
================
SW_FillDepthBuffer

RB_STD_FillDepthBuffer / RB_T_FillDepthBuffer: opaque surfaces draw black, perforated ones once
per alpha-tested stage with GL_GREATER against the stage's register.
================
*/
static void SW_FillDepthBuffer( drawSurf_t **drawSurfs, int numDrawSurfs ) {
	sw_begin_segment( SW_SEG_DEPTH, NULL );

	for ( int i = 0; i < numDrawSurfs; i++ ) {
		const drawSurf_t *surf = drawSurfs[i];
		const idMaterial *shader = surf->material;
		const float *regs = surf->shaderRegisters;

		if ( !shader->IsDrawn() || shader->Coverage() == MC_TRANSLUCENT ) {
			continue;
		}
		// if all stages of a material have been conditioned off, don't do anything
		int stage;
		for ( stage = 0; stage < shader->GetNumStages(); stage++ ) {
			if ( regs[ shader->GetStage( stage )->conditionRegister ] != 0 ) {
				break;
			}
		}
		if ( stage == shader->GetNumStages() ) {
			continue;
		}

		SwDraw d;
		if ( !SW_BaseDraw( surf, d ) ) {
			continue;
		}
		d.op = SW_OP_DEPTH_FILL;
		d.color = 0xFF000000u;
		if ( shader->GetSort() == SS_SUBVIEW ) {
			// GL modulates the colour buffer by 1 / overBright = 1 here: the subview's picture stays, depth is written
			d.writeMask = SW_WRITE_NONE;
		}

		// a mirror view: nothing behind the mirror plane may enter the depth buffer. GL kills those
		// fragments with an alpha-tested notch texture on unit 1; here the stage kernel's clip varying does.
		float clipPlane[4] = { 0, 0, 0, 0 };
		const bool hasClip = sw.viewDef->numClipPlanes > 0;
		if ( hasClip ) {
			idPlane plane;
			R_GlobalPlaneToLocal( surf->space->modelMatrix, sw.viewDef->clipPlanes[0], plane );
			clipPlane[0] = plane[0]; clipPlane[1] = plane[1]; clipPlane[2] = plane[2]; clipPlane[3] = plane[3];
		}

		bool drawSolid = shader->Coverage() == MC_OPAQUE;
		if ( shader->Coverage() == MC_PERFORATED ) {
			bool didDraw = false;
			for ( stage = 0; stage < shader->GetNumStages(); stage++ ) {
				const shaderStage_t *pStage = shader->GetStage( stage );
				if ( !pStage->hasAlphaTest || regs[ pStage->conditionRegister ] == 0 ) {
					continue;
				}
				// if we at least tried to draw an alpha tested stage, we won't draw the opaque surface
				didDraw = true;
				const float alpha = regs[ pStage->color.registers[3] ];
				if ( alpha <= 0 ) {
					continue;
				}
				SwStageParms *parms = (SwStageParms *)sw_view_alloc( sizeof( SwStageParms ) );
				if ( !parms ) {
					continue;
				}
				memset( parms, 0, sizeof( *parms ) );
				SwDraw sd = d;
				if ( !SW_StageTexturing( pStage, surf, sd, parms ) ) {
					continue;
				}
				parms->image = SW_ImageForDraw( pStage->texture.image );
				parms->color[0] = parms->color[1] = parms->color[2] = 0.0f;
				parms->color[3] = alpha > 1.0f ? 1.0f : alpha;
				parms->vertexColorModulate = 0.0f;
				parms->vertexColorAdd = 1.0f;
				parms->alphaTest = SW_ATEST_GT_REF;
				parms->alphaRef = regs[ pStage->alphaTestRegister ];
				parms->hasClip = hasClip;
				memcpy( parms->clipPlane, clipPlane, sizeof( clipPlane ) );
				sd.kernel = SW_KERN_STAGE;
				sd.kernelParms = parms;
				sw_draw( &sd );
			}
			if ( !didDraw ) {
				drawSolid = true;
			}
		}
		if ( drawSolid ) {
			if ( hasClip ) {
				SwStageParms *parms = (SwStageParms *)sw_view_alloc( sizeof( SwStageParms ) );
				if ( !parms ) {
					continue;
				}
				memset( parms, 0, sizeof( *parms ) );
				parms->texMatrix[0][0] = 1.0f;
				parms->texMatrix[1][1] = 1.0f;
				parms->color[3] = 1.0f;
				parms->vertexColorAdd = 1.0f;
				parms->hasClip = 1;
				memcpy( parms->clipPlane, clipPlane, sizeof( clipPlane ) );
				d.kernel = SW_KERN_STAGE;
				d.kernelParms = parms;
			}
			sw_draw( &d );
		}
	}
}

/*
================
SW_RenderShaderPasses

RB_STD_T_RenderShaderPasses, old-style stages. New-style stages (ARB programs: heat haze and
friends) and the program texgens are Stage 7.
================
*/
static void SW_RenderShaderPasses( const drawSurf_t *surf ) {
	const idMaterial *shader = surf->material;

	if ( !shader->HasAmbient() || shader->IsPortalSky() ) {
		return;
	}
	const float *regs = surf->shaderRegisters;

	SwDraw base;
	bool haveBase = false;

	for ( int stage = 0; stage < shader->GetNumStages(); stage++ ) {
		const shaderStage_t *pStage = shader->GetStage( stage );

		if ( regs[ pStage->conditionRegister ] == 0 ) {
			continue;
		}
		if ( pStage->lighting != SL_AMBIENT ) {
			continue;
		}
		const int blendBits = pStage->drawStateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS );
		// skip the stages that are ( GL_ZERO, GL_ONE ), which is used for some alpha masks
		if ( blendBits == ( GLS_SRCBLEND_ZERO | GLS_DSTBLEND_ONE ) ) {
			continue;
		}
		if ( pStage->newStage ) {
			if ( !haveBase ) {
				if ( !SW_BaseDraw( surf, base ) ) {
					return;
				}
				haveBase = true;
			}
			SwDraw nd = base;
			nd.op = SW_OP_COLOR;
			if ( SW_NewStage( pStage, surf, nd ) ) {
				SW_SetStateBits( nd, NULL, pStage->drawStateBits );
				sw_draw( &nd );
			}
			continue;
		}

		float color[4];
		color[0] = regs[ pStage->color.registers[0] ];
		color[1] = regs[ pStage->color.registers[1] ];
		color[2] = regs[ pStage->color.registers[2] ];
		color[3] = regs[ pStage->color.registers[3] ];

		// skip the entire stage if an add would be black
		if ( blendBits == ( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE ) && color[0] <= 0 && color[1] <= 0 && color[2] <= 0 ) {
			continue;
		}
		// skip the entire stage if a blend would be completely transparent
		if ( blendBits == ( GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA ) && color[3] <= 0 ) {
			continue;
		}

		if ( !haveBase ) {
			if ( !SW_BaseDraw( surf, base ) ) {
				return;
			}
			haveBase = true;
		}

		SwStageParms *parms = (SwStageParms *)sw_view_alloc( sizeof( SwStageParms ) );
		if ( !parms ) {
			return;
		}
		memset( parms, 0, sizeof( *parms ) );

		SwDraw d = base;
		d.op = SW_OP_COLOR;
		d.kernel = SW_KERN_STAGE;
		d.kernelParms = parms;
		if ( pStage->texture.texgen == TG_REFLECT_CUBE || pStage->texture.texgen == TG_SCREEN || pStage->texture.texgen == TG_SCREEN2 ) {
			// the texgens that are programs of their own
			if ( pStage->privatePolygonOffset ) {
				d.offsetFactor = r_offsetFactor.GetFloat();
				d.offsetUnits = r_offsetUnits.GetFloat() * pStage->privatePolygonOffset;
			}
			const bool ok = pStage->texture.texgen == TG_REFLECT_CUBE ? SW_ReflectStage( pStage, surf, d, color )
																	   : SW_ScreenTexgenStage( pStage, surf, d, color );
			if ( ok ) {
				SW_SetStateBits( d, NULL, pStage->drawStateBits );
				sw_draw( &d );
			}
			continue;
		}
		if ( !SW_StageTexturing( pStage, surf, d, parms ) ) {
			continue;
		}
		parms->image = SW_StageImage( &pStage->texture );
		if ( !parms->image ) {
			continue;
		}
		parms->screenAligned = !sw.is3D;
		// glColor and GL_TEXTURE_ENV_COLOR clamp to 0..1
		for ( int k = 0; k < 4; k++ ) {
			parms->color[k] = idMath::ClampFloat( 0.0f, 1.0f, color[k] );
		}
		switch ( pStage->vertexColor ) {
		case SVC_MODULATE:			parms->vertexColorModulate = 1.0f; parms->vertexColorAdd = 0.0f; break;
		case SVC_INVERSE_MODULATE:	parms->vertexColorModulate = -1.0f; parms->vertexColorAdd = 1.0f; break;
		default:					parms->vertexColorModulate = 0.0f; parms->vertexColorAdd = 1.0f; break;
		}
		SW_SetStateBits( d, parms, pStage->drawStateBits );
		sw_draw( &d );
	}
}

/*
================
SW_DrawShaderPasses

RB_STD_DrawShaderPasses. Returns the number of surfaces processed: the walk stops at the first
post-process surface (it needs _currentRender: a sync point, Stage 7).
================
*/
static int SW_DrawShaderPasses( drawSurf_t **drawSurfs, int numDrawSurfs ) {
	if ( sw.is3D && r_skipAmbient.GetBool() ) {
		return numDrawSurfs;
	}
	sw_begin_segment( SW_SEG_COLOR, NULL );

	int i;
	for ( i = 0; i < numDrawSurfs; i++ ) {
		const drawSurf_t *surf = drawSurfs[i];
		if ( surf->material->SuppressInSubview() ) {
			continue;
		}
		if ( sw.viewDef->isXraySubview && surf->space->entityDef ) {
			if ( surf->space->entityDef->parms.xrayIndex != 2 ) {
				continue;
			}
		}
		// post-process surfaces wait for the capture of everything else, fog included
		if ( surf->material->GetSort() >= SS_POST_PROCESS && !sw.captured ) {
			break;
		}
		SW_RenderShaderPasses( surf );
	}
	return i;
}

/*
=============================================================================================

LIGHTS: RB_ARB2_DrawInteractions, RB_CreateSingleDrawInteractions, RB_StencilShadowPass

=============================================================================================
*/

static SwRect SW_IntersectRects( const SwRect &a, const SwRect &b ) {
	SwRect r;
	r.x0 = Max( a.x0, b.x0 );
	r.y0 = Max( a.y0, b.y0 );
	r.x1 = Min( a.x1, b.x1 );
	r.y1 = Min( a.y1, b.y1 );
	return r;
}

// what one light's draws share
struct swLightPass_t {
	const viewLight_t *	vLight;
	bool				stencilTest;	// the light has shadows: pass where the count is at most 128
	int					depthTest;		// EQUAL for opaque surfaces, LEQUAL for translucent ones
	SwRect				lightScissor;	// every stencil-tested draw stays inside the rectangle that was cleared
	float				ambientVector[3];
};

/*
================
SW_SubmitInteraction

RB_SubmittInteraction + RB_ARB2_DrawInteraction
================
*/
static void SW_SubmitInteraction( const swLightPass_t &pass, const SwDraw &base, drawInteraction_t *din ) {
	if ( !din->bumpImage ) {
		return;
	}
	if ( !din->diffuseImage || r_skipDiffuse.GetBool() ) {
		din->diffuseImage = globalImages->blackImage;
	}
	if ( !din->specularImage || r_skipSpecular.GetBool() || din->ambientLight ) {
		din->specularImage = globalImages->blackImage;
	}
	if ( !din->bumpImage || r_skipBump.GetBool() ) {
		din->bumpImage = globalImages->flatNormalMap;
	}
	// if we wouldn't draw anything, don't draw
	const bool diffuse = ( din->diffuseColor[0] > 0 || din->diffuseColor[1] > 0 || din->diffuseColor[2] > 0 ) && din->diffuseImage != globalImages->blackImage;
	const bool specular = ( din->specularColor[0] > 0 || din->specularColor[1] > 0 || din->specularColor[2] > 0 ) && din->specularImage != globalImages->blackImage;
	if ( !diffuse && !specular ) {
		return;
	}

	SwInteractionParms *parms = (SwInteractionParms *)sw_view_alloc( sizeof( SwInteractionParms ) );
	if ( !parms ) {
		return;
	}
	memset( parms, 0, sizeof( *parms ) );
	for ( int i = 0; i < 3; i++ ) {
		parms->localLightOrigin[i] = din->localLightOrigin[i];
		parms->localViewOrigin[i] = din->localViewOrigin[i];
		parms->ambientVector[i] = pass.ambientVector[i];
	}
	for ( int i = 0; i < 4; i++ ) {
		parms->lightProjectS[i] = din->lightProjection[0][i];
		parms->lightProjectT[i] = din->lightProjection[1][i];
		parms->lightProjectQ[i] = din->lightProjection[2][i];
		parms->lightFalloffS[i] = din->lightProjection[3][i];
		parms->diffuseColor[i] = din->diffuseColor[i];
		parms->specularColor[i] = din->specularColor[i];
		for ( int k = 0; k < 2; k++ ) {
			parms->bumpMatrix[k][i] = din->bumpMatrix[k][i];
			parms->diffuseMatrix[k][i] = din->diffuseMatrix[k][i];
			parms->specularMatrix[k][i] = din->specularMatrix[k][i];
		}
	}
	switch ( din->vertexColor ) {
	case SVC_MODULATE:			parms->vertexColorModulate = 1.0f; parms->vertexColorAdd = 0.0f; break;
	case SVC_INVERSE_MODULATE:	parms->vertexColorModulate = -1.0f; parms->vertexColorAdd = 1.0f; break;
	default:					parms->vertexColorModulate = 0.0f; parms->vertexColorAdd = 1.0f; break;
	}
	parms->ambientLight = din->ambientLight;
	// r_swSpecularTable 1: the table of interaction.vfp, which ends at 1. 0: the formula without the
	// upper end, which is what the reference GPU's driver renders with the stock table (ledger).
	parms->specularMax = r_swSpecularTable.GetBool() ? 1.0f : 1.0e6f;
	parms->bump = SW_ImageForDraw( din->bumpImage );
	parms->diffuse = SW_ImageForDraw( din->diffuseImage );
	parms->specular = SW_ImageForDraw( din->specularImage );
	parms->falloff = SW_ImageForDraw( din->lightFalloffImage );
	parms->projection = SW_ImageForDraw( din->lightImage );
	if ( !parms->bump || !parms->diffuse || !parms->specular || !parms->falloff || !parms->projection ) {
		return;
	}

	SwDraw d = base;
	d.op = SW_OP_COLOR;
	d.kernel = SW_KERN_INTERACTION;
	d.kernelParms = parms;
	d.srcBlend = SW_BF_ONE;				// GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHMASK | backEnd.depthFunc
	d.dstBlend = SW_BF_ONE;
	d.writeMask = SW_WRITE_RGBA;
	d.depthTest = pass.depthTest;
	d.depthWrite = 0;
	d.stencilTest = pass.stencilTest ? 1 : 0;
	// RB_CreateSingleDrawInteractions never enables polygon offset
	d.offsetFactor = 0.0f;
	d.offsetUnits = 0.0f;
	sw_draw( &d );
}

/*
================
SW_CreateSingleDrawInteractions

RB_CreateSingleDrawInteractions without its GL calls: decomposes a light / surface pair into
(bump, diffuse, specular) triples, one draw each.
================
*/
static void SW_CreateSingleDrawInteractions( const swLightPass_t &pass, const drawSurf_t *surf ) {
	const idMaterial	*surfaceShader = surf->material;
	const float			*surfaceRegs = surf->shaderRegisters;
	const viewLight_t	*vLight = pass.vLight;
	const idMaterial	*lightShader = vLight->lightShader;
	const float			*lightRegs = vLight->shaderRegisters;
	drawInteraction_t	inter;

	if ( r_skipInteractions.GetBool() || !surf->geo || !surf->geo->ambientCache ) {
		return;
	}

	SwDraw base;
	if ( !SW_BaseDraw( surf, base ) ) {
		return;
	}
	if ( pass.stencilTest ) {
		base.scissor = SW_IntersectRects( base.scissor, pass.lightScissor );
		if ( base.scissor.x0 > base.scissor.x1 || base.scissor.y0 > base.scissor.y1 ) {
			return;
		}
	}

	inter.surf = surf;
	inter.lightFalloffImage = vLight->falloffImage;

	R_GlobalPointToLocal( surf->space->modelMatrix, vLight->globalLightOrigin, inter.localLightOrigin.ToVec3() );
	R_GlobalPointToLocal( surf->space->modelMatrix, sw.viewDef->renderView.vieworg, inter.localViewOrigin.ToVec3() );
	inter.localLightOrigin[3] = 0;
	inter.localViewOrigin[3] = 1;
	inter.ambientLight = lightShader->IsAmbientLight();

	// the base projections may be modified by texture matrix on light stages
	idPlane lightProject[4];
	for ( int i = 0; i < 4; i++ ) {
		R_GlobalPlaneToLocal( surf->space->modelMatrix, vLight->lightProject[i], lightProject[i] );
	}

	for ( int lightStageNum = 0; lightStageNum < lightShader->GetNumStages(); lightStageNum++ ) {
		const shaderStage_t *lightStage = lightShader->GetStage( lightStageNum );

		// ignore stages that fail the condition
		if ( !lightRegs[ lightStage->conditionRegister ] ) {
			continue;
		}

		inter.lightImage = lightStage->texture.image;

		memcpy( inter.lightProjection, lightProject, sizeof( inter.lightProjection ) );
		// now multiply the texgen by the light texture matrix
		if ( lightStage->texture.hasMatrix ) {
			float lightTextureMatrix[16];
			RB_GetShaderTextureMatrix( lightRegs, &lightStage->texture, lightTextureMatrix );
			memcpy( backEnd.lightTextureMatrix, lightTextureMatrix, sizeof( lightTextureMatrix ) );
			RB_BakeTextureMatrixIntoTexgen( reinterpret_cast<class idPlane *>(inter.lightProjection), backEnd.lightTextureMatrix );
		}

		inter.bumpImage = NULL;
		inter.specularImage = NULL;
		inter.diffuseImage = NULL;
		inter.diffuseColor[0] = inter.diffuseColor[1] = inter.diffuseColor[2] = inter.diffuseColor[3] = 0;
		inter.specularColor[0] = inter.specularColor[1] = inter.specularColor[2] = inter.specularColor[3] = 0;

		float lightColor[4];
		lightColor[0] = backEnd.lightScale * lightRegs[ lightStage->color.registers[0] ];
		lightColor[1] = backEnd.lightScale * lightRegs[ lightStage->color.registers[1] ];
		lightColor[2] = backEnd.lightScale * lightRegs[ lightStage->color.registers[2] ];
		lightColor[3] = lightRegs[ lightStage->color.registers[3] ];

		// go through the individual stages
		for ( int surfaceStageNum = 0; surfaceStageNum < surfaceShader->GetNumStages(); surfaceStageNum++ ) {
			const shaderStage_t *surfaceStage = surfaceShader->GetStage( surfaceStageNum );

			switch ( surfaceStage->lighting ) {
				case SL_AMBIENT: {
					// ignore ambient stages while drawing interactions
					break;
				}
				case SL_BUMP: {
					if ( !surfaceRegs[ surfaceStage->conditionRegister ] ) {
						break;
					}
					// draw any previous interaction
					SW_SubmitInteraction( pass, base, &inter );
					inter.diffuseImage = NULL;
					inter.specularImage = NULL;
					R_SetDrawInteraction( surfaceStage, surfaceRegs, &inter.bumpImage, inter.bumpMatrix, NULL );
					break;
				}
				case SL_DIFFUSE: {
					if ( !surfaceRegs[ surfaceStage->conditionRegister ] ) {
						break;
					}
					if ( inter.diffuseImage ) {
						SW_SubmitInteraction( pass, base, &inter );
					}
					R_SetDrawInteraction( surfaceStage, surfaceRegs, &inter.diffuseImage, inter.diffuseMatrix, inter.diffuseColor.ToFloatPtr() );
					inter.diffuseColor[0] *= lightColor[0];
					inter.diffuseColor[1] *= lightColor[1];
					inter.diffuseColor[2] *= lightColor[2];
					inter.diffuseColor[3] *= lightColor[3];
					inter.vertexColor = surfaceStage->vertexColor;
					break;
				}
				case SL_SPECULAR: {
					if ( !surfaceRegs[ surfaceStage->conditionRegister ] ) {
						break;
					}
					if ( inter.specularImage ) {
						SW_SubmitInteraction( pass, base, &inter );
					}
					R_SetDrawInteraction( surfaceStage, surfaceRegs, &inter.specularImage, inter.specularMatrix, inter.specularColor.ToFloatPtr() );
					inter.specularColor[0] *= lightColor[0];
					inter.specularColor[1] *= lightColor[1];
					inter.specularColor[2] *= lightColor[2];
					inter.specularColor[3] *= lightColor[3];
					inter.vertexColor = surfaceStage->vertexColor;
					break;
				}
			}
		}

		// draw the final interaction
		SW_SubmitInteraction( pass, base, &inter );
	}
}

static void SW_InteractionChain( const swLightPass_t &pass, const drawSurf_t *surf ) {
	if ( !surf ) {
		return;
	}
	sw_begin_segment( SW_SEG_LIGHT, NULL );
	for ( ; surf; surf = surf->nextOnLight ) {
		SW_CreateSingleDrawInteractions( pass, surf );
	}
}

/*
================
SW_StencilShadowPass

RB_StencilShadowPass / RB_T_Shadow. The GL code draws a volume twice (front faces, back faces)
or four times (with the depth-fail preload); the core counts both sides in ONE pass per volume:
SW_OP_STENCIL_ZPASS = front +1 / back -1 where the depth test passes, SW_OP_STENCIL_ZFAIL =
front -1 / back +1 where it fails, which is what the preload and the depth-pass pair add up to.
================
*/
static void SW_StencilShadowPass( const swLightPass_t &pass, const drawSurf_t *drawSurfs, const SwRect *stencilClear ) {
	if ( !r_shadows.GetBool() || !drawSurfs ) {
		if ( stencilClear ) {
			sw_begin_segment( SW_SEG_SHADOW, stencilClear );	// the clear must happen even when nothing is drawn
		}
		return;
	}
	sw_begin_segment( SW_SEG_SHADOW, stencilClear );

	for ( const drawSurf_t *surf = drawSurfs; surf; surf = surf->nextOnLight ) {
		const srfTriangles_t *tri = surf->geo;
		if ( !tri->shadowCache || !tri->indexes ) {
			continue;
		}

		// we always draw the sil planes, but we may not need to draw the front or rear caps
		int		numIndexes;
		bool	external = false;
		if ( !r_useExternalShadows.GetInteger() ) {
			numIndexes = tri->numIndexes;
		} else if ( r_useExternalShadows.GetInteger() == 2 ) {		// force to no caps for testing
			numIndexes = tri->numShadowIndexesNoCaps;
		} else if ( !( surf->dsFlags & DSF_VIEW_INSIDE_SHADOW ) ) {
			// if we aren't inside the shadow projection, no caps are ever needed needed
			numIndexes = tri->numShadowIndexesNoCaps;
			external = true;
		} else if ( !pass.vLight->viewInsideLight && !( tri->shadowCapPlaneBits & SHADOW_CAP_INFINITE ) ) {
			// if we are inside the shadow projection, but outside the light, and drawing
			// a non-infinite shadow, we can skip some caps
			if ( pass.vLight->viewSeesShadowPlaneBits & tri->shadowCapPlaneBits ) {
				// we can see through a rear cap, so we need to draw it, but we can skip the
				// caps on the actual surface
				numIndexes = tri->numShadowIndexesNoFrontCaps;
			} else {
				// we don't need to draw any caps
				numIndexes = tri->numShadowIndexesNoCaps;
			}
			external = true;
		} else {
			// must draw everything
			numIndexes = tri->numIndexes;
		}
		if ( numIndexes < 3 ) {
			continue;
		}

		const void *verts = vertexCache.Position( tri->shadowCache );
		const int numVerts = tri->shadowCache->size / (int)sizeof( shadowCache_t );

		SwDraw d;
		memset( &d, 0, sizeof( d ) );
		d.clip = SW_ClipPositions( verts, sizeof( shadowCache_t ), numVerts, surf->space, pass.vLight );
		if ( !d.clip ) {
			continue;
		}
		d.clipStride = 16;
		d.indexes = tri->indexes;
		d.numIndexes = numIndexes;
		d.op = external ? SW_OP_STENCIL_ZPASS : SW_OP_STENCIL_ZFAIL;
		d.cull = SW_CULL_TWO_SIDED;
		d.mirror = sw.viewDef->isMirror;
		d.scissor = SW_IntersectRects( SW_SurfaceScissor( surf ), pass.lightScissor );
		if ( d.scissor.x0 > d.scissor.x1 || d.scissor.y0 > d.scissor.y1 ) {
			continue;
		}
		// qglPolygonOffset( r_shadowPolygonFactor, -r_shadowPolygonOffset )
		d.offsetFactor = r_shadowPolygonFactor.GetFloat();
		d.offsetUnits = -r_shadowPolygonOffset.GetFloat();
		d.depthRangeMax = ( surf->space->weaponDepthHack && surf->space->modelDepthHack == 0.0f ) ? 0.5f : 1.0f;
		d.depthTest = SW_DEPTH_LEQUAL;
		// qglDepthBoundsEXT( surf->scissorRect.zmin, surf->scissorRect.zmax ): pixels whose stored depth is
		// outside the light's range keep their count. They cannot be lit by this light, so the picture does
		// not change; the rasterizer skips whole cells and tiles with it.
		if ( r_useDepthBoundsTest.GetBool() ) {
			d.depthBoundsMin = surf->scissorRect.zmin;
			d.depthBoundsMax = surf->scissorRect.zmax;
		}
		sw_draw( &d );
	}
}

/*
================
SW_DrawInteractions

RB_ARB2_DrawInteractions: per light, the stencil clear, global shadows, local interactions,
local shadows, global interactions, then the translucent interactions without the stencil.
================
*/
static void SW_DrawInteractions( void ) {
	// (byte)( 255 * v ) and back, as the ambient normal cube map stores it; x sits in alpha there,
	// and interaction.vfp reads .xyz of the texel: red is the 255 that was written as "alpha"
	float ambientVector[3];
	ambientVector[0] = 1.0f;
	ambientVector[1] = (float)(byte)( 255 * tr.ambientLightVector[1] ) * ( 2.0f / 255.0f ) - 1.0f;
	ambientVector[2] = (float)(byte)( 255 * tr.ambientLightVector[2] ) * ( 2.0f / 255.0f ) - 1.0f;

	for ( const viewLight_t *vLight = sw.viewDef->viewLights; vLight; vLight = vLight->next ) {
		// do fogging later
		if ( vLight->lightShader->IsFogLight() || vLight->lightShader->IsBlendLight() ) {
			continue;
		}
		if ( !vLight->localInteractions && !vLight->globalInteractions && !vLight->translucentInteractions ) {
			continue;
		}

		swLightPass_t pass;
		pass.vLight = vLight;
		pass.stencilTest = ( vLight->globalShadows || vLight->localShadows ) && r_shadows.GetBool();
		pass.depthTest = SW_DEPTH_EQUAL;
		pass.lightScissor = r_useScissor.GetBool() ? SW_ClampRect( SW_RectFromGL( vLight->scissorRect ) ) : sw.scissor;
		pass.ambientVector[0] = ambientVector[0];
		pass.ambientVector[1] = ambientVector[1];
		pass.ambientVector[2] = ambientVector[2];

		backEnd.vLight = const_cast<viewLight_t *>( vLight );

		if ( pass.stencilTest ) {
			if ( pass.lightScissor.x0 > pass.lightScissor.x1 || pass.lightScissor.y0 > pass.lightScissor.y1 ) {
				continue;
			}
			SW_StencilShadowPass( pass, vLight->globalShadows, &pass.lightScissor );
			SW_InteractionChain( pass, vLight->localInteractions );
			SW_StencilShadowPass( pass, vLight->localShadows, NULL );
			SW_InteractionChain( pass, vLight->globalInteractions );
		} else {
			SW_InteractionChain( pass, vLight->localInteractions );
			SW_InteractionChain( pass, vLight->globalInteractions );
		}

		// translucent surfaces never get stencil shadowed
		if ( r_skipTranslucent.GetBool() ) {
			continue;
		}
		pass.stencilTest = false;
		pass.depthTest = SW_DEPTH_LEQUAL;
		SW_InteractionChain( pass, vLight->translucentInteractions );
	}
}

/*
=============================================================================================

FOG AND BLEND LIGHTS: RB_STD_FogAllLights

Both are two textures times a colour with coordinates linear in object space (SW_KERN_DUAL).
GL leaves the cull at CT_FRONT_SIDED for the chains, whatever the material says.

=============================================================================================
*/

static void SW_DualChain( const drawSurf_t *surf, const SwDualParms &world, const idPlane globalPlanes[5], const float add[5], int stateBits ) {
	for ( ; surf; surf = surf->nextOnLight ) {
		SwDraw d;
		if ( !SW_BaseDraw( surf, d ) ) {
			continue;
		}
		SwDualParms *parms = (SwDualParms *)sw_view_alloc( sizeof( SwDualParms ) );
		if ( !parms ) {
			return;
		}
		*parms = world;
		float *dst[5] = { parms->s0, parms->t0, parms->q0, parms->s1, parms->t1 };
		for ( int k = 0; k < 5; k++ ) {
			idPlane local;
			R_GlobalPlaneToLocal( surf->space->modelMatrix, globalPlanes[k], local );
			dst[k][0] = local[0]; dst[k][1] = local[1]; dst[k][2] = local[2]; dst[k][3] = local[3] + add[k];
		}
		d.op = SW_OP_COLOR;
		d.kernel = SW_KERN_DUAL;
		d.kernelParms = parms;
		d.cull = SW_CULL_FRONT_SIDED;
		d.offsetFactor = 0.0f;
		d.offsetUnits = 0.0f;
		SW_SetStateBits( d, NULL, stateBits );
		sw_draw( &d );
	}
}

/*
================
SW_BlendLight

RB_BlendLight: projection x falloff x the stage colour, blended as the light stage says, depth EQUAL
================
*/
static void SW_BlendLight( const viewLight_t *vLight ) {
	if ( r_skipBlendLights.GetBool() || ( !vLight->globalInteractions && !vLight->localInteractions ) ) {
		return;
	}
	const idMaterial *lightShader = vLight->lightShader;
	const float *regs = vLight->shaderRegisters;

	for ( int i = 0; i < lightShader->GetNumStages(); i++ ) {
		const shaderStage_t *stage = lightShader->GetStage( i );
		if ( !regs[ stage->conditionRegister ] ) {
			continue;
		}
		SwDualParms world;
		memset( &world, 0, sizeof( world ) );
		world.image0 = SW_ImageForDraw( stage->texture.image );
		world.image1 = SW_ImageForDraw( vLight->falloffImage );
		if ( !world.image0 || !world.image1 ) {
			continue;
		}
		for ( int k = 0; k < 4; k++ ) {
			world.color[k] = idMath::ClampFloat( 0.0f, 1.0f, regs[ stage->color.registers[k] ] );
		}
		// S, T, Q of the projection (times the stage's texture matrix), then the falloff's S; its T is 0.5
		idPlane planes[5];
		planes[0] = vLight->lightProject[0];
		planes[1] = vLight->lightProject[1];
		planes[2] = vLight->lightProject[2];
		planes[3] = vLight->lightProject[3];
		planes[4].Zero();
		planes[4][3] = 0.0f;
		if ( stage->texture.hasMatrix ) {
			float texMatrix[16];
			RB_GetShaderTextureMatrix( regs, &stage->texture, texMatrix );
			RB_BakeTextureMatrixIntoTexgen( planes, texMatrix );
		}
		const float add[5] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.5f };
		const int stateBits = GLS_DEPTHMASK | stage->drawStateBits | GLS_DEPTHFUNC_EQUAL;
		SW_DualChain( vLight->globalInteractions, world, planes, add, stateBits );
		SW_DualChain( vLight->localInteractions, world, planes, add, stateBits );
	}
}

/*
================
SW_FogPass

RB_FogPass: the fog image along the view depth, times the enter image that fades the fog in at
the light's top plane; then the back faces of the light frustum, which are not in the depth buffer.
================
*/
static void SW_FogPass( const viewLight_t *vLight ) {
	const srfTriangles_t *frustumTris = vLight->frustumTris;
	if ( !frustumTris->ambientCache ) {
		return;
	}
	const float *regs = vLight->shaderRegisters;
	const shaderStage_t *stage = vLight->lightShader->GetStage( 0 );		// fog shaders have a single stage

	SwDualParms world;
	memset( &world, 0, sizeof( world ) );
	world.image0 = SW_ImageForDraw( globalImages->fogImage );
	world.image1 = SW_ImageForDraw( globalImages->fogEnterImage );
	if ( !world.image0 || !world.image1 ) {
		return;
	}
	const float alpha = regs[ stage->color.registers[3] ];
	world.color[0] = idMath::ClampFloat( 0.0f, 1.0f, regs[ stage->color.registers[0] ] );
	world.color[1] = idMath::ClampFloat( 0.0f, 1.0f, regs[ stage->color.registers[1] ] );
	world.color[2] = idMath::ClampFloat( 0.0f, 1.0f, regs[ stage->color.registers[2] ] );
	world.color[3] = 1.0f;		// qglColor3fv

	// if they left the default value on, set a fog distance of 500; otherwise, distance = alpha color
	const float a = alpha <= 1.0f ? -0.5f / DEFAULT_FOG_DISTANCE : -0.5f / alpha;

	const float *mv = sw.viewDef->worldSpace.modelViewMatrix;
	idPlane planes[5];
	// unit 0: S along the view depth (+ 0.5), T = 0.5, Q = 1
	planes[0][0] = a * mv[2]; planes[0][1] = a * mv[6]; planes[0][2] = a * mv[10]; planes[0][3] = a * mv[14];
	planes[1].Zero(); planes[1][3] = 0.0f;
	planes[2].Zero(); planes[2][3] = 0.0f;
	// unit 1: S is constant per viewer, T rises from the fade plane
	idPlane fade;
	fade[0] = 0.001f * vLight->fogPlane[0]; fade[1] = 0.001f * vLight->fogPlane[1];
	fade[2] = 0.001f * vLight->fogPlane[2]; fade[3] = 0.001f * vLight->fogPlane[3];
	const float s = sw.viewDef->renderView.vieworg * fade.Normal() + fade[3];
	planes[3].Zero(); planes[3][3] = 0.0f;
	planes[4] = fade;
	const float add[5] = { 0.5f, 0.5f, 1.0f, FOG_ENTER + s, FOG_ENTER };

	const int blend = GLS_DEPTHMASK | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA;
	SW_DualChain( vLight->globalInteractions, world, planes, add, blend | GLS_DEPTHFUNC_EQUAL );
	SW_DualChain( vLight->localInteractions, world, planes, add, blend | GLS_DEPTHFUNC_EQUAL );

	// the light frustum bounding planes aren't in the depth buffer, so use depthfunc_less instead
	// of depthfunc_equal; the frustum is drawn side out: its back faces
	const idDrawVert *ac = (const idDrawVert *)vertexCache.Position( frustumTris->ambientCache );
	SwDraw d;
	memset( &d, 0, sizeof( d ) );
	d.clip = SW_ClipPositions( ac, sizeof( idDrawVert ), frustumTris->numVerts, &sw.viewDef->worldSpace );
	if ( !d.clip || !frustumTris->indexes ) {
		return;
	}
	SwDualParms *parms = (SwDualParms *)sw_view_alloc( sizeof( SwDualParms ) );
	if ( !parms ) {
		return;
	}
	*parms = world;
	float *dst[5] = { parms->s0, parms->t0, parms->q0, parms->s1, parms->t1 };
	for ( int k = 0; k < 5; k++ ) {
		// the world space is the identity: global planes are local planes
		dst[k][0] = planes[k][0]; dst[k][1] = planes[k][1]; dst[k][2] = planes[k][2]; dst[k][3] = planes[k][3] + add[k];
	}
	d.clipStride = 16;
	d.indexes = frustumTris->indexes;
	d.numIndexes = frustumTris->numIndexes;
	d.op = SW_OP_COLOR;
	d.kernel = SW_KERN_DUAL;
	d.kernelParms = parms;
	d.cull = SW_CULL_BACK_SIDED;
	d.mirror = sw.viewDef->isMirror;
	d.scissor = sw.scissor;
	d.depthRangeMax = 1.0f;
	d.verts.base = ac;
	d.verts.stride = sizeof( idDrawVert );
	d.verts.ofsXyz = (int)offsetof( idDrawVert, xyz );
	d.verts.ofsSt = (int)offsetof( idDrawVert, st );
	d.verts.ofsNormal = (int)offsetof( idDrawVert, normal );
	d.verts.ofsTangent0 = (int)offsetof( idDrawVert, tangents );
	d.verts.ofsTangent1 = (int)offsetof( idDrawVert, tangents ) + (int)sizeof( idVec3 );
	d.verts.ofsColor = (int)offsetof( idDrawVert, color );
	SW_SetStateBits( d, NULL, blend | GLS_DEPTHFUNC_LESS );
	sw_draw( &d );
}

static void SW_FogAllLights( void ) {
	if ( r_skipFogLights.GetBool() || sw.viewDef->isXraySubview ) {
		return;
	}
	bool began = false;
	for ( const viewLight_t *vLight = sw.viewDef->viewLights; vLight; vLight = vLight->next ) {
		if ( !vLight->lightShader->IsFogLight() && !vLight->lightShader->IsBlendLight() ) {
			continue;
		}
		if ( !began ) {
			sw_begin_segment( SW_SEG_COLOR, NULL );
			began = true;
		}
		if ( vLight->lightShader->IsFogLight() ) {
			SW_FogPass( vLight );
		} else {
			SW_BlendLight( vLight );
		}
	}
}

/*
================
SW_DrawView
================
*/
void SW_DrawView( const viewDef_t *viewDef ) {
	sw.viewDef = viewDef;
	sw.is3D = viewDef->viewEntitys != NULL;
	sw.fbWidth = glConfig.vidWidth;
	sw.fbHeight = glConfig.vidHeight;
	sw.stamp++;

	idScreenRect whole;
	whole.x1 = 0;
	whole.y1 = 0;
	whole.x2 = viewDef->viewport.x2 - viewDef->viewport.x1;
	whole.y2 = viewDef->viewport.y2 - viewDef->viewport.y1;
	sw.viewport = SW_RectFromGL( whole );
	sw.scissor = SW_ClampRect( SW_RectFromGL( viewDef->scissor ) );

	drawSurf_t **drawSurfs = (drawSurf_t **)&viewDef->drawSurfs[0];
	const int numDrawSurfs = viewDef->numDrawSurfs;

	sw_begin_view( &sw.viewport );

	if ( sw.is3D ) {
		// decide how much overbrighting we are going to do (with ARB2's maxLight of 999: none,
		// so RB_STD_LightScale never draws and has no counterpart here)
		RB_DetermineLightScale();

		SW_FillDepthBuffer( drawSurfs, numDrawSurfs );
		SW_DrawInteractions();
	}
	// a 2D view never captures: its post-process materials read whatever the last 3D view captured, as with GL
	sw.captured = !sw.is3D;
	const int processed = SW_DrawShaderPasses( drawSurfs, numDrawSurfs );

	if ( sw.is3D ) {
		SW_FogAllLights();

		// now draw any post-processing effects using _currentRender: the view's capture point (I5)
		if ( processed < numDrawSurfs && !r_skipPostProcess.GetBool() ) {
			sw_capture_point( &sw.viewport );
			sw.captured = true;
			SW_DrawShaderPasses( drawSurfs + processed, numDrawSurfs - processed );
		}
	}

	sw_end_view();
}
