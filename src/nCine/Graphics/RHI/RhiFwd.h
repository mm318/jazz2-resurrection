#pragma once

#include "RhiTypes.h"

#include <cstdint>

// Compile-time RHI backend selection — exactly one backend is compiled into a binary. The OpenGL
// family backend (OpenGL 3.3 core / OpenGL ES 3.0 / WebGL 2 / ANGLE) is the default when no
// `WITH_RHI_*` macro is defined by the build.
#if !(defined(WITH_RHI_GL)) && !(defined(WITH_RHI_D3D11)) && !(defined(WITH_RHI_VULKAN)) && !(defined(WITH_RHI_METAL))
#define WITH_RHI_GL
#endif

#if defined(WITH_RHI_GL)

// Rendering capability flags of the selected backend. Pipeline code guarded by these compiles only when the
// active backend advertises the capability. `RHI_CAP_SHADERS` means the backend has hardware programmable
// shaders cheap enough to drive full-screen post-processing (the bloom Blur/Downsample/Combine chain, the
// shader lighting compositing and the rescale/antialiasing passes); `RHI_CAP_FRAMEBUFFERS` means off-screen
// render targets are available. The OpenGL family backend provides both.
//
// `RHI_CAP_HEAVY_RESCALE_SHADERS` means the heaviest of the shipped rescale filters - CleanEdge, SABR and
// Monochrome - are actually available. They are ordinary shaders everywhere a compiler runs at load time or
// targets a modern profile, so every such backend advertises this; a backend that compiles its shaders
// OFFLINE against a fixed profile may find one rejected, which is a build-time fact its menu has to respect
// (see the RSX arm). Anything gated on it must also be reachable without it - the modes simply disappear.
// `RHI_CAP_BATCHING` means CPU-side batching works: many sprites that share a material are collected into
// one draw whose per-instance data travels in a uniform block, and the backend must be able to run the
// batched twin of a shader (which indexes that block) for it to be worth anything. A backend that leaves it
// undefined never receives a batched shader at all - @ref RenderResources::GetBatchedShader reports none, so
// @ref RenderBatcher collects nothing and every command is submitted on its own. Correct, just more draws.
// It is a build-time fact everywhere (a shader profile either expresses the indexing or it does not), which
// is why it lives here rather than among the runtime @ref IRhiCapabilities values.
//
// `RHI_CAP_TRANSPARENT_RENDER_TARGETS` means a render target keeps an alpha channel - a single bit of it is
// enough - that the draws into it write and a later draw sampling the target blends by, so whatever a pass
// leaves undrawn stays see-through where its texture is drawn over the scene. Every backend of the shader tier
// has it (the HUD overlay layer is such a target); of the direct tier only those whose target surfaces carry
// alpha do, as several render into RGB565 and have none. Without it, a pass meant to be composited over the
// scene is drawn onto it directly instead.
//
// Every backend renders TOP-DOWN, the convention of Direct3D, Metal and the consoles: clip-space y = +1 is the
// top edge of the viewport for a pass onto the screen and for a pass into a render target alike, the first row
// of a render target is its top one - so a texture coordinate v = 0 samples the top of what was rendered,
// exactly as it samples the first row of an uploaded image - and viewport and scissor rectangles are pixels of
// the target counted from its top-left corner. Cameras are therefore Y-down wherever they render, and a pass
// that renders into a texture uses the same projection as one that renders onto the screen.
//
// `RHI_RENDER_TARGETS_BOTTOM_UP` means the backend stores a render target the OpenGL way, with the row at
// clip-space y = -1 first. The engine then flips clip-space Y of every pass that renders into one (see
// RenderResources::UpdateCameraUniforms()), which lands the top of the pass on the first row after all; a pass
// onto the screen needs no flip, since what is displayed is top-down already. OpenGL is the only family that
// defines it - its window-space origin is the bottom-left corner - and it also converts the viewport and
// scissor rectangles of a pass onto the screen (see GLDevice::SetViewport()).
#define RHI_CAP_SHADERS
#define RHI_CAP_FRAMEBUFFERS
#define RHI_CAP_HEAVY_RESCALE_SHADERS
#define RHI_CAP_TRANSPARENT_RENDER_TARGETS
#define RHI_RENDER_TARGETS_BOTTOM_UP
#if !defined(RHI_GL_PROFILE_ES2)
// ES2 has no uniform buffer objects to put the instance array in, and the batched programs' ESSL 100 form is
// not even valid there (a "uint aMeshIndex" integer attribute), so that profile does not batch
#define RHI_CAP_BATCHING
#endif
// `RHI_CAP_AUXILIARY_SWAPCHAINS` means the device can show a texture in a window besides the main one through
// `CreateAuxiliarySwapchain()`, `ResizeAuxiliarySwapchain()`, `PresentAuxiliaryTexture()` and
// `DestroyAuxiliarySwapchain()` (see @ref RHI::AuxiliarySwapchainDesc). Desktop OpenGL copies it in a context of
// that window sharing the objects of the main one, which OpenGL|ES and WebGL can't be relied on to provide.
#if !defined(RHI_GL_PROFILE_ES)
#define RHI_CAP_AUXILIARY_SWAPCHAINS
#endif

