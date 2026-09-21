p = 'C:/Source/DOOM-3/neo/renderer/sw/sw_submit.cpp'
s = open(p, encoding='utf-8').read()

def rep(old, new):
    global s
    assert s.count(old) == 1, old[:70]
    s = s.replace(old, new)

rep("""// tr_render.cpp; not in tr_local.h
""", """// draw_arb2.cpp (ID_SW_RENDERER): the file name of a program, by which a new-style stage finds its kernel
const char *R_ARBProgramName( int ident );
// tr_render.cpp; not in tr_local.h
""")

rep("""	int					stamp;
	swClipEntry_t		clipHash[SW_CLIP_HASH];
} sw;""", """	int					stamp;
	bool				captured;			// the view's capture point has been passed: SW_KERN_SCREEN draws may go
	swClipEntry_t		clipHash[SW_CLIP_HASH];
} sw;""")

# ---- texgens that need another kernel: reflect cube, screen
rep("""static bool SW_StageTexturing( const shaderStage_t *pStage, const drawSurf_t *surf, SwDraw &d, SwStageParms *parms ) {""",
"""static void SW_PlaneFromMatrixRow( const float *m, int row, float out[4] ) {
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

static bool SW_StageTexturing( const shaderStage_t *pStage, const drawSurf_t *surf, SwDraw &d, SwStageParms *parms ) {""")

# ---- the shader pass loop: new stages and the program texgens
rep("""		if ( pStage->newStage ) {
			continue;		// Stage 7
		}

		float color[4];""", """		if ( pStage->newStage ) {
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

		float color[4];""")

rep("""		SwDraw d = base;
		d.op = SW_OP_COLOR;
		d.kernel = SW_KERN_STAGE;
		d.kernelParms = parms;
		if ( !SW_StageTexturing( pStage, surf, d, parms ) ) {
			continue;
		}""", """		SwDraw d = base;
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
		}""")

# ---- depth fill: subview surfaces keep the colour; mirror views clip at the mirror plane
rep("""		SwDraw d;
		if ( !SW_BaseDraw( surf, d ) ) {
			continue;
		}
		d.op = SW_OP_DEPTH_FILL;
		d.color = 0xFF000000u;

		bool drawSolid = shader->Coverage() == MC_OPAQUE;""", """		SwDraw d;
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

		bool drawSolid = shader->Coverage() == MC_OPAQUE;""")

rep("""				parms->alphaTest = SW_ATEST_GT_REF;
				parms->alphaRef = regs[ pStage->alphaTestRegister ];
				sd.kernel = SW_KERN_STAGE;
				sd.kernelParms = parms;
				sw_draw( &sd );""", """				parms->alphaTest = SW_ATEST_GT_REF;
				parms->alphaRef = regs[ pStage->alphaTestRegister ];
				parms->hasClip = hasClip;
				memcpy( parms->clipPlane, clipPlane, sizeof( clipPlane ) );
				sd.kernel = SW_KERN_STAGE;
				sd.kernelParms = parms;
				sw_draw( &sd );""")

rep("""		if ( drawSolid ) {
			sw_draw( &d );
		}
	}
}""", """		if ( drawSolid ) {
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
}""")

# ---- fog and blend lights
rep("""/*
================
SW_DrawView
================
*/""", r"""/*
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
*/""")

rep("""		SW_FillDepthBuffer( drawSurfs, numDrawSurfs );
		SW_DrawInteractions();
	}
	SW_DrawShaderPasses( drawSurfs, numDrawSurfs );
""", """		SW_FillDepthBuffer( drawSurfs, numDrawSurfs );
		SW_DrawInteractions();
	}
	sw.captured = false;
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
""")

rep("""		if ( surf->material->GetSort() >= SS_POST_PROCESS ) {
			break;
		}
		SW_RenderShaderPasses( surf );""", """		// post-process surfaces wait for the capture of everything else, fog included
		if ( surf->material->GetSort() >= SS_POST_PROCESS && !sw.captured ) {
			break;
		}
		SW_RenderShaderPasses( surf );""")
open(p, 'w', encoding='utf-8', newline='').write(s)

# ---- the stage kernel's clip varying (core)
import sys
def cpatch(path, pairs):
    t = open(path, encoding='utf-8').read()
    for old, new in pairs:
        assert t.count(old) == 1, (path, old[:60])
        t = t.replace(old, new)
    open(path, 'w', encoding='utf-8', newline='').write(t)

C = 'C:/Source/DOOM-3/neo/renderer/sw/'
cpatch(C + 'sw_api.h', [
("""    int             alphaTest;          /* SW_ATEST_* */
    float           alphaRef;           /* SW_ATEST_GT_REF */
} SwStageParms;""", """    int             alphaTest;          /* SW_ATEST_* */
    float           alphaRef;           /* SW_ATEST_GT_REF */
    int             hasClip;            /* a mirror view's clip plane: lanes with dot(clipPlane, (x, y, z, 1)) <= 0 are dropped */
    float           clipPlane[4];       /* in the vertices' space */
} SwStageParms;"""),
])
cpatch(C + 'geom/vertex.c', [
("""enum { VS_U, VS_V, VS_Q, VS_R, VS_G, VS_B, VS_A, VS_COUNT };     /* q: the third coordinate of a cube lookup */""",
 """enum { VS_U, VS_V, VS_Q, VS_R, VS_G, VS_B, VS_A, VS_CLIP, VS_COUNT };     /* q: the third coordinate of a cube lookup; clip: distance to a mirror's plane */"""),
("""    const float a = (float)col[3] * (1.0f / 255.0f);
    var[VS_A] = (p->vertexColorModulate == 0.0f ? p->vertexColorAdd : a) * p->color[3];
}""", """    const float a = (float)col[3] * (1.0f / 255.0f);
    var[VS_A] = (p->vertexColorModulate == 0.0f ? p->vertexColorAdd : a) * p->color[3];
    if (p->hasClip) {
        const float *pos = (const float *)(v + d->verts.ofsXyz);
        var[VS_CLIP] = p->clipPlane[0] * pos[0] + p->clipPlane[1] * pos[1] + p->clipPlane[2] * pos[2] + p->clipPlane[3];
    } else var[VS_CLIP] = 0.0f;
}"""),
])
cpatch(C + 'kern/k_stage.c', [
("""static __forceinline __mmask16 k_stage(const SwTriAttr *a, const SwStageParms *p, __mmask16 m, VF X, VF Y, VI *out)
{
""", """static __forceinline __mmask16 k_stage(const SwTriAttr *a, const SwStageParms *p, __mmask16 m, VF X, VF Y, VI *out)
{
    if (p->hasClip) {                                               /* the sign of (distance / w) is the distance's: w > 0 */
        m = _mm512_mask_cmp_ps_mask(m, PLANE(a, VS_CLIP, X, Y), _mm512_setzero_ps(), _CMP_GT_OQ);
        if (!m) return 0;
    }
"""),
])
print('ok')
