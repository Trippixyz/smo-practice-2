#include "imgui_impl_nvn.hpp"
#include "helpers/fsHelper.h"
#include "imgui.h"
#include "imgui_hid_mappings.h"
#include "imgui_internal.h"
#include "lib.hpp"
#include <cmath>

#include "nn/hid.h"

#include "MemoryPoolMaker.h"
#include "helpers/InputHelper.h"
#include "util/sys/rw_pages.hpp"

#define UBOSIZE 0x1000

namespace ImguiNvnBackend {

void make_identity(Matrix44f& mtx)
{
    Matrix44f ident = {
        { 1.0f, 0.0f, 0.0f, 0.0f },
        { 0.0f, 1.0f, 0.0f, 0.0f },
        { 0.0f, 0.0f, 1.0f, 0.0f },
        { 0.0f, 0.0f, 0.0f, 1.0f }
    };
    memcpy(mtx, &ident, sizeof(Matrix44f));
}

void orthoRH_ZO(Matrix44f& mtx, float left, float right, float bottom, float top, float zNear, float zFar)
{
    make_identity(mtx);
    mtx[0][0] = 2.f / (right - left);
    mtx[1][1] = 2.f / (top - bottom);
    mtx[2][2] = -1.f / (zFar - zNear);
    mtx[3][0] = -(right + left) / (right - left);
    mtx[3][1] = -(top + bottom) / (top - bottom);
    mtx[3][2] = -zNear / (zFar - zNear);
}

// WIP ImGui Functions from docking branch
static void ScaleWindow(ImGuiWindow* window, float scale)
{
    ImVec2 origin = window->Viewport->Pos;
    window->Pos = ImFloor(
        ImVec2((window->Pos.x - origin.x) * scale + origin.x, (window->Pos.y - origin.y) * scale + origin.y));
    window->Size = ImFloor(ImVec2(window->Size.x * scale, window->Size.y * scale));
    window->SizeFull = ImFloor(ImVec2(window->SizeFull.x * scale, window->SizeFull.y * scale));
    window->ContentSize = ImFloor(ImVec2(window->ContentSize.x * scale, window->ContentSize.y * scale));
}

void ScaleWindowsInViewport(ImGuiViewport* viewport, float scale)
{
    ImGuiContext& g = *GImGui;

    for (int i = 0; i != g.Windows.Size; i++)
        if (g.Windows[i]->Viewport == viewport)
            ScaleWindow(g.Windows[i], scale);
}

// doesnt get used anymore really, as back when it was needed i had a simplified shader to test with, but now I just test with the actual imgui shader
void initTestShader()
{

    auto bd = getBackendData();
    bd->testShaderBinary = ImguiShaderCompiler::CompileShader("test");

    bd->testShaderBuffer = IM_NEW(MemoryBuffer)(bd->testShaderBinary.size, bd->testShaderBinary.ptr,
        nvn::MemoryPoolFlags::CPU_UNCACHED | nvn::MemoryPoolFlags::GPU_CACHED | nvn::MemoryPoolFlags::SHADER_CODE);

    EXL_ASSERT(bd->testShaderBuffer->IsBufferReady(), "Shader Buffer was not ready! unable to continue.");

    BinaryHeader offsetData = BinaryHeader((u32*)bd->testShaderBinary.ptr);

    nvn::BufferAddress addr = bd->testShaderBuffer->GetBufferAddress();

    nvn::ShaderData& vertShaderData = bd->testShaderDatas[0];
    vertShaderData.data = addr + offsetData.mVertexDataOffset;
    vertShaderData.control = bd->testShaderBinary.ptr + offsetData.mVertexControlOffset;

    nvn::ShaderData& fragShaderData = bd->testShaderDatas[1];
    fragShaderData.data = addr + offsetData.mFragmentDataOffset;
    fragShaderData.control = bd->testShaderBinary.ptr + offsetData.mFragmentControlOffset;

    EXL_ASSERT(bd->testShader.Initialize(bd->device), "Unable to Init Program!");
    EXL_ASSERT(bd->testShader.SetShaders(2, bd->testShaderDatas), "Unable to Set Shaders!");
}

// neat tool to cycle through all loaded textures in a texture pool
int texIDSelector()
{
    {

        static int curId = 256;
        static int downCounter = 0;
        static int upCounter = 0;

        if (InputHelper::isButtonPress(nn::hid::NpadButton::Left)) {
            curId--;
        } else if (InputHelper::isButtonHold(nn::hid::NpadButton::Left)) {

            downCounter++;
            if (downCounter > 30) {
                curId--;
            }
        } else {
            downCounter = 0;
        }

        if (InputHelper::isButtonPress(nn::hid::NpadButton::Right)) {
            curId++;
        } else if (InputHelper::isButtonHold(nn::hid::NpadButton::Right)) {

            upCounter++;
            if (upCounter > 30) {
                curId++;
            }
        } else {
            upCounter = 0;
        }

        /* fun values with bd->device->GetTextureHandle(curId, 256):
         * 282 = Window Texture
         * 393 = Some sort of render pass (shadow?) nvm it just seems to be the first occurrence of many more textures like it
         * 257 = debug font texture
         */

        return curId;
    }
}

// places ImDrawVerts, starting at startIndex, that use the x,y,width, and height values to define vertex coords
void createQuad(ImDrawVert* verts, int startIndex, int x, int y, int width, int height, ImU32 col)
{

    float minXVal = x;
    float maxXVal = x + width;
    float minYVal = y; // 400
    float maxYVal = y + height; // 400

    // top left
    ImDrawVert p1 = {
        .pos = ImVec2(minXVal, minYVal),
        .uv = ImVec2(0.0f, 0.0f),
        .col = col
    };
    // top right
    ImDrawVert p2 = {
        .pos = ImVec2(minXVal, maxYVal),
        .uv = ImVec2(0.0f, 1.0f),
        .col = col
    };
    // bottom left
    ImDrawVert p3 = {
        .pos = ImVec2(maxXVal, minYVal),
        .uv = ImVec2(1.0f, 0.0f),
        .col = col
    };
    // bottom right
    ImDrawVert p4 = {
        .pos = ImVec2(maxXVal, maxYVal),
        .uv = ImVec2(1.0f, 1.0f),
        .col = col
    };

    verts[startIndex] = p4;
    verts[startIndex + 1] = p2;
    verts[startIndex + 2] = p1;

    verts[startIndex + 3] = p1;
    verts[startIndex + 4] = p3;
    verts[startIndex + 5] = p4;
}

// this function is mainly what I used to debug the rendering of ImGui, so code is a bit messier
void renderTestShader(ImDrawData* drawData)
{

    auto bd = getBackendData();
    auto io = ImGui::GetIO();

    constexpr int triVertCount = 3;
    constexpr int quadVertCount = triVertCount * 2;

    int quadCount = 1; // modify to reflect how many quads need to be drawn per frame

    int pointCount = quadVertCount * quadCount;

    size_t totalVtxSize = pointCount * sizeof(ImDrawVert);
    if (!bd->vtxBuffer || bd->vtxBuffer->GetPoolSize() < totalVtxSize) {
        if (bd->vtxBuffer) {
            bd->vtxBuffer->Finalize();
            IM_FREE(bd->vtxBuffer);
        }
        bd->vtxBuffer = IM_NEW(MemoryBuffer)(totalVtxSize);
    }

    if (!bd->vtxBuffer->IsBufferReady()) {
        return;
    }

    ImDrawVert* verts = (ImDrawVert*)bd->vtxBuffer->GetMemPtr();

    float scale = 3.0f;

    float imageX = 1 * scale; // bd->fontTexture.GetWidth();
    float imageY = 1 * scale; // bd->fontTexture.GetHeight();

    createQuad(verts, 0, (io.DisplaySize.x / 2) - (imageX), (io.DisplaySize.y / 2) - (imageY), imageX, imageY,
        IM_COL32_WHITE);

    bd->cmdBuf->BeginRecording();
    bd->cmdBuf->BindProgram(&bd->shaderProgram, nvn::ShaderStageBits::VERTEX | nvn::ShaderStageBits::FRAGMENT);

    bd->cmdBuf->BindUniformBuffer(nvn::ShaderStage::VERTEX, 0, *bd->uniformMemory, UBOSIZE);
    bd->cmdBuf->UpdateUniformBuffer(*bd->uniformMemory, UBOSIZE, 0, sizeof(bd->mProjMatrix), &bd->mProjMatrix);

    bd->cmdBuf->BindVertexBuffer(0, (*bd->vtxBuffer), bd->vtxBuffer->GetPoolSize());

    setRenderStates();

    //        bd->cmdBuf->BindTexture(nvn::ShaderStage::FRAGMENT, 0, bd->fontTexHandle);

    bd->cmdBuf->DrawArrays(nvn::DrawPrimitive::TRIANGLES, 0, pointCount);

    auto handle = bd->cmdBuf->EndRecording();
    bd->queue->SubmitCommands(1, &handle);
}

// backend impl

NvnBackendData* getBackendData()
{
    NvnBackendData* result = ImGui::GetCurrentContext() ? (NvnBackendData*)ImGui::GetIO().BackendRendererUserData
                                                        : nullptr;
    EXL_ASSERT(result, "Backend has not been initialized!");
    return result;
}

bool createShaders()
{

    auto bd = getBackendData();

    if (ImguiShaderCompiler::CheckIsValidVersion(bd->device)) {

        ImguiShaderCompiler::InitializeCompiler();

        bd->imguiShaderBinary = ImguiShaderCompiler::CompileShader("imgui");

    } else {

        FsHelper::LoadData loadData = {
            .path = "content:/ImGuiData/imgui.bin"
        };

        FsHelper::loadFileFromPath(loadData);

        bd->imguiShaderBinary.size = loadData.bufSize;
        bd->imguiShaderBinary.ptr = (u8*)loadData.buffer;
    }

    if (bd->imguiShaderBinary.size > 0) {
        return true;
    }

    return false;
}

bool setupFont()
{

    auto bd = getBackendData();

    ImGuiIO& io = ImGui::GetIO();

    // init sampler and texture pools

    int sampDescSize = 0;
    bd->device->GetInteger(nvn::DeviceInfo::SAMPLER_DESCRIPTOR_SIZE, &sampDescSize);
    int texDescSize = 0;
    bd->device->GetInteger(nvn::DeviceInfo::TEXTURE_DESCRIPTOR_SIZE, &texDescSize);

    int sampMemPoolSize = sampDescSize * MaxSampDescriptors;
    int texMemPoolSize = texDescSize * MaxTexDescriptors;
    int totalPoolSize = ALIGN_UP(sampMemPoolSize + texMemPoolSize, 0x1000);
    if (!MemoryPoolMaker::createPool(&bd->sampTexMemPool, totalPoolSize)) {
        return false;
    }

    if (!bd->samplerPool.Initialize(&bd->sampTexMemPool, 0, MaxSampDescriptors)) {
        return false;
    }

    if (!bd->texPool.Initialize(&bd->sampTexMemPool, sampMemPoolSize, MaxTexDescriptors)) {
        return false;
    }

    // convert imgui font texels

    unsigned char* pixels;
    int width, height, pixelByteSize;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height, &pixelByteSize);
    int texPoolSize = pixelByteSize * width * height;