namespace nCine::RHI::GL
{
	class GLDevice;
	class GLTexture;
	class GLBufferObject;
	class GLShader;
	class GLShaderProgram;
	class GLShaderUniforms;
	class GLShaderUniformBlocks;
	class GLUniform;
	class GLUniformBlock;
	class GLUniformCache;
	class GLUniformBlockCache;
	class GLAttribute;
	class GLFramebuffer;
	class GLRenderbuffer;
	class GLRenderTarget;
	class GLVertexArrayObject;
	class GLVertexFormat;
	class GLRhiCapabilities;
	class GLDebug;
}

namespace nCine::RHI
{
	// Backend-neutral names for the classes of the selected backend. The render pipeline only refers
	// to these aliases, so that additional backends only have to provide the same set of names with
	// the same surface. This header only forward-declares them — include `Rhi.h` for the definitions.
	using Device = RHI::GL::GLDevice;
	using Texture = RHI::GL::GLTexture;
	using Buffer = RHI::GL::GLBufferObject;
	using Shader = RHI::GL::GLShader;
	using ShaderProgram = RHI::GL::GLShaderProgram;
	using ShaderUniforms = RHI::GL::GLShaderUniforms;
	using ShaderUniformBlocks = RHI::GL::GLShaderUniformBlocks;
	using Uniform = RHI::GL::GLUniform;
	using UniformBlock = RHI::GL::GLUniformBlock;
	using UniformCache = RHI::GL::GLUniformCache;
	using UniformBlockCache = RHI::GL::GLUniformBlockCache;
	using Attribute = RHI::GL::GLAttribute;
	using Framebuffer = RHI::GL::GLFramebuffer;
	using Renderbuffer = RHI::GL::GLRenderbuffer;
	using RenderTarget = RHI::GL::GLRenderTarget;
	using VertexArray = RHI::GL::GLVertexArrayObject;
	using VertexFormat = RHI::GL::GLVertexFormat;

	// Runtime capabilities of the selected backend
	using Capabilities = RHI::GL::GLRhiCapabilities;

	// Debug output and object labelling
	using Debug = RHI::GL::GLDebug;

	/**
		@brief Locates a sub-range within a buffer object, together with its mapped memory
	*/
	struct BufferRange
	{
		BufferRange()
			: object(nullptr), size(0), offset(0), mapBase(nullptr)
		{
		}

		/** @brief Buffer object the range belongs to */
		Buffer* object;
		/** @brief Size of the range in bytes */
		std::uint32_t size;
		/** @brief Byte offset of the range within the buffer object */
		std::uint32_t offset;
		/** @brief Base pointer of the mapped (or host) buffer memory */
		std::uint8_t* mapBase;
	};
}

#elif defined(WITH_RHI_D3D11)

// Rendering capability flags of the selected backend (see the OpenGL arm above for the meaning). The Direct3D
// 11 backend is a full-pipeline hardware backend like the OpenGL family: it has programmable shaders and
// off-screen render targets, so both `RHI_CAP_SHADERS` and `RHI_CAP_FRAMEBUFFERS` are defined and the pipeline
// runs the whole bloom / lighting / combine / rescale chain exactly as it does on OpenGL.
#define RHI_CAP_SHADERS
#define RHI_CAP_FRAMEBUFFERS
#define RHI_CAP_TRANSPARENT_RENDER_TARGETS
#define RHI_CAP_HEAVY_RESCALE_SHADERS
#define RHI_CAP_BATCHING
#define RHI_CAP_AUXILIARY_SWAPCHAINS

