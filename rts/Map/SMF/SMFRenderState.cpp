/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "SMFRenderState.h"
#include "SMFGroundDrawer.h"
#include "SMFReadMap.h"
#include "Game/Camera.h"
#include "Map/MapInfo.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/ShadowHandler.h"
#include "Rendering/Env/CubeMapHandler.h"
#include "Rendering/Env/ISky.h"
#include "Rendering/Env/SkyLight.h"
#include "Rendering/Env/SunLighting.h"
#include "Rendering/Env/WaterRendering.h"
#include "Rendering/Env/MapRendering.h"
#include "Rendering/GL/myGL.h"
#include "Rendering/Map/InfoTexture/IInfoTextureHandler.h"
#include "Rendering/Shaders/ShaderHandler.h"
#include "Rendering/Shaders/Shader.h"
#include "Sim/Misc/GlobalSynced.h"
#include "System/Config/ConfigHandler.h"
#include "System/StringUtil.h"

static constexpr float SMF_TEXSQUARE_SIZE = 1024.0f;


ISMFRenderState* ISMFRenderState::GetInstance(bool haveGLSL, bool luaShaders, bool noop) {
	if (noop)
		return new SMFRenderStateNOOP();

	ISMFRenderState* instance = nullptr;
	if (!haveGLSL)
		instance = new SMFRenderStateFFP();
	else
		instance = new SMFRenderStateGLSL(luaShaders);

	return instance;
}

bool SMFRenderStateGLSL::Init(const CSMFGroundDrawer* smfGroundDrawer) {
	if (!globalRendering->haveGLSL) {
		// not possible to do (GLSL) shader-based map rendering
		return false;
	}
	if (!configHandler->GetBool("AdvMapShading")) {
		// not allowed to do (GLSL) shader-based map rendering
		return false;
	}


	const std::string names[GLSL_SHADER_COUNT - 1] = {
		"SMFShaderGLSL-Standard",
		"SMFShaderGLSL-Deferred",
	};
	const std::string defs =
		("#define SMF_TEXSQUARE_SIZE " + FloatToString(                  SMF_TEXSQUARE_SIZE) + "\n") +
		("#define SMF_INTENSITY_MULT " + FloatToString(CGlobalRendering::SMF_INTENSITY_MULT) + "\n");


	if (useLuaShaders) {
		for (unsigned int n = GLSL_SHADER_STANDARD; n <= GLSL_SHADER_DEFERRED; n++) {
			glslShaders[n] = shaderHandler->CreateProgramObject("[SMFGroundDrawer::Lua]", names[n] + "-Lua");
			// release ID created by GLSLProgramObject's ctor; should be 0
			glslShaders[n]->Release();
		}
	} else {
		for (unsigned int n = GLSL_SHADER_STANDARD; n <= GLSL_SHADER_DEFERRED; n++) {
			// load from VFS files
			glslShaders[n] = shaderHandler->CreateProgramObject("[SMFGroundDrawer::VFS]", names[n]);
			glslShaders[n]->AttachShaderObject(shaderHandler->CreateShaderObject("GLSL/SMFVertProg.glsl", defs, GL_VERTEX_SHADER));
			glslShaders[n]->AttachShaderObject(shaderHandler->CreateShaderObject("GLSL/SMFFragProg.glsl", defs, GL_FRAGMENT_SHADER));
			glslShaders[n]->BindAttribLocation("vertexPos", 0);
		}
	}

	glslShaders[GLSL_SHADER_CURRENT] = glslShaders[GLSL_SHADER_STANDARD];
	return true;
}

void SMFRenderStateGLSL::Kill() {
	if (useLuaShaders) {
		// make sure SH deletes only the wrapper objects; programs are managed by LuaShaders
		for (unsigned int n = GLSL_SHADER_STANDARD; n <= GLSL_SHADER_DEFERRED; n++) {
			if (glslShaders[n] != nullptr) {
				glslShaders[n]->LoadFromID(0);
			}
		}

		shaderHandler->ReleaseProgramObjects("[SMFGroundDrawer::Lua]");
	} else {
		shaderHandler->ReleaseProgramObjects("[SMFGroundDrawer::VFS]");
	}
}