    if (!MemoryPoolMaker::createPool(&bd->fontMemPool, ALIGN_UP(texPoolSize, 0x1000),
            nvn::MemoryPoolFlags::CPU_UNCACHED | nvn::MemoryPoolFlags::GPU_CACHED)) {
        return false;
    }

    bd->texBuilder.SetDefaults()
        .SetDevice(bd->device)
        .SetTarget(nvn::TextureTarget::TARGET_2D)
        .SetFormat(nvn::Format::RGBA8)
        .SetSize2D(width, height)
        .SetStorage(&bd->fontMemPool, 0);

    if (!bd->fontTexture.Initialize(&bd->texBuilder)) {
        return false;
    }

    // setup font texture

    nvn::CopyRegion region = {
        .xoffset = 0,
        .yoffset = 0,
        .zoffset = 0,
        .width = bd->fontTexture.GetWidth(),
        .height = bd->fontTexture.GetHeight(),
        .depth = 1
    };

    bd->fontTexture.WriteTexels(nullptr, &region, pixels);
    bd->fontTexture.FlushTexels(nullptr, &region);

    bd->samplerBuilder.SetDefaults()
        .SetDevice(bd->device)
        .SetMinMagFilter(nvn::MinFilter::LINEAR, nvn::MagFilter::LINEAR)
        .SetWrapMode(nvn::WrapMode::CLAMP, nvn::WrapMode::CLAMP, nvn::WrapMode::CLAMP);

