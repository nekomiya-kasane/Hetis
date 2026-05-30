// 01-TaskShader: Renders a rotating colored cube using the Task/Mesh shader pipeline.
// Uses volk (function loader), vk-bootstrap (instance/device setup), GLFW (window), GLM (math).
// Shaders compiled from Slang -> SPIR-V at build time via slangc.

#include <volk.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <VkBootstrap.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <spdlog/spdlog.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

// ─── Helpers ─────────────────────────────────────────────────────────────────

#define VK_CHECK(expr)                                                         \
    do {                                                                        \
        VkResult _r = (expr);                                                  \
        if (_r != VK_SUCCESS) {                                                \
            spdlog::critical("VkResult {} at {}:{}", (int)_r, __FILE__, __LINE__); \
            std::abort();                                                       \
        }                                                                      \
    } while (0)

static std::vector<uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("Failed to open: " + path.string());
    auto size = f.tellg();
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()), size);
    return buf;
}

static VkShaderModule createShaderModule(VkDevice device, const std::vector<uint8_t>& code) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule mod{};
    VK_CHECK(vkCreateShaderModule(device, &ci, nullptr, &mod));
    return mod;
}

// ─── Constants ───────────────────────────────────────────────────────────────

constexpr uint32_t WIDTH = 1280;
constexpr uint32_t HEIGHT = 720;
constexpr int MAX_FRAMES_IN_FLIGHT = 2;

// ─── Application ─────────────────────────────────────────────────────────────

struct App {
    GLFWwindow* window = nullptr;
    VkInstance instance{};
    VkDebugUtilsMessengerEXT messenger{};
    VkSurfaceKHR surface{};
    VkPhysicalDevice gpu{};
    VkDevice device{};
    uint32_t graphicsQueueFamily = 0;
    VkQueue graphicsQueue{};
    VkSwapchainKHR swapchain{};
    VkFormat swapchainFormat{};
    VkExtent2D swapchainExtent{};
    std::vector<VkImage> swapImages;
    std::vector<VkImageView> swapViews;
    VkRenderPass renderPass{};
    std::vector<VkFramebuffer> framebuffers;
    VkPipelineLayout pipelineLayout{};
    VkPipeline pipeline{};
    VkCommandPool commandPool{};
    std::array<VkCommandBuffer, MAX_FRAMES_IN_FLIGHT> cmdBufs{};
    std::array<VkSemaphore, MAX_FRAMES_IN_FLIGHT> imageAvailable{};
    std::array<VkSemaphore, MAX_FRAMES_IN_FLIGHT> renderFinished{};
    std::array<VkFence, MAX_FRAMES_IN_FLIGHT> inFlight{};
    uint32_t frameIndex = 0;

    // Depth buffer
    VkImage depthImage{};
    VkDeviceMemory depthMemory{};
    VkImageView depthView{};
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;

    void init();
    void createDepthResources();
    void createRenderPass();
    void createFramebuffers();
    void createPipeline();
    void createSyncObjects();
    void mainLoop();
    void drawFrame();
    void cleanup();
};

