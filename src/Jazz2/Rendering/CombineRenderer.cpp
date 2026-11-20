#include "CombineRenderer.h"
#include "PlayerViewport.h"
#include "../PreferencesCache.h"

#include "../../nCine/Graphics/RenderQueue.h"

#include "SoftwareLightSplat.h"

namespace Jazz2::Rendering
{

	CombineRenderer::CombineRenderer(PlayerViewport* owner)
		: _owner(owner)
	{
		SetVisitOrderState(SceneNode::VisitOrderState::Disabled);
	}

	void CombineRenderer::Initialize(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height)
	{
		_bounds = Rectf(static_cast<float>(x), static_cast<float>(y), static_cast<float>(width), static_cast<float>(height));

		if (_renderCommand.GetMaterial().SetShader(_owner->_levelHandler->_combineShader)) {
			_renderCommand.GetMaterial().ReserveUniformsDataMemory();
			_renderCommand.GetGeometry().SetDrawParameters(PrimitiveType::TriangleStrip, 0, 4);
			auto* textureUniform = _renderCommand.GetMaterial().Uniform(Material::TextureUniformName);
			if (textureUniform != nullptr) {
				textureUniform->SetIntValue(0); // GL_TEXTURE0
			}
			auto* lightTexUniform = _renderCommand.GetMaterial().Uniform("uTextureLighting");
			if (lightTexUniform != nullptr) {
				lightTexUniform->SetIntValue(1); // GL_TEXTURE1
			}
			auto* blurHalfTexUniform = _renderCommand.GetMaterial().Uniform("uTextureBlurHalf");
			if (blurHalfTexUniform != nullptr) {
				blurHalfTexUniform->SetIntValue(2); // GL_TEXTURE2
			}
			auto* blurQuarterTexUniform = _renderCommand.GetMaterial().Uniform("uTextureBlurQuarter");
			if (blurQuarterTexUniform != nullptr) {
				blurQuarterTexUniform->SetIntValue(3); // GL_TEXTURE3
			}
		}

		if (_renderCommandWithWater.GetMaterial().SetShader(_owner->_levelHandler->_combineWithWaterShader)) {
			_renderCommandWithWater.GetMaterial().ReserveUniformsDataMemory();
			_renderCommandWithWater.GetGeometry().SetDrawParameters(PrimitiveType::TriangleStrip, 0, 4);
			auto* textureUniform = _renderCommandWithWater.GetMaterial().Uniform(Material::TextureUniformName);
			if (textureUniform != nullptr) {
				textureUniform->SetIntValue(0); // GL_TEXTURE0
			}
			auto* lightTexUniform = _renderCommandWithWater.GetMaterial().Uniform("uTextureLighting");
			if (lightTexUniform != nullptr) {
				lightTexUniform->SetIntValue(1); // GL_TEXTURE1
			}
			auto* blurHalfTexUniform = _renderCommandWithWater.GetMaterial().Uniform("uTextureBlurHalf");
			if (blurHalfTexUniform != nullptr) {
				blurHalfTexUniform->SetIntValue(2); // GL_TEXTURE2
			}
			auto* blurQuarterTexUniform = _renderCommandWithWater.GetMaterial().Uniform("uTextureBlurQuarter");
			if (blurQuarterTexUniform != nullptr) {
				blurQuarterTexUniform->SetIntValue(3); // GL_TEXTURE3
			}
			auto* noiseTexUniform = _renderCommandWithWater.GetMaterial().Uniform("uTextureNoise");
			if (noiseTexUniform != nullptr) {
				noiseTexUniform->SetIntValue(4); // GL_TEXTURE4
			}
		}

		_renderCommand.SetTransformation(Matrix4x4f::Translation((float)x, (float)y, 0.0f));
		_renderCommandWithWater.SetTransformation(Matrix4x4f::Translation((float)x, (float)y, 0.0f));
	}

	Rectf CombineRenderer::GetBounds() const
	{
		return _bounds;
	}