    if (!bd->fontSampler.Initialize(&bd->samplerBuilder)) {
        return false;
    }

    bd->textureId = 257;
    bd->samplerId = 257;

    bd->texPool.RegisterTexture(bd->textureId, &bd->fontTexture, nullptr);
    bd->samplerPool.RegisterSampler(bd->samplerId, &bd->fontSampler);

    bd->fontTexHandle = bd->device->GetTextureHandle(bd->textureId, bd->samplerId);
    io.Fonts->SetTexID(&bd->fontTexHandle);

    return true;
}

// idfk what im doing
/*
ImVector<nvn::TextureHandle> sTextureHandles;
int sTextureIdCounter = 5954530;

ImTextureID loadTextureRGBA32(const ImU32* texture, int width, int height)
{
    auto bd = getBackendData();

    // get texture size
    int texSize = width * height * sizeof(ImU32);
    nvn::MemoryPool memPool;
    if (!MemoryPoolMaker::createPool(&memPool, ALIGN_UP(texSize, 0x1000),
            nvn::MemoryPoolFlags::CPU_UNCACHED | nvn::MemoryPoolFlags::GPU_CACHED)) {
        return nullptr;
    }

    bd->texBuilder.SetDefaults()
        .SetDevice(bd->device)
        .SetTarget(nvn::TextureTarget::TARGET_2D)
        .SetFormat(nvn::Format::RGBA8)
        .SetSize2D(width, height)
        .SetStorage(&memPool, 0);

    // initialize texture
    nvn::Texture nvnTexture;
    if (!nvnTexture.Initialize(&bd->texBuilder)) {
        return 0;
    }

    // copy pixel data
    nvn::CopyRegion region = {
        .xoffset = 0,
        .yoffset = 0,
        .width = width,
        .height = height,
        .depth = 1
    };
    nvnTexture.WriteTexels(nullptr, &region, texture);
    nvnTexture.FlushTexels(nullptr, &region);

    // setup sampler
    bd->samplerBuilder.SetDefaults()
        .SetDevice(bd->device)
        .SetMinMagFilter(nvn::MinFilter::LINEAR, nvn::MagFilter::LINEAR)
        .SetWrapMode(nvn::WrapMode::CLAMP, nvn::WrapMode::CLAMP, nvn::WrapMode::CLAMP);

    nvn::Sampler sampler;
    if (!sampler.Initialize(&bd->samplerBuilder)) {
        return 0;
    }

    // register with pools
    int texId = sTextureIdCounter;
    int sampId = sTextureIdCounter;
    bd->texPool.RegisterTexture(texId, &nvnTexture, nullptr);
    bd->samplerPool.RegisterSampler(sampId, &sampler);

    sTextureIdCounter++;
    sTextureHandles.push_back(bd->device->GetTextureHandle(texId, sampId));
    return &sTextureHandles.Data[sTextureHandles.size() - 1];
}
*/

