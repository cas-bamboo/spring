/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "FeatureDrawer.h"

#include "Game/Camera.h"
#include "Game/GlobalUnsynced.h"
#include "Map/Ground.h"
#include "Map/MapInfo.h"
#include "Map/ReadMap.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/Env/IGroundDecalDrawer.h"
#include "Rendering/FarTextureHandler.h"
#include "Rendering/Env/ISky.h"
#include "Rendering/Env/ITreeDrawer.h"
#include "Rendering/Env/IWater.h"
#include "Rendering/GL/glExtra.h"
#include "Rendering/GL/myGL.h"
#include "Rendering/GL/VertexArray.h"
#include "Rendering/Map/InfoTexture/IInfoTextureHandler.h"
#include "Rendering/LuaObjectDrawer.h"
#include "Rendering/ShadowHandler.h"
#include "Rendering/Shaders/Shader.h"
#include "Rendering/Textures/S3OTextureHandler.h"
#include "Rendering/Textures/3DOTextureHandler.h"
#include "Rendering/UnitDrawer.h"
#include "Rendering/UnitDrawerState.hpp"
#include "Sim/Features/Feature.h"
#include "Sim/Features/FeatureDef.h"
#include "Sim/Misc/GlobalSynced.h"
#include "System/Config/ConfigHandler.h"
#include "System/EventHandler.h"
#include "System/myMath.h"
#include "System/Util.h"

#define DRAW_QUAD_SIZE 32

enum {
	FD_NODRAW_FLAG = 0,
	FD_OPAQUE_FLAG = 1,
	FD_ALPHAF_FLAG = 2,
	FD_SHADOW_FLAG = 3,
	FD_FARTEX_FLAG = 4,
};

static bool SetDrawAlphaValue(
	CFeature* f,
	CCamera* cam,
	float sqFadeDistMin = -1.0f,
	float sqFadeDistMax = -1.0f
) {
	// always reset
	f->drawAlpha = 0.0f;

	if (cam == nullptr)
		return false;

	// special case for non-fading features
	if (!f->alphaFade) {
		f->drawAlpha = 1.0f;
		return true;
	}

	const float sqDist = (f->pos - cam->GetPos()).SqLength();
	const float farLength = f->sqRadius * unitDrawer->unitDrawDistSqr;

	// if true, feature will become a fartex
	if (sqDist >= farLength)
		return false;

	float sqFadeDistB = sqFadeDistMin;
	float sqFadeDistE = sqFadeDistMax;

	if (farLength < sqFadeDistMax) {
		sqFadeDistE = farLength;
		sqFadeDistB = farLength * (sqFadeDistMin / sqFadeDistMax);
	}

	if (sqDist < sqFadeDistB) {
		// draw feature as normal, no fading
		f->drawAlpha = 1.0f;
		return true;
	}

	if (sqDist < sqFadeDistE) {
		// otherwise save it for the fade-pass
		f->drawAlpha = 1.0f - ((sqDist - sqFadeDistB) / (sqFadeDistE - sqFadeDistB));
		return true;
	}

	return false;
}



CONFIG(bool, ShowRezBars).defaultValue(true).headlessValue(false);

CONFIG(float, FeatureDrawDistance)
.defaultValue(6000.0f)
.minimumValue(0.0f)
.description("Maximum distance at which features will be drawn.");

CONFIG(float, FeatureFadeDistance)
.defaultValue(4500.0f)
.minimumValue(0.0f)
.description("Distance at which features will begin to fade from view.");

CFeatureDrawer* featureDrawer = NULL;

/******************************************************************************/

CR_BIND(CFeatureDrawer, )

CR_REG_METADATA(CFeatureDrawer, (
	CR_IGNORED(unsortedFeatures),
	CR_IGNORED(drawQuadsX),
	CR_IGNORED(drawQuadsY),
	CR_IGNORED(farDist),
	CR_IGNORED(featureDrawDistance),
	CR_IGNORED(featureFadeDistance),
	CR_IGNORED(modelRenderers),

	CR_POSTLOAD(PostLoad)
))

/******************************************************************************/