	bool CombineRenderer::OnDraw(RenderQueue& renderQueue)
	{
		float viewWaterLevel = (_owner->_levelHandler->_waterLevel - _owner->_cameraPos.Y) * _owner->_cameraScale + _bounds.H * 0.5f;
		bool viewHasWater = (viewWaterLevel < _bounds.H && !_owner->_levelHandler->_waterHidden);
		// The water drawn mid-scene (see PlayerViewport::DrawWater()) leaves only the darkness above it to the plain Combine
		const bool waterInScene = (viewHasWater && _owner->IsWaterInScene());
		auto& command = (viewHasWater && !waterInScene ? _renderCommandWithWater : _renderCommand);

		command.GetMaterial().SetTexture(0, *_owner->_viewTexture);
		command.GetMaterial().SetTexture(1, *_owner->_lightingBuffer);
		if (PreferencesCache::BlurEffects) {
			// The quarter-size level does not exist at a low lighting resolution, and the half-size one takes its place
			command.GetMaterial().SetTexture(2, *_owner->_blurPass2.GetTarget());
			command.GetMaterial().SetTexture(3, *_owner->GetBlurredTarget());
		} else {
			// The darkness fades the scene toward a desaturated copy of it, which is the blurred one when there is
			// one. Without it, the scene itself stands in: an unbound sampler reads black, which dimmed every unlit
			// pixel almost to black at a low ambient level instead of leaving it as bright as the light says.
			command.GetMaterial().SetTexture(2, *_owner->_viewTexture);
			command.GetMaterial().SetTexture(3, *_owner->_viewTexture);
		}
		if (viewHasWater && !waterInScene && !PreferencesCache::LowWaterQuality) {
			command.GetMaterial().SetTexture(4, *_owner->_levelHandler->_noiseTexture);
		}

		auto* instanceBlock = command.GetMaterial().UniformBlock(Material::InstanceBlockName);
		if DEATH_UNLIKELY (instanceBlock == nullptr) {
			// The program failed to link (a driver rejected it), so none of its uniforms exist - skip the pass
			// rather than dereference them every frame
			return false;
		}
		instanceBlock->GetUniform(Material::TexRectUniformName)->SetFloatValue(1.0f, 0.0f, 1.0f, 0.0f);
		instanceBlock->GetUniform(Material::SpriteSizeUniformName)->SetFloatValue(_bounds.W, _bounds.H);
		instanceBlock->GetUniform(Material::ColorUniformName)->SetFloatVector(Colorf::White.Data());

		command.GetMaterial().Uniform("uAmbientColor")->SetFloatVector(_owner->_ambientLight.Data());
		command.GetMaterial().Uniform("uTime")->SetFloatValue(_owner->_levelHandler->_elapsedFrames * 0.0018f);

		if (&command == &_renderCommand) {
			command.GetMaterial().Uniform("uWaterLevel")->SetFloatValue(waterInScene ? viewWaterLevel / _bounds.H : 1.0f);
			if (waterInScene) {
				command.GetMaterial().Uniform("uWavePhase")->SetFloatVector(_owner->GetWaterView().WavePhase.Data());
			}
		} else if (viewHasWater) {
			const PlayerViewport::WaterView water = _owner->GetWaterView();
			const Vector3f& top = _owner->_levelHandler->_waterColorTop;
			const Vector3f& bottom = _owner->_levelHandler->_waterColorBottom;
			command.GetMaterial().Uniform("uWaterLevel")->SetFloatValue(viewWaterLevel / _bounds.H);
			command.GetMaterial().Uniform("uCameraPos")->SetFloatValue(water.CameraPos.X, water.CameraPos.Y);
			command.GetMaterial().Uniform("uWavePhase")->SetFloatVector(water.WavePhase.Data());
			command.GetMaterial().Uniform("uWaterBase")->SetFloatVector(water.Base.Data());
			command.GetMaterial().Uniform("uWaterColorTop")->SetFloatValue(top.X, top.Y, top.Z, 1.0f);
			command.GetMaterial().Uniform("uWaterColorBottom")->SetFloatValue(bottom.X, bottom.Y, bottom.Z, 1.0f);
			// Only the high quality one has the light rays
			if (auto* uniform = command.GetMaterial().Uniform("uRaysBase")) {
				uniform->SetFloatValue(water.RaysBase);
			}
		}

		renderQueue.AddCommand(&command);

		return true;
	}

}