void SMFRenderStateGLSL::Update(
	const CSMFGroundDrawer* smfGroundDrawer,
	const LuaMapShaderData* luaMapShaderData
) {
	if (!globalRendering->haveGLSL || !configHandler->GetBool("AdvMapShading")) {
		return; //nothing to do here
	}

	if (useLuaShaders) {
		assert(luaMapShaderData != nullptr);

		// load from LuaShader ID; should be a linked and valid program (or 0)
		// NOTE: only non-custom shaders get to have engine flags and uniforms!
		for (unsigned int n = GLSL_SHADER_STANDARD; n <= GLSL_SHADER_DEFERRED; n++) {
			glslShaders[n]->LoadFromID(luaMapShaderData->shaderIDs[n]);
		}
	} else {
		assert(luaMapShaderData == nullptr);

		const CSMFReadMap* smfMap = smfGroundDrawer->GetReadMap();
		const GL::LightHandler* lightHandler = smfGroundDrawer->GetLightHandler();

		const int2 normTexSize = smfMap->GetTextureSize(MAP_BASE_NORMALS_TEX);
		// const int2 specTexSize = smfMap->GetTextureSize(MAP_SSMF_SPECULAR_TEX);

		for (unsigned int n = GLSL_SHADER_STANDARD; n <= GLSL_SHADER_DEFERRED; n++) {
			glslShaders[n]->SetFlag("SMF_VOID_WATER",                       mapRendering->voidWater);
			glslShaders[n]->SetFlag("SMF_VOID_GROUND",                      mapRendering->voidGround);
			glslShaders[n]->SetFlag("SMF_SPECULAR_LIGHTING",               (smfMap->GetSpecularTexture() != 0));
			glslShaders[n]->SetFlag("SMF_DETAIL_TEXTURE_SPLATTING",        (smfMap->GetSplatDistrTexture() != 0 && smfMap->GetSplatDetailTexture() != 0));
			glslShaders[n]->SetFlag("SMF_DETAIL_NORMAL_TEXTURE_SPLATTING", (smfMap->GetSplatDistrTexture() != 0 && smfMap->HaveSplatNormalTexture()));
			glslShaders[n]->SetFlag("SMF_DETAIL_NORMAL_DIFFUSE_ALPHA",      mapRendering->splatDetailNormalDiffuseAlpha);
			glslShaders[n]->SetFlag("SMF_WATER_ABSORPTION",                 smfMap->HasVisibleWater());
			glslShaders[n]->SetFlag("SMF_SKY_REFLECTIONS",                 (smfMap->GetSkyReflectModTexture() != 0));
			glslShaders[n]->SetFlag("SMF_BLEND_NORMALS",                   (smfMap->GetBlendNormalsTexture() != 0));
			glslShaders[n]->SetFlag("SMF_LIGHT_EMISSION",                  (smfMap->GetLightEmissionTexture() != 0));
			glslShaders[n]->SetFlag("SMF_PARALLAX_MAPPING",                (smfMap->GetParallaxHeightTexture() != 0));

			glslShaders[n]->SetFlag("BASE_DYNAMIC_MAP_LIGHT", lightHandler->GetBaseLight());
			glslShaders[n]->SetFlag("MAX_DYNAMIC_MAP_LIGHTS", lightHandler->GetMaxLights());

			// both are runtime set in ::Enable, but ATI drivers need values from the beginning
			glslShaders[n]->SetFlag("HAVE_SHADOWS", false);
			glslShaders[n]->SetFlag("HAVE_INFOTEX", false);

			// used to strip down the shader for the deferred pass
			glslShaders[n]->SetFlag("DEFERRED_MODE", (n != GLSL_SHADER_STANDARD));
			glslShaders[n]->SetFlag("GBUFFER_NORMTEX_IDX", GL::GeometryBuffer::ATTACHMENT_NORMTEX);
			glslShaders[n]->SetFlag("GBUFFER_DIFFTEX_IDX", GL::GeometryBuffer::ATTACHMENT_DIFFTEX);
			glslShaders[n]->SetFlag("GBUFFER_SPECTEX_IDX", GL::GeometryBuffer::ATTACHMENT_SPECTEX);
			glslShaders[n]->SetFlag("GBUFFER_EMITTEX_IDX", GL::GeometryBuffer::ATTACHMENT_EMITTEX);
			glslShaders[n]->SetFlag("GBUFFER_MISCTEX_IDX", GL::GeometryBuffer::ATTACHMENT_MISCTEX);
			glslShaders[n]->SetFlag("GBUFFER_ZVALTEX_IDX", GL::GeometryBuffer::ATTACHMENT_ZVALTEX);

			glslShaders[n]->Link();
			glslShaders[n]->Enable();

			// tex1 (shadingTex) is not used by SMFFragProg
			glslShaders[n]->SetUniform("diffuseTex",             0);
			glslShaders[n]->SetUniform("detailTex",              2);
			glslShaders[n]->SetUniform("shadowTex",              4);
			glslShaders[n]->SetUniform("normalsTex",             5);
			glslShaders[n]->SetUniform("specularTex",            6);
			glslShaders[n]->SetUniform("splatDetailTex",         7);
			glslShaders[n]->SetUniform("splatDistrTex",          8);
			glslShaders[n]->SetUniform("skyReflectTex",          9);
			glslShaders[n]->SetUniform("skyReflectModTex",      10);
			glslShaders[n]->SetUniform("blendNormalsTex",       11);
			glslShaders[n]->SetUniform("lightEmissionTex",      12);
			glslShaders[n]->SetUniform("parallaxHeightTex",     13);
			glslShaders[n]->SetUniform("infoTex",               14);
			glslShaders[n]->SetUniform("splatDetailNormalTex1", 15);
			glslShaders[n]->SetUniform("splatDetailNormalTex2", 16);
			glslShaders[n]->SetUniform("splatDetailNormalTex3", 17);
			glslShaders[n]->SetUniform("splatDetailNormalTex4", 18);
			glslShaders[n]->SetUniform("shadowColorTex",        19);

			glslShaders[n]->SetUniform("mapSizePO2", mapDims.pwr2mapx * SQUARE_SIZE * 1.0f, mapDims.pwr2mapy * SQUARE_SIZE * 1.0f);
			glslShaders[n]->SetUniform("mapSize",    mapDims.mapx     * SQUARE_SIZE * 1.0f, mapDims.mapy     * SQUARE_SIZE * 1.0f);

			glslShaders[n]->SetUniform4v("lightDir",  &ISky::GetSky()->GetLight()->GetLightDir()[0]);
			glslShaders[n]->SetUniform3v("cameraPos", &camera->GetPos()[0]);

			glslShaders[n]->SetUniform3v("groundAmbientColor",  &sunLighting->groundAmbientColor[0]);
			glslShaders[n]->SetUniform3v("groundDiffuseColor",  &sunLighting->groundDiffuseColor[0]);
			glslShaders[n]->SetUniform3v("groundSpecularColor", &sunLighting->groundSpecularColor[0]);
			glslShaders[n]->SetUniform  ("groundSpecularExponent", sunLighting->specularExponent);
			glslShaders[n]->SetUniform  ("groundShadowDensity", sunLighting->groundShadowDensity);

			glslShaders[n]->SetUniformMatrix4x4("shadowMat", false, shadowHandler.GetShadowMatrixRaw());

			glslShaders[n]->SetUniform3v("waterMinColor",    &waterRendering->minColor[0]);
			glslShaders[n]->SetUniform3v("waterBaseColor",   &waterRendering->baseColor[0]);
			glslShaders[n]->SetUniform3v("waterAbsorbColor", &waterRendering->absorb[0]);

			glslShaders[n]->SetUniform4v("splatTexScales", &mapRendering->splatTexScales[0]);
			glslShaders[n]->SetUniform4v("splatTexMults", &mapRendering->splatTexMults[0]);

			glslShaders[n]->SetUniform("infoTexIntensityMul", 1.0f);

			glslShaders[n]->SetUniform(  "normalTexGen", 1.0f / ((normTexSize.x - 1) * SQUARE_SIZE), 1.0f / ((normTexSize.y - 1) * SQUARE_SIZE));
			glslShaders[n]->SetUniform("specularTexGen", 1.0f / (   mapDims.mapx     * SQUARE_SIZE), 1.0f / (   mapDims.mapy     * SQUARE_SIZE));
			glslShaders[n]->SetUniform(    "infoTexGen", 1.0f / (   mapDims.pwr2mapx * SQUARE_SIZE), 1.0f / (   mapDims.pwr2mapy * SQUARE_SIZE));

			glslShaders[n]->Disable();
			glslShaders[n]->Validate();
		}
	}
}