CFeatureDrawer::CFeatureDrawer(): CEventClient("[CFeatureDrawer]", 313373, false)
{
	eventHandler.AddClient(this);

	LuaObjectDrawer::ReadLODScales(LUAOBJ_FEATURE);

	// shared with UnitDrawer!
	geomBuffer = LuaObjectDrawer::GetGeometryBuffer();

	drawForward = true;
	drawDeferred = (geomBuffer->Valid());

	inAlphaPass = false;
	inShadowPass = false;

	drawQuadsX = mapDims.mapx / DRAW_QUAD_SIZE;
	drawQuadsY = mapDims.mapy / DRAW_QUAD_SIZE;
	featureDrawDistance = configHandler->GetFloat("FeatureDrawDistance");
	featureFadeDistance = std::min(configHandler->GetFloat("FeatureFadeDistance"), featureDrawDistance);

	modelRenderers.resize(drawQuadsX * drawQuadsY);
	camVisibleQuadFlags.resize(CCamera::CAMTYPE_ENVMAP, 0);
}


CFeatureDrawer::~CFeatureDrawer()
{
	eventHandler.RemoveClient(this);

	for (CFeature* f: unsortedFeatures) {
		groundDecals->ForceRemoveSolidObject(f);
	}

	modelRenderers.clear();
	camVisibleQuadFlags.clear();
}



void CFeatureDrawer::RenderFeatureCreated(const CFeature* feature)
{
	if (feature->def->drawType != DRAWTYPE_MODEL)
		return;

	CFeature* f = const_cast<CFeature*>(feature);

	// otherwise UpdateDrawQuad returns early
	f->drawQuad = -1;

	SetDrawAlphaValue(f, nullptr);
	UpdateDrawQuad(f);

	unsortedFeatures.push_back(f);
}



void CFeatureDrawer::RenderFeatureDestroyed(const CFeature* feature)
{
	CFeature* f = const_cast<CFeature*>(feature);

	if (f->def->drawType == DRAWTYPE_MODEL) {
		VectorErase(unsortedFeatures, f);
	}
	if (f->model && f->drawQuad >= 0) {
		modelRenderers[f->drawQuad].GetRenderer(MDL_TYPE(f))->DelFeature(f);
		f->drawQuad = -1;
	}

	LuaObjectDrawer::SetObjectLOD(f, LUAOBJ_FEATURE, 0);
}


void CFeatureDrawer::FeatureMoved(const CFeature* feature, const float3& oldpos)
{
	UpdateDrawQuad(const_cast<CFeature*>(feature));
}

void CFeatureDrawer::UpdateDrawQuad(CFeature* feature)
{
	const int oldDrawQuad = feature->drawQuad;

	if (oldDrawQuad < -1)
		return;

	const int newDrawQuadX = Clamp(int(feature->pos.x / SQUARE_SIZE / DRAW_QUAD_SIZE), 0, drawQuadsX - 1);
	const int newDrawQuadY = Clamp(int(feature->pos.z / SQUARE_SIZE / DRAW_QUAD_SIZE), 0, drawQuadsY - 1);
	const int newDrawQuad = newDrawQuadY * drawQuadsX + newDrawQuadX;

	if (oldDrawQuad == newDrawQuad)
		return;

	//TODO check if out of map features get drawn, when the camera is outside of the map
	//     (q: does DrawGround render the border quads in such cases?)
	assert(oldDrawQuad < drawQuadsX * drawQuadsY);
	assert(newDrawQuad < drawQuadsX * drawQuadsY && newDrawQuad >= 0);

	if (feature->model) {
		if (oldDrawQuad >= 0) {
			modelRenderers[oldDrawQuad].GetRenderer(MDL_TYPE(feature))->DelFeature(feature);
		}

		modelRenderers[newDrawQuad].GetRenderer(MDL_TYPE(feature))->AddFeature(feature);
	}

	feature->drawQuad = newDrawQuad;
}


void CFeatureDrawer::Update()
{
	for (CFeature* f: unsortedFeatures) {
		UpdateDrawPos(f);
		SetDrawAlphaValue(f, nullptr);
	}
}


inline void CFeatureDrawer::UpdateDrawPos(CFeature* f)
{
	const float time = globalRendering->timeOffset;

	f->drawPos    =    f->pos + (f->speed * time);
	f->drawMidPos = f->midPos + (f->speed * time);
}


