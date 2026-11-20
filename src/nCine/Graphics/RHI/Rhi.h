#pragma once

#include "RhiTypes.h"
#include "RhiFwd.h"

// Definitions of the classes aliased in `RhiFwd.h` for the selected backend

#if defined(WITH_RHI_GL)

#include "GL/GLDevice.h"
#include "GL/GLTexture.h"
#include "GL/GLBufferObject.h"
#include "GL/GLShader.h"
#include "GL/GLShaderProgram.h"
#include "GL/GLShaderUniforms.h"
#include "GL/GLShaderUniformBlocks.h"
#include "GL/GLUniform.h"
#include "GL/GLUniformBlock.h"
#include "GL/GLUniformCache.h"
#include "GL/GLUniformBlockCache.h"
#include "GL/GLAttribute.h"
#include "GL/GLFramebuffer.h"
#include "GL/GLRenderbuffer.h"
#include "GL/GLRenderTarget.h"
#include "GL/GLVertexArrayObject.h"
#include "GL/GLVertexFormat.h"
#include "GL/GLBlending.h"
#include "GL/GLDepthTest.h"
#include "GL/GLCullFace.h"
#include "GL/GLScissorTest.h"
#include "GL/GLClearColor.h"
#include "GL/GLViewport.h"
#include "GL/GLDebug.h"
#include "GL/GLRhiCapabilities.h"

#elif defined(WITH_RHI_D3D11)

#include "D3D11/D3D11Debug.h"
#include "D3D11/D3D11Shader.h"
#include "D3D11/D3D11BufferObject.h"
#include "D3D11/D3D11Texture.h"
#include "D3D11/D3D11VertexFormat.h"
#include "D3D11/D3D11ShaderTypes.h"
#include "D3D11/D3D11UniformCache.h"
#include "D3D11/D3D11ShaderUniforms.h"
#include "D3D11/D3D11ShaderProgram.h"
#include "D3D11/D3D11RenderTarget.h"
#include "D3D11/D3D11Device.h"
#include "D3D11/D3D11RhiCapabilities.h"

#elif defined(WITH_RHI_VULKAN)

#include "Vulkan/VulkanDebug.h"
#include "Vulkan/VulkanShader.h"
#include "Vulkan/VulkanBufferObject.h"
#include "Vulkan/VulkanTexture.h"
#include "Vulkan/VulkanVertexFormat.h"
#include "Vulkan/VulkanShaderTypes.h"
#include "Vulkan/VulkanUniformCache.h"
#include "Vulkan/VulkanShaderUniforms.h"
#include "Vulkan/VulkanShaderProgram.h"
#include "Vulkan/VulkanRenderTarget.h"
#include "Vulkan/VulkanDevice.h"
#include "Vulkan/VulkanRhiCapabilities.h"

#elif defined(WITH_RHI_METAL)

#include "Metal/MetalDebug.h"
#include "Metal/MetalShader.h"
#include "Metal/MetalBufferObject.h"
#include "Metal/MetalTexture.h"
#include "Metal/MetalVertexFormat.h"
#include "Metal/MetalShaderTypes.h"
#include "Metal/MetalUniformCache.h"
#include "Metal/MetalShaderUniforms.h"
#include "Metal/MetalShaderProgram.h"
#include "Metal/MetalRenderTarget.h"
#include "Metal/MetalDevice.h"
#include "Metal/MetalRhiCapabilities.h"

#endif