bool SMFRenderStateGLSL::HasValidShader(const DrawPass::e& drawPass) const {
	Shader::IProgramObject* shader = nullptr;

	switch (drawPass) {
		case DrawPass::TerrainDeferred: { shader = glslShaders[GLSL_SHADER_DEFERRED]; } break;
		default:                        { shader = glslShaders[GLSL_SHADER_CURRENT ]; } break;
	}

	return (shader != nullptr && shader->IsValid());
}



bool SMFRenderStateFFP::CanEnable(const CSMFGroundDrawer* smfGroundDrawer) const {
	return (!smfGroundDrawer->UseAdvShading());
}

bool SMFRenderStateGLSL::CanEnable(const CSMFGroundDrawer* smfGroundDrawer) const {
	return (smfGroundDrawer->UseAdvShading());
}



void SMFRenderStateFFP::Enable(const CSMFGroundDrawer* smfGroundDrawer, const DrawPass::e&) {
	const CSMFReadMap* smfMap = smfGroundDrawer->GetReadMap();
	static const GLfloat planeX[] = {0.02f, 0.0f, 0.00f, 0.0f};
	static const GLfloat planeZ[] = {0.00f, 0.0f, 0.02f, 0.0f};

	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

	if (infoTextureHandler->IsEnabled()) {
		glActiveTexture(GL_TEXTURE1);
		glEnable(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, infoTextureHandler->GetCurrentInfoTexture());
		glMultiTexCoord4f(GL_TEXTURE1_ARB, 1.0f, 1.0f, 1.0f, 1.0f); // fix nvidia bug with gltexgen
		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_ARB, GL_ADD_SIGNED_ARB);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_ARB);
		SetTexGen(1.0f / (mapDims.pwr2mapx * SQUARE_SIZE), 1.0f / (mapDims.pwr2mapy * SQUARE_SIZE), 0, 0);


		glActiveTexture(GL_TEXTURE2);
		glEnable(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, smfMap->GetShadingTexture());
		glMultiTexCoord4f(GL_TEXTURE2_ARB, 1.0f, 1.0f, 1.0f, 1.0f); // fix nvidia bug with gltexgen

		if (infoTextureHandler->InMetalMode()) {
			// increase brightness for metal spots
			glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_ARB, GL_ADD_SIGNED_ARB);
			glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_ARB);
		}

		SetTexGen(1.0f / (mapDims.pwr2mapx * SQUARE_SIZE), 1.0f / (mapDims.pwr2mapy * SQUARE_SIZE), 0, 0);


		glActiveTexture(GL_TEXTURE3);
		if (smfMap->GetDetailTexture() != 0) {
			glEnable(GL_TEXTURE_2D);
			glBindTexture(GL_TEXTURE_2D, smfMap->GetDetailTexture());
			glMultiTexCoord4f(GL_TEXTURE3_ARB, 1.0f, 1.0f, 1.0f, 1.0f); // fix nvidia bug with gltexgen
			glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_ARB, GL_ADD_SIGNED_ARB);
			glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_ARB);

			glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
			glTexGenfv(GL_S, GL_OBJECT_PLANE, planeX);
			glEnable(GL_TEXTURE_GEN_S);

			glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
			glTexGenfv(GL_T, GL_OBJECT_PLANE, planeZ);
			glEnable(GL_TEXTURE_GEN_T);
		} else {
			glDisable(GL_TEXTURE_2D);
		}
	} else {
		if (smfMap->GetDetailTexture()) {
			glActiveTexture(GL_TEXTURE1);
			glEnable(GL_TEXTURE_2D);
			glBindTexture(GL_TEXTURE_2D, smfMap->GetDetailTexture());
			glMultiTexCoord4f(GL_TEXTURE1_ARB, 1.0f, 1.0f, 1.0f, 1.0f); // fix nvidia bug with gltexgen
			glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_ARB, GL_ADD_SIGNED_ARB);
			glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_ARB);

			glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
			glTexGenfv(GL_S, GL_OBJECT_PLANE, planeX);
			glEnable(GL_TEXTURE_GEN_S);

			glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
			glTexGenfv(GL_T, GL_OBJECT_PLANE, planeZ);
			glEnable(GL_TEXTURE_GEN_T);
		} else {
			glActiveTexture(GL_TEXTURE1);
			glDisable(GL_TEXTURE_2D);
		}


		glActiveTexture(GL_TEXTURE2);
		glEnable(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, smfMap->GetShadingTexture());
		glMultiTexCoord4f(GL_TEXTURE2_ARB, 1.0f, 1.0f, 1.0f, 1.0f); // fix nvidia bug with gltexgen
		SetTexGen(1.0f / (mapDims.pwr2mapx * SQUARE_SIZE), 1.0f / (mapDims.pwr2mapy * SQUARE_SIZE), -0.5f / mapDims.pwr2mapx, -0.5f / mapDims.pwr2mapy);


		// bind the detail texture a 2nd time to increase the details (-> GL_ADD_SIGNED_ARB is limited -0.5 to +0.5)
		// (also do this after the shading texture cause of color clamping issues)
		if (smfMap->GetDetailTexture()) {
			glActiveTexture(GL_TEXTURE3);
			glEnable(GL_TEXTURE_2D);
			glBindTexture(GL_TEXTURE_2D, smfMap->GetDetailTexture());
			glMultiTexCoord4f(GL_TEXTURE3_ARB, 1.0f, 1.0f, 1.0f, 1.0f); // fix nvidia bug with gltexgen
			glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_ARB, GL_ADD_SIGNED_ARB);
			glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_ARB);

			glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
			glTexGenfv(GL_S, GL_OBJECT_PLANE, planeX);
			glEnable(GL_TEXTURE_GEN_S);

			glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
			glTexGenfv(GL_T, GL_OBJECT_PLANE, planeZ);
			glEnable(GL_TEXTURE_GEN_T);
		} else {
			glActiveTexture(GL_TEXTURE3);
			glDisable(GL_TEXTURE_2D);
		}
	}

	glActiveTexture(GL_TEXTURE0);
	glEnable(GL_TEXTURE_2D);
}