void CFeatureDrawer::Draw()
{
	ISky::SetupFog();

	if (infoTextureHandler->IsEnabled()) {
		glActiveTextureARB(GL_TEXTURE2_ARB);
		glEnable(GL_TEXTURE_2D);
		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_ARB, GL_ADD_SIGNED_ARB);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_ARB);

		glMultiTexCoord4f(GL_TEXTURE2_ARB, 1.0f,1.0f,1.0f,1.0f); // workaround a nvidia bug with TexGen
		SetTexGen(1.0f / (mapDims.pwr2mapx * SQUARE_SIZE), 1.0f / (mapDims.pwr2mapy * SQUARE_SIZE), 0.0f, 0.0f);

		glBindTexture(GL_TEXTURE_2D, infoTextureHandler->GetCurrentInfoTexture());
		glActiveTextureARB(GL_TEXTURE0_ARB);
	}

	// mark all features (in the quads we can see) with a FD_*_FLAG value
	// the passes below will ignore any features whose marker is not valid
	GetVisibleFeatures(CCamera::GetCamera(CCamera::CAMTYPE_ACTIVE), 0, true);

	// first do the deferred pass; conditional because
	// most of the water renderers use their own FBO's
	if (drawDeferred && !water->DrawReflectionPass() && !water->DrawRefractionPass()) {
		LuaObjectDrawer::DrawDeferredPass(LUAOBJ_FEATURE);
	}

	// now do the regular forward pass
	if (drawForward) {
		DrawOpaquePass(false, water->DrawReflectionPass(), water->DrawRefractionPass());
	}

	farTextureHandler->Draw();

	if (infoTextureHandler->IsEnabled()) {
		glActiveTextureARB(GL_TEXTURE2_ARB);
		glDisable(GL_TEXTURE_2D);
		glDisable(GL_TEXTURE_GEN_S);
		glDisable(GL_TEXTURE_GEN_T);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		glActiveTextureARB(GL_TEXTURE0_ARB);
	}

	glDisable(GL_TEXTURE_2D);
	glDisable(GL_FOG);
}

void CFeatureDrawer::DrawOpaquePass(bool deferredPass, bool, bool)
{
	unitDrawer->SetupOpaqueDrawing(deferredPass);

	for (int modelType = MODELTYPE_3DO; modelType < MODELTYPE_OTHER; modelType++) {
		unitDrawer->PushModelRenderState(modelType);
		DrawOpaqueFeatures(modelType);
		unitDrawer->PopModelRenderState(modelType);
	}

	unitDrawer->ResetOpaqueDrawing(deferredPass);

	// draw all custom'ed features that were bypassed in the loop above
	LuaObjectDrawer::SetDrawPassGlobalLODFactor(LUAOBJ_FEATURE);
	LuaObjectDrawer::DrawOpaqueMaterialObjects(LUAOBJ_FEATURE, deferredPass);
}

void CFeatureDrawer::DrawOpaqueFeatures(int modelType)
{
	for (const auto& mdlRenderProxy: modelRenderers) {
		if (mdlRenderProxy.GetLastDrawFrame() < globalRendering->drawFrame)
			continue;

		const auto mdlRenderer = mdlRenderProxy.GetRenderer(modelType);
		const auto& featureBin = mdlRenderer->GetFeatureBin();

		for (const auto& binElem: featureBin) {
			CUnitDrawer::BindModelTypeTexture(modelType, binElem.first);

			for (CFeature* f: binElem.second) {
				// fartex, opaque, shadow are allowed here
				switch (f->drawFlag) {
					case FD_NODRAW_FLAG: {                              continue; } break;
					case FD_ALPHAF_FLAG: {                              continue; } break;
					case FD_FARTEX_FLAG: { farTextureHandler->Queue(f); continue; } break;
					default: {} break;
				}

				// test this before the LOD calls (for consistency with UD)
				if (!CanDrawFeature(f))
					continue;

				if ( inShadowPass && LuaObjectDrawer::AddShadowMaterialObject(f, LUAOBJ_FEATURE))
					continue;
				if (!inShadowPass && LuaObjectDrawer::AddOpaqueMaterialObject(f, LUAOBJ_FEATURE))
					continue;

				if (!inShadowPass)
					unitDrawer->SetTeamColour(f->team);

				DrawFeature(f, 0, 0, false, false);
			}
		}
	}
}