bool setupShaders(u8* shaderBinary, ulong binarySize)
{

    auto bd = getBackendData();

    if (!bd->shaderProgram.Initialize(bd->device)) {
        return false;
    }

    bd->shaderMemory = IM_NEW(MemoryBuffer)(binarySize, shaderBinary, nvn::MemoryPoolFlags::CPU_UNCACHED | nvn::MemoryPoolFlags::GPU_CACHED | nvn::MemoryPoolFlags::SHADER_CODE);

    if (!bd->shaderMemory->IsBufferReady()) {
        return false;
    }

    BinaryHeader offsetData = BinaryHeader((u32*)shaderBinary);

    nvn::BufferAddress addr = bd->shaderMemory->GetBufferAddress();

    nvn::ShaderData& vertShaderData = bd->shaderDatas[0];
    vertShaderData.data = addr + offsetData.mVertexDataOffset;
    vertShaderData.control = shaderBinary + offsetData.mVertexControlOffset;

    nvn::ShaderData& fragShaderData = bd->shaderDatas[1];
    fragShaderData.data = addr + offsetData.mFragmentDataOffset;
    fragShaderData.control = shaderBinary + offsetData.mFragmentControlOffset;

    if (!bd->shaderProgram.SetShaders(2, bd->shaderDatas)) {
        return false;
    }

    bd->shaderProgram.SetDebugLabel("ImGuiShader");

    // Uniform Block Object Memory Setup

    bd->uniformMemory = IM_NEW(MemoryBuffer)(UBOSIZE);

    if (!bd->uniformMemory->IsBufferReady()) {
        return false;
    }

    // setup vertex attrib & stream

    bd->attribStates[0].SetDefaults().SetFormat(nvn::Format::RG32F, offsetof(ImDrawVert, pos)); // pos
    bd->attribStates[1].SetDefaults().SetFormat(nvn::Format::RG32F, offsetof(ImDrawVert, uv)); // uv
    bd->attribStates[2].SetDefaults().SetFormat(nvn::Format::RGBA8, offsetof(ImDrawVert, col)); // color

    bd->streamState.SetDefaults().SetStride(sizeof(ImDrawVert));

    return true;
}