void SMFRenderStateFFP::Disable(const CSMFGroundDrawer*, const DrawPass::e&) {
	glActiveTexture(GL_TEXTURE3);
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_TEXTURE_GEN_S);
	glDisable(GL_TEXTURE_GEN_T);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

	glActiveTexture(GL_TEXTURE2);
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_TEXTURE_GEN_S);
	glDisable(GL_TEXTURE_GEN_T);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

	glActiveTexture(GL_TEXTURE1);
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_TEXTURE_GEN_S);
	glDisable(GL_TEXTURE_GEN_T);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

	glActiveTexture(GL_TEXTURE0);
	glDisable(GL_TEXTURE_GEN_S);
	glDisable(GL_TEXTURE_GEN_T);
	glDisable(GL_TEXTURE_2D);

}

void SMFRenderStateGLSL::Enable(const CSMFGroundDrawer* smfGroundDrawer, const DrawPass::e&) {
	if (useLuaShaders) {
		// use raw, GLSLProgramObject::Enable also calls RecompileIfNeeded
		glslShaders[GLSL_SHADER_CURRENT]->EnableRaw();
		// diffuse textures are always bound (SMFGroundDrawer::SetupBigSquare)
		glActiveTexture(GL_TEXTURE0);
		return;
	}

	const CSMFReadMap* smfMap = smfGroundDrawer->GetReadMap();
	const GL::LightHandler* cLightHandler = smfGroundDrawer->GetLightHandler();
	      GL::LightHandler* mLightHandler = const_cast<GL::LightHandler*>(cLightHandler); // XXX

	glslShaders[GLSL_SHADER_CURRENT]->SetFlag("HAVE_SHADOWS", shadowHandler.ShadowsLoaded());
	glslShaders[GLSL_SHADER_CURRENT]->SetFlag("HAVE_INFOTEX", infoTextureHandler->IsEnabled());

	glslShaders[GLSL_SHADER_CURRENT]->Enable();
	glslShaders[GLSL_SHADER_CURRENT]->SetUniform("mapHeights", readMap->GetCurrMinHeight(), readMap->GetCurrMaxHeight());
	glslShaders[GLSL_SHADER_CURRENT]->SetUniform3v("cameraPos", &camera->GetPos()[0]);
	glslShaders[GLSL_SHADER_CURRENT]->SetUniformMatrix4x4("shadowMat", false, shadowHandler.GetShadowMatrixRaw());
	glslShaders[GLSL_SHADER_CURRENT]->SetUniform("infoTexIntensityMul", float(infoTextureHandler->InMetalMode()) + 1.0f);

	// already on the MV stack at this point
	glLoadIdentity();
	mLightHandler->Update(glslShaders[GLSL_SHADER_CURRENT]);
	glMultMatrixf(camera->GetViewMatrix());

	if (shadowHandler.ShadowsLoaded()) {
		shadowHandler.SetupShadowTexSampler(GL_TEXTURE4);
		glActiveTexture(GL_TEXTURE19); glBindTexture(GL_TEXTURE_2D, shadowHandler.GetColorTextureID());
	}

	glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, smfMap->GetDetailTexture());
	glActiveTexture(GL_TEXTURE5); glBindTexture(GL_TEXTURE_2D, smfMap->GetNormalsTexture());
	glActiveTexture(GL_TEXTURE6); glBindTexture(GL_TEXTURE_2D, smfMap->GetSpecularTexture());
	glActiveTexture(GL_TEXTURE7); glBindTexture(GL_TEXTURE_2D, smfMap->GetSplatDetailTexture());
	glActiveTexture(GL_TEXTURE8); glBindTexture(GL_TEXTURE_2D, smfMap->GetSplatDistrTexture());
	glActiveTexture(GL_TEXTURE9); glBindTexture(GL_TEXTURE_CUBE_MAP_ARB, cubeMapHandler.GetSkyReflectionTextureID());
	glActiveTexture(GL_TEXTURE10); glBindTexture(GL_TEXTURE_2D, smfMap->GetSkyReflectModTexture());
	glActiveTexture(GL_TEXTURE11); glBindTexture(GL_TEXTURE_2D, smfMap->GetBlendNormalsTexture());
	glActiveTexture(GL_TEXTURE12); glBindTexture(GL_TEXTURE_2D, smfMap->GetLightEmissionTexture());
	glActiveTexture(GL_TEXTURE13); glBindTexture(GL_TEXTURE_2D, smfMap->GetParallaxHeightTexture());
	glActiveTexture(GL_TEXTURE14); glBindTexture(GL_TEXTURE_2D, infoTextureHandler->GetCurrentInfoTexture());

	for (int i = 0; i < CSMFReadMap::NUM_SPLAT_DETAIL_NORMALS; i++) {
		if (smfMap->GetSplatNormalTexture(i) != 0) {
			glActiveTexture(GL_TEXTURE15 + i); glBindTexture(GL_TEXTURE_2D, smfMap->GetSplatNormalTexture(i));
		}
	}

	glActiveTexture(GL_TEXTURE0);
}