bool CFeatureDrawer::CanDrawFeature(const CFeature* feature) const
{
	if (feature->noDraw)
		return false;
	if (feature->IsInVoid())
		return false;
	if (!feature->IsInLosForAllyTeam(gu->myAllyTeam) && !gu->spectatingFullView)
		return false;

	// either PLAYER or SHADOW or UWREFL
	const CCamera* cam = CCamera::GetActiveCamera();

	if (feature->alphaFade && cam->GetCamType() != CCamera::CAMTYPE_SHADOW) {
		const float sqDist = (feature->pos - cam->GetPos()).SqLength();
		const float farLength = feature->sqRadius * unitDrawer->unitDrawDistSqr;
		const float sqFadeDistEnd = featureDrawDistance * featureDrawDistance;

		if (sqDist >= std::min(farLength, sqFadeDistEnd))
			return false;
	}

	return (cam->InView(feature->drawMidPos, feature->drawRadius));
}



inline void CFeatureDrawer::DrawFeatureModel(const CFeature* feature, bool noLuaCall) {
	if (!noLuaCall && feature->luaDraw && eventHandler.DrawFeature(feature))
		return;

	feature->localModel.Draw();
}

void CFeatureDrawer::DrawFeatureNoTrans(
	const CFeature* feature,
	unsigned int preList,
	unsigned int postList,
	bool /*lodCall*/,
	bool noLuaCall
) {
	if (preList != 0) {
		glCallList(preList);
	}

	DrawFeatureModel(feature, noLuaCall);

	if (postList != 0) {
		glCallList(postList);
	}
}

void CFeatureDrawer::DrawFeature(const CFeature* feature, unsigned int preList, unsigned int postList, bool lodCall, bool noLuaCall)
{
	glPushMatrix();
	glMultMatrixf(feature->GetTransformMatrixRef());

	DrawFeatureNoTrans(feature, preList, postList, lodCall, noLuaCall);

	glPopMatrix();
}




void CFeatureDrawer::PushIndividualState(const CFeature* feature, bool deferredPass)
{
	unitDrawer->SetupOpaqueDrawing(false);
	unitDrawer->PushModelRenderState(feature);
	unitDrawer->SetTeamColour(feature->team);
}

void CFeatureDrawer::PopIndividualState(const CFeature* feature, bool deferredPass)
{
	unitDrawer->PopModelRenderState(feature);
	unitDrawer->ResetOpaqueDrawing(false);
}


void CFeatureDrawer::DrawIndividual(const CFeature* feature, bool noLuaCall)
{
	const bool origDrawDebug = globalRendering->GetSetDrawDebug(false);

	if (!LuaObjectDrawer::DrawSingleObject(feature, LUAOBJ_FEATURE /*, noLuaCall*/)) {
		// set the full default state
		PushIndividualState(feature, false);
		DrawFeature(feature, 0, 0, false, noLuaCall);
		PopIndividualState(feature, false);
	}

	globalRendering->GetSetDrawDebug(origDrawDebug);
}

void CFeatureDrawer::DrawIndividualNoTrans(const CFeature* feature, bool noLuaCall)
{
	const bool origDrawDebug = globalRendering->GetSetDrawDebug(false);

	if (!LuaObjectDrawer::DrawSingleObjectNoTrans(feature, LUAOBJ_FEATURE /*, noLuaCall*/)) {
		PushIndividualState(feature, false);
		DrawFeatureNoTrans(feature, 0, 0, false, noLuaCall);
		PopIndividualState(feature, false);
	}

	globalRendering->GetSetDrawDebug(origDrawDebug);
}




static void SetFeatureAlphaDrawState(const CFeature* f, bool ffp)
{
	if (ffp) {
		const float cols[] = {1.0f, 1.0f, 1.0f, f->drawAlpha};

		glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, cols);
		glColor4fv(cols);
	}

	// hack, sorting objects by distance would look better
	glAlphaFunc(GL_GREATER, f->drawAlpha * 0.5f);
}