void InitBackend(const NvnBackendInitInfo& initInfo)
{
    ImGuiIO& io = ImGui::GetIO();
    EXL_ASSERT(!io.BackendRendererUserData, "Already Initialized Imgui Backend!");

    io.BackendPlatformName = "Switch";
    io.BackendRendererName = "imgui_impl_nvn";
    io.IniFilename = nullptr;
    io.MouseDrawCursor = false;
    io.ConfigFlags |= ImGuiConfigFlags_IsTouchScreen;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.DisplaySize = ImVec2(1920, 1080); // default size

    auto* bd = IM_NEW(NvnBackendData)();
    io.BackendRendererUserData = (void*)bd;

    bd->device = initInfo.device;
    bd->queue = initInfo.queue;
    bd->cmdBuf = initInfo.cmdBuf;
    bd->isInitialized = false;

    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    FsHelper::LoadData loadData = {
        .path = "content:/ImGuiData/Fonts/RodinNTLG-Bold.otf"
    };

    FsHelper::loadFileFromPath(loadData);

    ImVector<ImWchar> ranges;
    ImFontGlyphRangesBuilder builder;
    builder.AddText(
        " "
        "abcdefghikjlmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789"
        "あいうえおかきくけこさしすせそたちつてとなにぬねのはひふへほまみむめもやゆよらりるれろわをんアイウエオカキクケコサシスセソタチツテトナニヌネノハヒフヘホマミムメモヤユヨラリルレロワヲンぁぃぅぇぉっゃゅょァィゥェォッャュョーがぎぐげござじずぜぞだぢづでどばびぶべぼぱぴぷぺぽガギグゲゴザジズゼゾダヂヅデドバビブベボパピプペポ"
        "日本語詳細設定配位置無有効化十字乱数次前行常時入力表示触瞬間認識切替"
        "上下左右"
        "！、"
        "äüöß"
        "!\"§$%&/()=?´`^°#+-·.,;:_'*\\}][{");
    builder.BuildRanges(&ranges);

    io.Fonts->AddFontFromMemoryTTF(loadData.buffer, loadData.bufSize, 50, nullptr, ranges.Data);
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    ImGui::StyleColorsDark();

    style.FrameBorderSize = 0;
    style.WindowBorderSize = 0;
    style.WindowRounding = 5;
    style.ScrollbarRounding = 1;
    style.TabRounding = 4;
    style.PopupRounding = 3;
    style.FrameRounding = 3;
    style.ChildRounding = 3;

    colors[ImGuiCol_Text] = ImVec4(1, 1, 1, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.35f, 0.35f, 0.35f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.97f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.08f, 0.08f, 0.94f);
    colors[ImGuiCol_Border] = ImVec4(0.00f, 0.00f, 0.00f, 0.50f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.04f, 0.04f, 0.04f, 0.54f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.37f, 0.14f, 0.14f, 0.67f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.39f, 0.20f, 0.20f, 0.67f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.04f, 0.04f, 0.04f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.48f, 0.16f, 0.16f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.48f, 0.16f, 0.16f, 1.00f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.02f, 0.02f, 0.02f, 0.53f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.31f, 0.31f, 0.31f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.41f, 0.41f, 0.41f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.51f, 0.51f, 0.51f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.56f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(1.00f, 0.19f, 0.19f, 0.40f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.89f, 0.00f, 0.19f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(1.00f, 0.19f, 0.19f, 0.40f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.80f, 0.17f, 0.00f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.89f, 0.00f, 0.19f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.33f, 0.35f, 0.36f, 0.53f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.76f, 0.28f, 0.44f, 0.67f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.47f, 0.47f, 0.47f, 0.67f);
    colors[ImGuiCol_Separator] = ImVec4(0.32f, 0.32f, 0.32f, 1.00f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.32f, 0.32f, 0.32f, 1.00f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(0.32f, 0.32f, 0.32f, 1.00f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(1.00f, 1.00f, 1.00f, 0.85f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(1.00f, 1.00f, 1.00f, 0.60f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(1.00f, 1.00f, 1.00f, 0.90f);
    colors[ImGuiCol_Tab] = ImVec4(0.07f, 0.07f, 0.07f, 0.51f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.86f, 0.23f, 0.43f, 0.67f);
    colors[ImGuiCol_TabActive] = ImVec4(0.19f, 0.19f, 0.19f, 0.57f);
    colors[ImGuiCol_TabUnfocused] = ImVec4(0.05f, 0.05f, 0.05f, 0.90f);
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.13f, 0.13f, 0.13f, 0.74f);
    colors[ImGuiCol_DockingPreview] = ImVec4(0.47f, 0.47f, 0.47f, 0.47f);
    colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_PlotLines] = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
    colors[ImGuiCol_PlotHistogram] = ImVec4(1, 0, 0.00f, 0.32);
    colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1, 0, 0.00f, 0.7);
    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.19f, 0.19f, 0.20f, 1.00f);
    colors[ImGuiCol_TableBorderStrong] = ImVec4(0.31f, 0.31f, 0.35f, 1.00f);
    colors[ImGuiCol_TableBorderLight] = ImVec4(0.23f, 0.23f, 0.25f, 1.00f);
    colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.07f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.26f, 0.59f, 0.98f, 0.35f);
    colors[ImGuiCol_DragDropTarget] = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
    colors[ImGuiCol_NavHighlight] = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);

    if (createShaders()) {

        if (bd->isUseTestShader)
            initTestShader();

        if (setupShaders(bd->imguiShaderBinary.ptr, bd->imguiShaderBinary.size) && setupFont()) {

            bd->isInitialized = true;
        }
    }
}