namespace nCine::RHI::D3D11
{
	class D3D11Device;
	class D3D11Texture;
	class D3D11BufferObject;
	class D3D11Shader;
	class D3D11ShaderProgram;
	class D3D11ShaderUniforms;
	class D3D11ShaderUniformBlocks;
	class D3D11Uniform;
	class D3D11UniformBlock;
	class D3D11UniformCache;
	class D3D11UniformBlockCache;
	class D3D11Attribute;
	class D3D11Framebuffer;
	class D3D11Renderbuffer;
	class D3D11RenderTarget;
	class D3D11VertexArray;
	class D3D11VertexFormat;
	class D3D11RhiCapabilities;
	class D3D11Debug;
}

namespace nCine::RHI
{
	// Backend-neutral names for the classes of the selected backend. The render pipeline only refers
	// to these aliases, so that additional backends only have to provide the same set of names with
	// the same surface. This header only forward-declares them — include `Rhi.h` for the definitions.
	using Device = RHI::D3D11::D3D11Device;
	using Texture = RHI::D3D11::D3D11Texture;
	using Buffer = RHI::D3D11::D3D11BufferObject;
	using Shader = RHI::D3D11::D3D11Shader;
	using ShaderProgram = RHI::D3D11::D3D11ShaderProgram;
	using ShaderUniforms = RHI::D3D11::D3D11ShaderUniforms;
	using ShaderUniformBlocks = RHI::D3D11::D3D11ShaderUniformBlocks;
	using Uniform = RHI::D3D11::D3D11Uniform;
	using UniformBlock = RHI::D3D11::D3D11UniformBlock;
	using UniformCache = RHI::D3D11::D3D11UniformCache;
	using UniformBlockCache = RHI::D3D11::D3D11UniformBlockCache;
	using Attribute = RHI::D3D11::D3D11Attribute;
	using Framebuffer = RHI::D3D11::D3D11Framebuffer;
	using Renderbuffer = RHI::D3D11::D3D11Renderbuffer;
	using RenderTarget = RHI::D3D11::D3D11RenderTarget;
	using VertexArray = RHI::D3D11::D3D11VertexArray;
	using VertexFormat = RHI::D3D11::D3D11VertexFormat;

	// Runtime capabilities of the selected backend
	using Capabilities = RHI::D3D11::D3D11RhiCapabilities;

	// Debug output and object labelling
	using Debug = RHI::D3D11::D3D11Debug;

	/**
		@brief Locates a sub-range within a buffer object, together with its mapped memory
	*/
	struct BufferRange
	{
		BufferRange()
			: object(nullptr), size(0), offset(0), mapBase(nullptr)
		{
		}

		/** @brief Buffer object the range belongs to */
		Buffer* object;
		/** @brief Size of the range in bytes */
		std::uint32_t size;
		/** @brief Byte offset of the range within the buffer object */
		std::uint32_t offset;
		/** @brief Base pointer of the mapped (or host) buffer memory */
		std::uint8_t* mapBase;
	};
}

#elif defined(WITH_RHI_VULKAN)

// Rendering capability flags of the selected backend (see the OpenGL arm above for the meaning). Vulkan is a
// full-pipeline hardware backend like the OpenGL family and Direct3D 11: it has programmable shaders (built
// offline as SPIR-V, embedded per program-variant) and off-screen render targets, so both `RHI_CAP_SHADERS`
// and `RHI_CAP_FRAMEBUFFERS` are defined and the pipeline runs the whole bloom / lighting / combine / rescale
// chain exactly as it does on OpenGL. SPIR-V has no profile limits to trip over, so every rescale filter
// compiles and `RHI_CAP_HEAVY_RESCALE_SHADERS` holds as well.
#define RHI_CAP_SHADERS
#define RHI_CAP_FRAMEBUFFERS
#define RHI_CAP_TRANSPARENT_RENDER_TARGETS
#define RHI_CAP_HEAVY_RESCALE_SHADERS
#define RHI_CAP_BATCHING
#define RHI_CAP_AUXILIARY_SWAPCHAINS