void SMFRenderStateGLSL::Disable(const CSMFGroundDrawer*, const DrawPass::e&) {
	if (useLuaShaders) {
		glActiveTexture(GL_TEXTURE0);
		glslShaders[GLSL_SHADER_CURRENT]->DisableRaw();
		return;
	}

	if (shadowHandler.ShadowsLoaded()) {
		glActiveTexture(GL_TEXTURE4);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
	}

	glActiveTexture(GL_TEXTURE0);
	glslShaders[GLSL_SHADER_CURRENT]->Disable();
}






void SMFRenderStateFFP::SetSquareTexGen(const int sqx, const int sqy) const {
	// const CSMFReadMap* smfMap = smfGroundDrawer->GetReadMap();
	// const float smfTexSquareSizeInv = 1.0f / smfMap->bigTexSize;

	SetTexGen(1.0f / SMF_TEXSQUARE_SIZE, 1.0f / SMF_TEXSQUARE_SIZE, -sqx, -sqy);
}

void SMFRenderStateGLSL::SetSquareTexGen(const int sqx, const int sqy) const {
	// needs to be set even for Lua shaders, is unknowable otherwise
	// (works because SMFGroundDrawer::SetupBigSquare always calls us)
	glslShaders[GLSL_SHADER_CURRENT]->SetUniform("texSquare", sqx, sqy);
}