void ShutdownBackend()
{
}

void updateMouse(ImGuiIO& io)
{
    ImVec2 mousePos(0, 0);
    InputHelper::getMouseCoords(&mousePos.x, &mousePos.y);
    mousePos.x *= 1.5;
    mousePos.y *= 1.5;
    io.AddMousePosEvent(mousePos.x, mousePos.y);

    ImVec2 scrollDelta(0, 0);
    InputHelper::getScrollDelta(&scrollDelta.x, &scrollDelta.y);

    if (scrollDelta.x != 0.0f)
        io.AddMouseWheelEvent(0.0f, scrollDelta.x > 0.0f ? 0.5f : -0.5f);

    for (auto [im_k, nx_k] : mouse_mapping) {
        if (InputHelper::isMousePress((nn::hid::MouseButton)nx_k))
            io.AddMouseButtonEvent((ImGuiMouseButton)im_k, true);
        else if (InputHelper::isMouseRelease((nn::hid::MouseButton)nx_k))
            io.AddMouseButtonEvent((ImGuiMouseButton)im_k, false);
    }
}

void updateKeyboard(ImGuiIO& io)
{
    for (auto [im_k, nx_k] : key_mapping) {
        if (InputHelper::isKeyPress((nn::hid::KeyboardKey)nx_k)) {
            io.AddKeyEvent((ImGuiKey)im_k, true);
        } else if (InputHelper::isKeyRelease((nn::hid::KeyboardKey)nx_k)) {
            io.AddKeyEvent((ImGuiKey)im_k, false);
        }
    }
}

void updateGamepad(ImGuiIO& io)
{
    for (auto [im_k, nx_k] : npad_mapping) {
        if (InputHelper::isButtonPress((nn::hid::NpadButton)nx_k))
            io.AddKeyEvent((ImGuiKey)im_k, true);
        else if (InputHelper::isButtonRelease((nn::hid::NpadButton)nx_k))
            io.AddKeyEvent((ImGuiKey)im_k, false);
    }
}

void updateInput()
{

    ImGuiIO& io = ImGui::GetIO();
    updateKeyboard(io);
    updateMouse(io);

    if (InputHelper::isInputToggled()) {
        updateGamepad(io);
    }
}

void updateProjection(ImVec2 dispSize)
{
    orthoRH_ZO(getBackendData()->mProjMatrix, 0.0f, dispSize.x, dispSize.y, 0.0f, -1.0f, 1.0f);
}