namespace nCine::RHI::Vulkan
{
	class VulkanDevice;
	class VulkanTexture;
	class VulkanBufferObject;
	class VulkanShader;
	class VulkanShaderProgram;
	class VulkanShaderUniforms;
	class VulkanShaderUniformBlocks;
	class VulkanUniform;
	class VulkanUniformBlock;
	class VulkanUniformCache;
	class VulkanUniformBlockCache;
	class VulkanAttribute;
	class VulkanFramebuffer;
	class VulkanRenderbuffer;
	class VulkanRenderTarget;
	class VulkanVertexArray;
	class VulkanVertexFormat;
	class VulkanRhiCapabilities;
	class VulkanDebug;
}

namespace nCine::RHI
{
	// Backend-neutral names for the classes of the selected backend. The render pipeline only refers
	// to these aliases, so that additional backends only have to provide the same set of names with
	// the same surface. This header only forward-declares them — include `Rhi.h` for the definitions.
	using Device = RHI::Vulkan::VulkanDevice;
	using Texture = RHI::Vulkan::VulkanTexture;
	using Buffer = RHI::Vulkan::VulkanBufferObject;
	using Shader = RHI::Vulkan::VulkanShader;
	using ShaderProgram = RHI::Vulkan::VulkanShaderProgram;
	using ShaderUniforms = RHI::Vulkan::VulkanShaderUniforms;
	using ShaderUniformBlocks = RHI::Vulkan::VulkanShaderUniformBlocks;
	using Uniform = RHI::Vulkan::VulkanUniform;
	using UniformBlock = RHI::Vulkan::VulkanUniformBlock;
	using UniformCache = RHI::Vulkan::VulkanUniformCache;
	using UniformBlockCache = RHI::Vulkan::VulkanUniformBlockCache;
	using Attribute = RHI::Vulkan::VulkanAttribute;
	using Framebuffer = RHI::Vulkan::VulkanFramebuffer;
	using Renderbuffer = RHI::Vulkan::VulkanRenderbuffer;
	using RenderTarget = RHI::Vulkan::VulkanRenderTarget;
	using VertexArray = RHI::Vulkan::VulkanVertexArray;
	using VertexFormat = RHI::Vulkan::VulkanVertexFormat;

	// Runtime capabilities of the selected backend
	using Capabilities = RHI::Vulkan::VulkanRhiCapabilities;

	// Debug output and object labelling
	using Debug = RHI::Vulkan::VulkanDebug;

	/**
		@brief Locates a sub-range within a buffer object, together with its mapped memory
	*/
	struct BufferRange
	{
		BufferRange()
			: object(nullptr), size(0), offset(0), mapBase(nullptr)
		{
		}

		/** @brief Buffer object the range belongs to */
		Buffer* object;
		/** @brief Size of the range in bytes */
		std::uint32_t size;
		/** @brief Byte offset of the range within the buffer object */
		std::uint32_t offset;
		/** @brief Base pointer of the mapped (or host) buffer memory */
		std::uint8_t* mapBase;
	};
}

#elif defined(WITH_RHI_METAL)

// Rendering capability flags of the selected backend (see the OpenGL arm above for the meaning). Metal is a
// full-pipeline hardware backend like the OpenGL family, Direct3D 11 and Vulkan: it has programmable shaders
// (the offline MSL lowering, compiled by the driver at load time) and off-screen render targets, so both
// `RHI_CAP_SHADERS` and `RHI_CAP_FRAMEBUFFERS` are defined and the pipeline runs the whole bloom / lighting /
// combine / rescale chain exactly as it does on OpenGL. MSL has no profile limits to trip over, so every rescale
// filter compiles and `RHI_CAP_HEAVY_RESCALE_SHADERS` holds as well.
#define RHI_CAP_SHADERS
#define RHI_CAP_FRAMEBUFFERS
#define RHI_CAP_TRANSPARENT_RENDER_TARGETS
#define RHI_CAP_HEAVY_RESCALE_SHADERS
#define RHI_CAP_BATCHING
#define RHI_CAP_AUXILIARY_SWAPCHAINS

namespace nCine::RHI::Metal
{
	class MetalDevice;
	class MetalTexture;
	class MetalBufferObject;
	class MetalShader;
	class MetalShaderProgram;
	class MetalShaderUniforms;
	class MetalShaderUniformBlocks;
	class MetalUniform;
	class MetalUniformBlock;
	class MetalUniformCache;
	class MetalUniformBlockCache;
	class MetalAttribute;
	class MetalFramebuffer;
	class MetalRenderbuffer;
	class MetalRenderTarget;
	class MetalVertexArray;
	class MetalVertexFormat;
	class MetalRhiCapabilities;
	class MetalDebug;
}