void SMFRenderStateGLSL::SetCurrentShader(const DrawPass::e& drawPass) {
	switch (drawPass) {
		case DrawPass::TerrainDeferred: { glslShaders[GLSL_SHADER_CURRENT] = glslShaders[GLSL_SHADER_DEFERRED]; } break;
		default:                        { glslShaders[GLSL_SHADER_CURRENT] = glslShaders[GLSL_SHADER_STANDARD]; } break;
	}
}

void SMFRenderStateGLSL::UpdateCurrentShaderSky(const ISkyLight* skyLight) const {
	glslShaders[GLSL_SHADER_CURRENT]->Enable();
	glslShaders[GLSL_SHADER_CURRENT]->SetUniform4v("lightDir", &skyLight->GetLightDir().x);
	glslShaders[GLSL_SHADER_CURRENT]->SetUniform("groundShadowDensity", sunLighting->groundShadowDensity);
	glslShaders[GLSL_SHADER_CURRENT]->SetUniform3v("groundAmbientColor",  &sunLighting->groundAmbientColor[0]);
	glslShaders[GLSL_SHADER_CURRENT]->SetUniform3v("groundDiffuseColor",  &sunLighting->groundDiffuseColor[0]);
	glslShaders[GLSL_SHADER_CURRENT]->SetUniform3v("groundSpecularColor", &sunLighting->groundSpecularColor[0]);
	glslShaders[GLSL_SHADER_CURRENT]->Disable();
}