void updateScale(bool isDocked)
{
    static float prevScale = 0.0f;

    float scale = isDocked ? 1.5f : 1.f;

    ImGuiStyle& stylePtr = ImGui::GetStyle();
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGuiIO& io = ImGui::GetIO();

    ImVec4 prevColors[ImGuiCol_COUNT] = {};
    memcpy(&prevColors, &stylePtr.Colors, sizeof(stylePtr.Colors));

    // reset style
    stylePtr = ImGuiStyle();
    // set colors back to previous
    memcpy(&stylePtr.Colors, &prevColors, sizeof(stylePtr.Colors));
    // scale style
    ImGui::GetStyle().ScaleAllSizes(scale);
    // reset scale of windows
    if (prevScale != 0.0f) {
        ScaleWindowsInViewport(viewport, 1.f / prevScale);
    }

    // scale window
    ScaleWindowsInViewport(viewport, scale);
    prevScale = scale;
    // set font scale
    io.FontGlobalScale = scale;
}

void newFrame()
{
    ImGuiIO& io = ImGui::GetIO();
    auto* bd = getBackendData();

    nn::TimeSpan curTick = nn::os::GetSystemTick().ToTimeSpan();
    nn::TimeSpan prevTick(bd->lastTick);
    io.DeltaTime = fabsf((float)(curTick - prevTick).GetNanoSeconds() / 1e9f);

    bd->lastTick = curTick;

    InputHelper::updatePadState(); // update input helper

    updateInput(); // update backend inputs
}

void setRenderStates()
{

    auto bd = getBackendData();

    nvn::PolygonState polyState;
    polyState.SetDefaults();
    polyState.SetPolygonMode(nvn::PolygonMode::FILL);
    polyState.SetCullFace(nvn::Face::NONE);
    polyState.SetFrontFace(nvn::FrontFace::CCW);
    bd->cmdBuf->BindPolygonState(&polyState);

    nvn::ColorState colorState;
    colorState.SetDefaults();
    colorState.SetLogicOp(nvn::LogicOp::COPY);
    colorState.SetAlphaTest(nvn::AlphaFunc::ALWAYS);
    for (int i = 0; i < 8; ++i) {
        colorState.SetBlendEnable(i, true);
    }
    bd->cmdBuf->BindColorState(&colorState);

    nvn::BlendState blendState;
    blendState.SetDefaults();
    blendState.SetBlendFunc(nvn::BlendFunc::SRC_ALPHA, nvn::BlendFunc::ONE_MINUS_SRC_ALPHA, nvn::BlendFunc::ONE,
        nvn::BlendFunc::ZERO);
    blendState.SetBlendEquation(nvn::BlendEquation::ADD, nvn::BlendEquation::ADD);
    bd->cmdBuf->BindBlendState(&blendState);

    bd->cmdBuf->BindVertexAttribState(3, bd->attribStates);
    bd->cmdBuf->BindVertexStreamState(1, &bd->streamState);

    bd->cmdBuf->SetTexturePool(&bd->texPool);
    bd->cmdBuf->SetSamplerPool(&bd->samplerPool);
}