namespace nCine::RHI
{
	// Backend-neutral names for the classes of the selected backend. The render pipeline only refers
	// to these aliases, so that additional backends only have to provide the same set of names with
	// the same surface. This header only forward-declares them — include `Rhi.h` for the definitions.
	using Device = RHI::Metal::MetalDevice;
	using Texture = RHI::Metal::MetalTexture;
	using Buffer = RHI::Metal::MetalBufferObject;
	using Shader = RHI::Metal::MetalShader;
	using ShaderProgram = RHI::Metal::MetalShaderProgram;
	using ShaderUniforms = RHI::Metal::MetalShaderUniforms;
	using ShaderUniformBlocks = RHI::Metal::MetalShaderUniformBlocks;
	using Uniform = RHI::Metal::MetalUniform;
	using UniformBlock = RHI::Metal::MetalUniformBlock;
	using UniformCache = RHI::Metal::MetalUniformCache;
	using UniformBlockCache = RHI::Metal::MetalUniformBlockCache;
	using Attribute = RHI::Metal::MetalAttribute;
	using Framebuffer = RHI::Metal::MetalFramebuffer;
	using Renderbuffer = RHI::Metal::MetalRenderbuffer;
	using RenderTarget = RHI::Metal::MetalRenderTarget;
	using VertexArray = RHI::Metal::MetalVertexArray;
	using VertexFormat = RHI::Metal::MetalVertexFormat;

	// Runtime capabilities of the selected backend
	using Capabilities = RHI::Metal::MetalRhiCapabilities;

	// Debug output and object labelling
	using Debug = RHI::Metal::MetalDebug;

	/**
		@brief Locates a sub-range within a buffer object, together with its mapped memory
	*/
	struct BufferRange
	{
		BufferRange()
			: object(nullptr), size(0), offset(0), mapBase(nullptr)
		{
		}

		/** @brief Buffer object the range belongs to */
		Buffer* object;
		/** @brief Size of the range in bytes */
		std::uint32_t size;
		/** @brief Byte offset of the range within the buffer object */
		std::uint32_t offset;
		/** @brief Base pointer of the mapped (or host) buffer memory */
		std::uint8_t* mapBase;
	};
}

#else
#error No RHI backend selected - define WITH_RHI_GL (or another WITH_RHI_* backend)
#endif

// Derived tier macro: the full GPU post-processing pipeline (scene FBO + lighting FBO + blur chain +
// rescale/antialiasing passes) requires BOTH programmable shaders and off-screen render targets, so
// game code gates it on this single macro instead of ad-hoc compounds of the two capabilities (or a
// backend-identity test like WITH_RHI_SOFTWARE, which would silently misroute a future fixed-function
// console backend). Backends lacking either capability take the direct tier: the scene renders
// straight into the screen buffer at the logical resolution (the backend's presentation stretches it
// to the display - a backend without RHI_CAP_SHADERS must provide Device::ResizeScreenFramebuffer())
// and lighting is composited by the CPU lightmap path in CombineRenderer.
#define RHI_CAP_POSTPROCESSING

// Derived tier macro: this build's shaders were assembled with the `.shader` language's LOW_POWER_GPU
// conditional resolved as DEFINED, so wherever such a block substitutes a cheaper approximation for an
// expensive per-pixel path, this binary got the substitute. Unlike the capability macros above it says
// nothing about what the backend CAN do - both of the backends below run every shipped shader as written
// - only about how much per-pixel work the part can sustain. Game code gates on it when an option would
// be steering something the shader no longer contains (see the background dithering item in
// GraphicsOptionsSection), which would otherwise be a menu entry with no effect.
//
// It must stay in lockstep with what actually builds the shader sources with the macro defined: the
// sceGxm backend, whose Cg is emitted offline by `ShaderCompiler --emit-cg` (the PS Vita's PowerVR
// SGX543 - a handheld part shading a full screen from a shared memory bus), and the OpenGL family's ES2
// profile, whose baked ESSL 100 sources come from the same view (see Emit.cpp) and whose runtime-compiled
// ".shader" files are assembled that way too (see RuntimeShader.cpp). That profile is never a default -
// it is selected only for hardware with nothing else, which is the same class of part.
#if defined(RHI_GL_PROFILE_ES2)
#define RHI_LOW_POWER_GPU
#endif