void App::init() {
    // GLFW
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    window = glfwCreateWindow(WIDTH, HEIGHT, "01-TaskShader Cube", nullptr, nullptr);

    // Volk
    VK_CHECK(volkInitialize());

    // Instance (via vk-bootstrap)
    vkb::InstanceBuilder instBuilder;
    auto instRet = instBuilder.set_app_name("TaskShaderCube")
                       .require_api_version(1, 3, 0)
                       .use_default_debug_messenger()
                       .build();
    if (!instRet) {
        spdlog::critical("Instance creation failed: {}", instRet.error().message());
        std::abort();
    }
    auto vkbInst = instRet.value();
    instance = vkbInst.instance;
    messenger = vkbInst.debug_messenger;
    volkLoadInstance(instance);

    // Surface
    VK_CHECK(glfwCreateWindowSurface(instance, window, nullptr, &surface));

    // Physical device — require mesh shader extension
    vkb::PhysicalDeviceSelector selector(vkbInst);
    auto physRet = selector.set_surface(surface)
                       .set_minimum_version(1, 3)
                       .add_required_extension(VK_EXT_MESH_SHADER_EXTENSION_NAME)
                       .select();
    if (!physRet) {
        spdlog::critical("No suitable GPU: {}", physRet.error().message());
        std::abort();
    }
    auto vkbPhys = physRet.value();
    gpu = vkbPhys.physical_device;

    // Enable mesh shader features
    VkPhysicalDeviceMeshShaderFeaturesEXT meshFeatures{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    meshFeatures.taskShader = VK_TRUE;
    meshFeatures.meshShader = VK_TRUE;

    // Logical device
    vkb::DeviceBuilder devBuilder(vkbPhys);
    auto devRet = devBuilder.add_pNext(&meshFeatures).build();
    if (!devRet) {
        spdlog::critical("Device creation failed: {}", devRet.error().message());
        std::abort();
    }
    auto vkbDev = devRet.value();
    device = vkbDev.device;
    volkLoadDevice(device);

    auto qRet = vkbDev.get_queue(vkb::QueueType::graphics);
    graphicsQueue = qRet.value();
    graphicsQueueFamily = vkbDev.get_queue_index(vkb::QueueType::graphics).value();

    // Swapchain
    vkb::SwapchainBuilder swapBuilder(vkbDev);
    auto swapRet = swapBuilder.set_desired_format({VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
                       .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
                       .set_desired_extent(WIDTH, HEIGHT)
                       .build();
    if (!swapRet) {
        spdlog::critical("Swapchain creation failed: {}", swapRet.error().message());
        std::abort();
    }
    auto vkbSwap = swapRet.value();
    swapchain = vkbSwap.swapchain;
    swapchainFormat = vkbSwap.image_format;
    swapchainExtent = vkbSwap.extent;
    swapImages = vkbSwap.get_images().value();
    swapViews = vkbSwap.get_image_views().value();

    // Command pool
    VkCommandPoolCreateInfo poolCI{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolCI.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolCI.queueFamilyIndex = graphicsQueueFamily;
    VK_CHECK(vkCreateCommandPool(device, &poolCI, nullptr, &commandPool));

    VkCommandBufferAllocateInfo allocCI{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocCI.commandPool = commandPool;
    allocCI.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocCI.commandBufferCount = MAX_FRAMES_IN_FLIGHT;
    VK_CHECK(vkAllocateCommandBuffers(device, &allocCI, cmdBufs.data()));

    createDepthResources();
    createRenderPass();
    createFramebuffers();
    createPipeline();
    createSyncObjects();
}

void App::createDepthResources() {
    VkImageCreateInfo imgCI{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imgCI.imageType = VK_IMAGE_TYPE_2D;
    imgCI.format = depthFormat;
    imgCI.extent = {swapchainExtent.width, swapchainExtent.height, 1};
    imgCI.mipLevels = 1;
    imgCI.arrayLayers = 1;
    imgCI.samples = VK_SAMPLE_COUNT_1_BIT;
    imgCI.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgCI.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VK_CHECK(vkCreateImage(device, &imgCI, nullptr, &depthImage));

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(device, depthImage, &memReq);

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(gpu, &memProps);

    uint32_t memIdx = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReq.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memIdx = i;
            break;
        }
    }
    assert(memIdx != UINT32_MAX);

    VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = memIdx;
    VK_CHECK(vkAllocateMemory(device, &allocInfo, nullptr, &depthMemory));
    VK_CHECK(vkBindImageMemory(device, depthImage, depthMemory, 0));

    VkImageViewCreateInfo viewCI{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewCI.image = depthImage;
    viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewCI.format = depthFormat;
    viewCI.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(device, &viewCI, nullptr, &depthView));
}

void App::createRenderPass() {
    std::array<VkAttachmentDescription, 2> attachments{};
    // Color
    attachments[0].format = swapchainFormat;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    // Depth
    attachments[1].format = depthFormat;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpCI{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpCI.attachmentCount = static_cast<uint32_t>(attachments.size());
    rpCI.pAttachments = attachments.data();
    rpCI.subpassCount = 1;
    rpCI.pSubpasses = &subpass;
    rpCI.dependencyCount = 1;
    rpCI.pDependencies = &dep;
    VK_CHECK(vkCreateRenderPass(device, &rpCI, nullptr, &renderPass));
}

void App::createFramebuffers() {
    framebuffers.resize(swapViews.size());
    for (size_t i = 0; i < swapViews.size(); ++i) {
        std::array<VkImageView, 2> views = {swapViews[i], depthView};
        VkFramebufferCreateInfo fbCI{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbCI.renderPass = renderPass;
        fbCI.attachmentCount = static_cast<uint32_t>(views.size());
        fbCI.pAttachments = views.data();
        fbCI.width = swapchainExtent.width;
        fbCI.height = swapchainExtent.height;
        fbCI.layers = 1;
        VK_CHECK(vkCreateFramebuffer(device, &fbCI, nullptr, &framebuffers[i]));
    }
}

void App::createPipeline() {
    namespace fs = std::filesystem;
    fs::path shaderDir(SHADER_DIR);

    auto taskCode = readFile(shaderDir / "cube.task.spv");
    auto meshCode = readFile(shaderDir / "cube.mesh.spv");
    auto fragCode = readFile(shaderDir / "cube.frag.spv");

    VkShaderModule taskModule = createShaderModule(device, taskCode);
    VkShaderModule meshModule = createShaderModule(device, meshCode);
    VkShaderModule fragModule = createShaderModule(device, fragCode);

    std::array<VkPipelineShaderStageCreateInfo, 3> stages{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_TASK_BIT_EXT;
    stages[0].module = taskModule;
    stages[0].pName = "taskMain";

    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_MESH_BIT_EXT;
    stages[1].module = meshModule;
    stages[1].pName = "meshMain";

    stages[2] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[2].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[2].module = fragModule;
    stages[2].pName = "fragMain";

    // Push constant: MVP matrix (64 bytes)
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_MESH_BIT_EXT;
    pushRange.offset = 0;
    pushRange.size = sizeof(glm::mat4);

    VkPipelineLayoutCreateInfo layoutCI{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(device, &layoutCI, nullptr, &pipelineLayout));

    // Viewport / scissor (dynamic)
    VkPipelineViewportStateCreateInfo vpState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vpState.viewportCount = 1;
    vpState.scissorCount = 1;

    std::array<VkDynamicState, 2> dynStates = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynState{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynState.dynamicStateCount = static_cast<uint32_t>(dynStates.size());
    dynState.pDynamicStates = dynStates.data();

    // Rasterization
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_BACK_BIT;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    // Multisampling (off)
    VkPipelineMultisampleStateCreateInfo msaa{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth-stencil
    VkPipelineDepthStencilStateCreateInfo depthStencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    // Color blend
    VkPipelineColorBlendAttachmentState blendAtt{};
    blendAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAtt;

    // Graphics pipeline (mesh shader — no vertex input stage)
    VkGraphicsPipelineCreateInfo pipeCI{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeCI.stageCount = static_cast<uint32_t>(stages.size());
    pipeCI.pStages = stages.data();
    pipeCI.pViewportState = &vpState;
    pipeCI.pRasterizationState = &raster;
    pipeCI.pMultisampleState = &msaa;
    pipeCI.pDepthStencilState = &depthStencil;
    pipeCI.pColorBlendState = &blend;
    pipeCI.pDynamicState = &dynState;
    pipeCI.layout = pipelineLayout;
    pipeCI.renderPass = renderPass;
    pipeCI.subpass = 0;
    // No pVertexInputState or pInputAssemblyState needed for mesh shaders

    VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &pipeline));

    vkDestroyShaderModule(device, taskModule, nullptr);
    vkDestroyShaderModule(device, meshModule, nullptr);
    vkDestroyShaderModule(device, fragModule, nullptr);
}

void App::createSyncObjects() {
    VkSemaphoreCreateInfo semCI{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fenceCI{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceCI.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VK_CHECK(vkCreateSemaphore(device, &semCI, nullptr, &imageAvailable[i]));
        VK_CHECK(vkCreateSemaphore(device, &semCI, nullptr, &renderFinished[i]));
        VK_CHECK(vkCreateFence(device, &fenceCI, nullptr, &inFlight[i]));
    }
}

void App::mainLoop() {
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        drawFrame();
    }
    vkDeviceWaitIdle(device);
}

void App::drawFrame() {
    uint32_t fi = frameIndex;
    vkWaitForFences(device, 1, &inFlight[fi], VK_TRUE, UINT64_MAX);
    vkResetFences(device, 1, &inFlight[fi]);

    uint32_t imgIdx;
    vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, imageAvailable[fi], VK_NULL_HANDLE, &imgIdx);

    VkCommandBuffer cmd = cmdBufs[fi];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &beginInfo));

    // Begin render pass
    std::array<VkClearValue, 2> clears{};
    clears[0].color = {{0.1f, 0.1f, 0.12f, 1.0f}};
    clears[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rpBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpBegin.renderPass = renderPass;
    rpBegin.framebuffer = framebuffers[imgIdx];
    rpBegin.renderArea = {{0, 0}, swapchainExtent};
    rpBegin.clearValueCount = static_cast<uint32_t>(clears.size());
    rpBegin.pClearValues = clears.data();
    vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    // Dynamic viewport & scissor
    VkViewport vp{0, 0, (float)swapchainExtent.width, (float)swapchainExtent.height, 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D scissor{{0, 0}, swapchainExtent};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Compute MVP
    static auto startTime = glfwGetTime();
    float time = static_cast<float>(glfwGetTime() - startTime);

    float aspect = (float)swapchainExtent.width / (float)swapchainExtent.height;

    // GLM_FORCE_LEFT_HANDED + GLM_FORCE_DEPTH_ZERO_TO_ONE matches Vulkan conventions
    glm::mat4 model = glm::rotate(glm::mat4(1.0f), time * glm::radians(45.0f), glm::vec3(0, 1, 0));
    model = glm::rotate(model, time * glm::radians(30.0f), glm::vec3(1, 0, 0));

    glm::mat4 view = glm::lookAt(
        glm::vec3(3.0f, 2.5f, 3.0f),  // eye
        glm::vec3(0.0f, 0.0f, 0.0f),  // center
        glm::vec3(0.0f, 1.0f, 0.0f)   // up
    );

    glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 100.0f);

    glm::mat4 mvp = proj * view * model;

    vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_MESH_BIT_EXT, 0, sizeof(mvp), &mvp);

    // Dispatch task shader: 1 workgroup → task shader emits 6 mesh workgroups
    vkCmdDrawMeshTasksEXT(cmd, 1, 1, 1);

    vkCmdEndRenderPass(cmd);
    VK_CHECK(vkEndCommandBuffer(cmd));

    // Submit
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &imageAvailable[fi];
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &renderFinished[fi];
    VK_CHECK(vkQueueSubmit(graphicsQueue, 1, &submit, inFlight[fi]));

    // Present
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderFinished[fi];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &imgIdx;
    vkQueuePresentKHR(graphicsQueue, &present);

    frameIndex = (frameIndex + 1) % MAX_FRAMES_IN_FLIGHT;
}

void App::cleanup() {
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        vkDestroySemaphore(device, imageAvailable[i], nullptr);
        vkDestroySemaphore(device, renderFinished[i], nullptr);
        vkDestroyFence(device, inFlight[i], nullptr);
    }
    vkDestroyCommandPool(device, commandPool, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    for (auto fb : framebuffers) vkDestroyFramebuffer(device, fb, nullptr);
    vkDestroyRenderPass(device, renderPass, nullptr);
    vkDestroyImageView(device, depthView, nullptr);
    vkFreeMemory(device, depthMemory, nullptr);
    vkDestroyImage(device, depthImage, nullptr);
    for (auto iv : swapViews) vkDestroyImageView(device, iv, nullptr);
    vkDestroySwapchainKHR(device, swapchain, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyDebugUtilsMessengerEXT(instance, messenger, nullptr);
    vkDestroyInstance(instance, nullptr);
    glfwDestroyWindow(window);
    glfwTerminate();
}

// ─── Entry point ─────────────────────────────────────────────────────────────

int main() {
    App app;
    try {
        app.init();
        app.mainLoop();
        app.cleanup();
    } catch (const std::exception& e) {
        spdlog::critical("Fatal: {}", e.what());
        return 1;
    }
    return 0;
}