void renderDrawData(ImDrawData* drawData)
{

    // we dont need to process any data if it isnt valid
    if (!drawData->Valid) {
        return;
    }
    // if we dont have any command lists to draw, we can stop here
    if (drawData->CmdListsCount == 0) {
        return;
    }

    // get both the main backend data and IO from ImGui
    auto bd = getBackendData();
    ImGuiIO& io = ImGui::GetIO();

    // if something went wrong during backend setup, don't try to render anything
    if (!bd->isInitialized) {
        return;
    }

    // disable imgui rendering if we are using the test shader code
    if (bd->isUseTestShader) {
        renderTestShader(drawData);
        return;
    }

    // initializes/resizes buffer used for all vertex data created by ImGui
    size_t totalVtxSize = drawData->TotalVtxCount * sizeof(ImDrawVert);
    if (!bd->vtxBuffer || bd->vtxBuffer->GetPoolSize() < totalVtxSize) {
        if (bd->vtxBuffer) {
            bd->vtxBuffer->Finalize();
            IM_FREE(bd->vtxBuffer);
        }

        bd->vtxBuffer = IM_NEW(MemoryBuffer)(totalVtxSize);
    }

    // initializes/resizes buffer used for all index data created by ImGui
    size_t totalIdxSize = drawData->TotalIdxCount * sizeof(ImDrawIdx);
    if (!bd->idxBuffer || bd->idxBuffer->GetPoolSize() < totalIdxSize) {
        if (bd->idxBuffer) {

            bd->idxBuffer->Finalize();
            IM_FREE(bd->idxBuffer);
        }

        bd->idxBuffer = IM_NEW(MemoryBuffer)(totalIdxSize);
    }

    // if we fail to resize/init either buffers, end execution before we try to use said invalid buffer(s)
    if (!(bd->vtxBuffer->IsBufferReady() && bd->idxBuffer->IsBufferReady())) {
        return;
    }

    bd->cmdBuf->BeginRecording(); // start recording our commands to the cmd buffer

    bd->cmdBuf->BindProgram(&bd->shaderProgram, nvn::ShaderStageBits::VERTEX | nvn::ShaderStageBits::FRAGMENT); // bind main imgui shader

    bd->cmdBuf->BindUniformBuffer(nvn::ShaderStage::VERTEX, 0, *bd->uniformMemory,
        UBOSIZE); // bind uniform block ptr
    bd->cmdBuf->UpdateUniformBuffer(*bd->uniformMemory, UBOSIZE, 0, sizeof(bd->mProjMatrix),
        &bd->mProjMatrix); // add projection matrix data to uniform data

    setRenderStates(); // sets up the rest of the render state, required so that our shader properly gets drawn to the screen

    size_t vtxOffset = 0, idxOffset = 0;
    nvn::TextureHandle boundTextureHandle = 0;

    // load data into buffers, and process draw commands
    for (size_t i = 0; i < drawData->CmdListsCount; i++) {

        auto cmdList = drawData->CmdLists[i];

        // calc vertex and index buffer sizes
        size_t vtxSize = cmdList->VtxBuffer.Size * sizeof(ImDrawVert);
        size_t idxSize = cmdList->IdxBuffer.Size * sizeof(ImDrawIdx);

        // bind vtx buffer at the current offset
        bd->cmdBuf->BindVertexBuffer(0, (*bd->vtxBuffer) + vtxOffset, vtxSize);

        // copy data from imgui command list into our gpu dedicated memory
        memcpy(bd->vtxBuffer->GetMemPtr() + vtxOffset, cmdList->VtxBuffer.Data, vtxSize);
        memcpy(bd->idxBuffer->GetMemPtr() + idxOffset, cmdList->IdxBuffer.Data, idxSize);

        for (auto cmd : cmdList->CmdBuffer) {

            // im not exactly sure this scaling is a good solution,
            // for some reason imgui clipping coords are relative to 720p instead of whatever I set for disp size.
            // TRIPPIEDIT I really dunno if this is good
            ImVec2 origRes(1600.0f, 900.0f); // used to be (1280.0f, 720.0f);
            ImVec2 newRes = io.DisplaySize; // (1600.0f, 900.0f);

            ImVec4 clipRect = ImVec4((cmd.ClipRect.x / origRes.x) * newRes.x,
                (cmd.ClipRect.y / origRes.y) * newRes.y,
                (cmd.ClipRect.z / origRes.x) * newRes.x,
                (cmd.ClipRect.w / origRes.y) * newRes.y);

            ImVec2 clip_min(clipRect.x, clipRect.y);
            ImVec2 clip_max(clipRect.z, clipRect.w);
            ImVec2 clip_size(clip_max.x - clip_min.x, clip_max.y - clip_min.y);

            if (clip_max.x <= clip_min.x || clip_max.y <= clip_min.y)
                continue;

            bd->cmdBuf->SetScissor((int)clip_min.x, (int)clip_min.y,
                (int)clip_size.x, (int)clip_size.y);

            // get texture ID from the command
            nvn::TextureHandle TexID = *(nvn::TextureHandle*)cmd.GetTexID();
            // if our previous handle is different from the current, bind the texture
            if (boundTextureHandle != TexID) {
                boundTextureHandle = TexID;
                bd->cmdBuf->BindTexture(nvn::ShaderStage::FRAGMENT, 0, TexID);
            }
            // draw our vertices using the indices stored in the buffer, offset by the current command index offset,
            // as well as the current offset into our buffer.
            bd->cmdBuf->DrawElementsBaseVertex(nvn::DrawPrimitive::TRIANGLES,
                nvn::IndexType::UNSIGNED_SHORT, cmd.ElemCount,
                (*bd->idxBuffer) + (cmd.IdxOffset * sizeof(ImDrawIdx)) + idxOffset,
                cmd.VtxOffset);
        }

        vtxOffset += vtxSize;
        idxOffset += idxSize;
    }

    // end the command recording and submit to queue.
    auto handle = bd->cmdBuf->EndRecording();
    bd->queue->SubmitCommands(1, &handle);
}

}