void CFeatureDrawer::DrawAlphaPass()
{
	inAlphaPass = true;

	{
		unitDrawer->SetupAlphaDrawing(false);

		glPushAttrib(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glDepthMask(GL_TRUE);
		glEnable(GL_ALPHA_TEST);
		glAlphaFunc(GL_GREATER, 0.5f);

		ISky::SetupFog();

		for (int modelType = MODELTYPE_3DO; modelType < MODELTYPE_OTHER; modelType++) {
			unitDrawer->PushModelRenderState(modelType);
			DrawAlphaFeatures(modelType);
			unitDrawer->PopModelRenderState(modelType);
		}

		glDisable(GL_FOG);
		glPopAttrib();

		unitDrawer->ResetAlphaDrawing(false);
	}

	LuaObjectDrawer::SetDrawPassGlobalLODFactor(LUAOBJ_FEATURE);
	LuaObjectDrawer::DrawAlphaMaterialObjects(LUAOBJ_FEATURE, false);

	inAlphaPass = false;
}

void CFeatureDrawer::DrawAlphaFeatures(int modelType)
{
	const bool ffpMat = !(unitDrawer->GetWantedDrawerState(true))->CanDrawAlpha();

	for (const auto& mdlRenderProxy: modelRenderers) {
		if (mdlRenderProxy.GetLastDrawFrame() < globalRendering->drawFrame)
			continue;

		const auto mdlRenderer = mdlRenderProxy.GetRenderer(modelType);
		const auto& featureBin = mdlRenderer->GetFeatureBin();

		for (const auto& binElem: featureBin) {
			CUnitDrawer::BindModelTypeTexture(modelType, binElem.first);

			for (CFeature* f: binElem.second) {
				// only alpha is allowed here
				switch (f->drawFlag) {
					case FD_NODRAW_FLAG: { continue; } break;
					case FD_OPAQUE_FLAG: { continue; } break;
					case FD_SHADOW_FLAG: { continue; } break;
					case FD_FARTEX_FLAG: { continue; } break;
					default: {} break;
				}

				if (!CanDrawFeature(f))
					continue;

				if (LuaObjectDrawer::AddAlphaMaterialObject(f, LUAOBJ_FEATURE))
					continue;

				unitDrawer->SetTeamColour(f->team, float2(f->drawAlpha, 1.0f));

				SetFeatureAlphaDrawState(f, ffpMat);
				DrawFeature(f, 0, 0, false, false);
			}
		}
	}
}




void CFeatureDrawer::DrawShadowPass()
{
	inShadowPass = true;

	glPolygonOffset(1.0f, 1.0f);
	glEnable(GL_POLYGON_OFFSET_FILL);

	Shader::IProgramObject* po =
		shadowHandler->GetShadowGenProg(CShadowHandler::SHADOWGEN_PROGRAM_MODEL);

	po->Enable();

	{
		// mark all features (in the quads we can see) with FD_SHADOW_FLAG
		// the pass below will ignore any features whose tag != this value
		GetVisibleFeatures(CCamera::GetCamera(CCamera::CAMTYPE_SHADOW), 0, false);

		// need the alpha-mask for transparent features
		glEnable(GL_TEXTURE_2D);
		glPushAttrib(GL_COLOR_BUFFER_BIT);
		glEnable(GL_ALPHA_TEST);
		glAlphaFunc(GL_GREATER, 0.5f);

		// needed for 3do models (else they will use any currently bound texture)
		// note: texture0 is by default a 1x1 texture with rgba(0,0,0,255)
		// (we are just interested in the 255 alpha here)
		glBindTexture(GL_TEXTURE_2D, 0);

		// 3DO's have clockwise-wound faces and
		// (usually) holes, so disable backface
		// culling for them
		glDisable(GL_CULL_FACE);
		DrawOpaqueFeatures(MODELTYPE_3DO);
		glEnable(GL_CULL_FACE);

		for (int modelType = MODELTYPE_S3O; modelType < MODELTYPE_OTHER; modelType++) {
			DrawOpaqueFeatures(modelType);
		}

		glPopAttrib();
		glDisable(GL_TEXTURE_2D);
	}

	po->Disable();

	glDisable(GL_POLYGON_OFFSET_FILL);

	LuaObjectDrawer::SetDrawPassGlobalLODFactor(LUAOBJ_FEATURE);
	LuaObjectDrawer::DrawShadowMaterialObjects(LUAOBJ_FEATURE, false);

	inShadowPass = false;
}



class CFeatureQuadDrawer : public CReadMap::IQuadDrawer {
public:
	int drawQuadsX;

	bool drawReflection;
	bool drawRefraction;
	bool drawShadowPass;
	bool drawFarFeatures;

	float sqFadeDistBegin;
	float sqFadeDistEnd;

	CCamera* cam;

public:
	void ResetState() {
		drawQuadsX = 0;

		drawReflection = false;
		drawRefraction = false;
		drawShadowPass = false;
		drawFarFeatures = false;

		sqFadeDistBegin = 0.0f;
		sqFadeDistEnd   = 0.0f;

		cam = nullptr;
	}

	void DrawQuad(int x, int y) {
		auto& mdlRenderProxy = featureDrawer->modelRenderers[y * drawQuadsX + x];

		// used so we do not iterate over non-visited renderers (in any pass)
		mdlRenderProxy.SetLastDrawFrame(globalRendering->drawFrame);

		for (int i = 0; i < MODELTYPE_OTHER; ++i) {
			auto mdlRenderer = mdlRenderProxy.GetRenderer(i);
			auto& featureBin = mdlRenderer->GetFeatureBinMutable();

			for (auto& binElem: featureBin) {
				for (CFeature* f: binElem.second) {
					assert((y * drawQuadsX + x) == f->drawQuad);

					// clear marker; will be set at most once below
					f->drawFlag = FD_NODRAW_FLAG;

					if (f->noDraw)
						continue;
					if (f->IsInVoid())
						continue;

					assert(f->def->drawType == DRAWTYPE_MODEL);

					if (!gu->spectatingFullView && !f->IsInLosForAllyTeam(gu->myAllyTeam))
						continue;


					if (drawShadowPass) {
						f->drawFlag = FD_SHADOW_FLAG;
						continue;
					}

					if (drawRefraction && !f->IsInWater())
						continue;

					if (drawReflection && !CUnitDrawer::ObjectVisibleReflection(f->drawMidPos, cam->GetPos(), f->drawRadius))
						continue;


					if (SetDrawAlphaValue(f, cam, sqFadeDistBegin, sqFadeDistEnd)) {
						f->drawFlag += (FD_OPAQUE_FLAG * (f->drawAlpha == 1.0f));
						f->drawFlag += (FD_ALPHAF_FLAG * (f->drawAlpha <  1.0f));
						continue;
					}

					// note: it looks pretty bad to first alpha-fade and then
					// draw a fully *opaque* fartex, so restrict impostors to
					// non-fading features
					f->drawFlag = FD_FARTEX_FLAG * drawFarFeatures * (!f->alphaFade);
				}
			}
		}
	}
};



void CFeatureDrawer::GetVisibleFeatures(CCamera* cam, int extraSize, bool drawFar)
{
	// should only ever be called for the first three types
	assert(cam->GetCamType() < camVisibleQuadFlags.size());

	// check if we already did a pass for this camera-type
	// (e.g. water refraction and the standard opaque pass
	// use CAMTYPE_PLAYER with equal state)
	// ignored: the refraction pass needs to skip features
	// that are not in water, so we must either recalculate
	// drawFlag or add drawRefraction checks in Draw{*}Pass
	if (false && camVisibleQuadFlags[cam->GetCamType()] >= globalRendering->drawFrame)
		return;

	camVisibleQuadFlags[cam->GetCamType()] = globalRendering->drawFrame;

	CFeatureQuadDrawer drawer;

	drawer.drawQuadsX = drawQuadsX;
	drawer.drawReflection = water->DrawReflectionPass();
	drawer.drawRefraction = water->DrawRefractionPass();
	drawer.drawShadowPass = (cam->GetCamType() == CCamera::CAMTYPE_SHADOW);
	drawer.drawFarFeatures = drawFar;
	drawer.sqFadeDistBegin = featureFadeDistance * featureFadeDistance;
	drawer.sqFadeDistEnd = featureDrawDistance * featureDrawDistance;
	drawer.cam = cam;

	cam->GetFrustumSides(readMap->GetCurrMinHeight() - 100.0f, readMap->GetCurrMaxHeight() + 100.0f, SQUARE_SIZE);
	readMap->GridVisibility(cam, &drawer, featureDrawDistance, DRAW_QUAD_SIZE, extraSize);
}

void CFeatureDrawer::PostLoad()
{
	drawQuadsX = mapDims.mapx/DRAW_QUAD_SIZE;
	drawQuadsY = mapDims.mapy/DRAW_QUAD_SIZE;
}

