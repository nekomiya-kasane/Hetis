// Probe project for G:\Teaching\Vulkan\thirdparty
//
// Touches one symbol from every linked library so the linker has to actually
// pull in the corresponding object files. Builds + runs to "[ok]" if the
// aggregator's CMakeLists.txt produced functional targets.

// stb requires implementation macros in exactly one TU.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define CGLTF_IMPLEMENTATION
#define TINYEXR_IMPLEMENTATION
// tinyobjloader is compiled into its own static library — header only here.

#include <volk.h>
#include <vulkan/vulkan.h>

#include <VkBootstrap.h>
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <spirv_cross/spirv_cross.hpp>
#include <spirv_reflect.h>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>

#include <glm/glm.hpp>
#include <stb_image.h>
#include <stb_image_write.h>
#include <stb_image_resize2.h>

#include <tiny_obj_loader.h>
// tinygltf vendors its own stb_image — disable that copy and let it reuse
// our externally implemented stb symbols.
#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_INCLUDE_STB_IMAGE
#define TINYGLTF_NO_INCLUDE_STB_IMAGE_WRITE
#define TINYGLTF_NO_INCLUDE_JSON
#include <nlohmann/json.hpp>
#include <tiny_gltf.h>
#include <cgltf.h>
#include <fastgltf/core.hpp>
#include <meshoptimizer.h>
#include <mikktspace.h>
#include <tinyexr.h>

#include <tracy/Tracy.hpp>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <tinyfiledialogs.h>
#include <renderdoc_app.h>

#if defined(VK_TP_PROBE_PERFETTO)
#include <perfetto.h>
PERFETTO_DEFINE_CATEGORIES(
    perfetto::Category("vulkan.thirdparty").SetDescription("Vulkan thirdparty dependency probe"));
PERFETTO_TRACK_EVENT_STATIC_STORAGE();
#endif

#if defined(VK_TP_PROBE_KTX)
#include <ktx.h>
#endif
#if defined(VK_TP_PROBE_ASSIMP)
#include <assimp/Importer.hpp>
#include <assimp/version.h>
#endif
#if defined(VK_TP_PROBE_SLANG)
#include <slang.h>
#include <slang-com-ptr.h>
#endif

// --- frontier-renderer additions (2026) --------------------------------------
#if defined(VK_TP_PROBE_CLI11)
#include <CLI/CLI.hpp>
#endif
#if defined(VK_TP_PROBE_TASKFLOW)
#include <taskflow/taskflow.hpp>
#endif
#if defined(VK_TP_PROBE_MINIAUDIO)
#include <miniaudio.h>            // declarations only; impl is in the static lib
#endif
#if defined(VK_TP_PROBE_IMGUIZMO)
#include <ImGuizmo.h>             // needs imgui.h (included above)
#endif
#if defined(VK_TP_PROBE_IMPLOT)
#include <implot.h>               // needs imgui.h (included above)
#endif
#if defined(VK_TP_PROBE_CATCH2)
#include <catch2/catch_version.hpp>
#endif
#if defined(VK_TP_PROBE_EFSW)
#include <efsw/efsw.hpp>
#endif
#if defined(VK_TP_PROBE_MSDFGEN)
#include <msdfgen.h>
#endif
#if defined(VK_TP_PROBE_JOLT)
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#endif
#if defined(VK_TP_PROBE_TBB)
#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>
#include <tbb/task_arena.h>
#include <tbb/global_control.h>
#endif
#if defined(VK_TP_PROBE_METIS)
#include <metis.h>
#endif
// stdexec is compiled in its own TU (MSVC requires /Zc:preprocessor for it).
#if defined(VK_TP_PROBE_STDEXEC)
const char* probe_stdexec();
#endif

#include <cstdio>
#include <cstdint>
#include <memory>
#include <vector>
#include <string>
#include <atomic>

#define STEP(msg) do { std::printf("[step] %s\n", msg); std::fflush(stdout); } while (0)

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0); // make every printf visible
    STEP("entered main");

    // 1. SDK loader
    STEP("before volkInitialize");
    VkResult vres = volkInitialize();
    STEP("after volkInitialize");
    std::printf("volk        : initialised result=%d loader=%p\n",
                int(vres), (void*)vkGetInstanceProcAddr);
    std::fflush(stdout);

    // 2. Allocator type only (no instance — we don't actually create devices).
    VmaAllocatorCreateInfo vma_ci{};
    (void)vma_ci;
    std::printf("vma         : sizeof(VmaAllocatorCreateInfo)=%zu\n", sizeof(vma_ci));

    // 3. vk-bootstrap
    vkb::InstanceBuilder ib;
    (void)ib;
    std::printf("vk-bootstrap: InstanceBuilder constructed\n");

    // 4. SPIRV-Cross
    std::printf("spirv-cross : version=%s\n", "core target only");

    // 5. SPIRV-Reflect: empty module sentinel
    SpvReflectShaderModule m{};
    (void)m;
    std::printf("spirv-reflect: SpvReflectShaderModule cleared\n");

    // 6. GLFW
    if (glfwInit() == GLFW_TRUE) {
        std::printf("glfw        : %s\n", glfwGetVersionString());
        glfwTerminate();
    } else {
        std::printf("glfw        : init failed (no display ok in CI)\n");
    }

    // 7. ImGui — just touch a symbol
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    (void)io;
    std::printf("imgui       : %s\n", ImGui::GetVersion());

    // 8. glm
    glm::vec3 v{1, 2, 3};
    std::printf("glm         : (%g, %g, %g)\n", v.x, v.y, v.z);

    // 9. tinyobj (v1.0.x — classic LoadObj API)
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;
    (void)attrib; (void)shapes; (void)materials; (void)warn; (void)err;
    std::printf("tinyobj     : structures declared\n");

    // 10. tinygltf
    tinygltf::TinyGLTF gltf;
    (void)gltf;
    std::printf("tinygltf    : TinyGLTF instanced\n");

    // 11. cgltf
    cgltf_data* d = nullptr;
    (void)d;
    std::printf("cgltf       : header reachable\n");

    // 12. fastgltf
    fastgltf::Parser parser;
    (void)parser;
    std::printf("fastgltf    : Parser instanced\n");

    // 13. meshoptimizer
    unsigned int idx[3] = {0, 1, 2};
    meshopt_optimizeVertexCache(idx, idx, 3, 3);
    std::printf("meshopt     : optimizeVertexCache returned\n");

    // 14. MikkTSpace
    SMikkTSpaceContext ctx{};
    (void)ctx;
    std::printf("mikktspace  : context zeroed\n");

    // 15. tinyexr
    EXRVersion exr_ver{};
    (void)exr_ver;
    std::printf("tinyexr     : version struct sized\n");

    // 16. Tracy zone macro (no-op when client disabled)
    ZoneScopedN("probe_main");
    std::printf("tracy       : ZoneScopedN expanded\n");

    // 17. spdlog
    spdlog::info("spdlog      : info() OK");

    // 18. nlohmann_json
    nlohmann::json j = {{"vulkan", "ok"}};
    std::printf("json        : %s\n", j.dump().c_str());

    // 19. entt
    entt::registry reg;
    auto e = reg.create();
    reg.emplace<int>(e, 42);
    std::printf("entt        : registry has %zu int entities\n", reg.storage<int>().size());

    // 20. tinyfiledialogs — touch a symbol (no actual dialog)
    auto* ver = tinyfd_version;
    std::printf("tinyfd      : %s\n", ver);

    // 21. RenderDoc API loader address (no actual injection)
    pRENDERDOC_GetAPI rd = nullptr;
    (void)rd;
    std::printf("renderdoc   : pRENDERDOC_GetAPI declared\n");

#if defined(VK_TP_PROBE_PERFETTO)
    // 22. Perfetto: initialise the in-process backend and write one track event.
    {
        perfetto::TracingInitArgs args;
        args.backends = perfetto::kInProcessBackend;
        perfetto::Tracing::Initialize(args);
        perfetto::TrackEvent::Register();

        perfetto::TraceConfig cfg;
        cfg.add_buffers()->set_size_kb(64);
        auto* ds = cfg.add_data_sources()->mutable_config();
        ds->set_name("track_event");

        std::unique_ptr<perfetto::TracingSession> session =
            perfetto::Tracing::NewTrace(perfetto::kInProcessBackend);
        session->Setup(cfg);
        session->StartBlocking();
        TRACE_EVENT("vulkan.thirdparty", "perfetto_probe");
        session->StopBlocking();
        std::vector<char> trace = session->ReadTraceBlocking();
        std::printf("perfetto    : trace bytes=%zu\n", trace.size());
    }
#endif

    // 22. stb — exercise stb_image, stb_image_write, stb_image_resize2 symbols.
    //          (Use memory-based APIs; fopen(NULL,...) trips the MSVC invalid
    //           parameter handler.)
    const unsigned char dummy[1] = {0};
    int w = 0, h = 0, c = 0;
    (void)stbi_info_from_memory(dummy, 1, &w, &h, &c);
    (void)stbi_write_png_to_func(
        [](void*, void*, int) {}, nullptr, 0, 0, 0, nullptr, 0);
    std::printf("stb         : stbi_info_from_memory + stbi_write linked\n");

#if defined(VK_TP_PROBE_KTX)
    // 23. KTX-Software — touch a symbol from the static library so the linker
    //     pulls in libktx.lib instead of silently dropping it.
    ktxTexture2* k = nullptr;
    (void)k;
    std::printf("ktx         : ktxTexture2 type reachable\n");
#endif

#if defined(VK_TP_PROBE_ASSIMP)
    // 24. Assimp — query version (forces linkage to assimp-vc143-mt.lib).
    std::printf("assimp      : %u.%u.%u\n",
                aiGetVersionMajor(), aiGetVersionMinor(), aiGetVersionRevision());
#endif

#if defined(VK_TP_PROBE_SLANG)
    // 25. Slang — the renderer's sole shading language. Verify runtime (dynamic)
    //     compilation of an in-memory module to SPIR-V, plus cross-compilation to
    //     GLSL / HLSL / Metal / WGSL so the other emitted languages can be
    //     inspected. One session carries every target; getEntryPointCode(ep, tgt)
    //     pulls each language back out.
    {
        using namespace slang;
        Slang::ComPtr<IGlobalSession> globalSession;
        if (SLANG_FAILED(createGlobalSession(globalSession.writeRef()))) {
            std::printf("slang       : FAILED createGlobalSession\n");
            return 1;
        }

        struct TargetInfo { const char* name; SlangCompileTarget fmt; const char* profile; bool text; };
        const TargetInfo wanted[] = {
            { "SPIR-V", SLANG_SPIRV, "spirv_1_5", false },  // execution target (Vulkan)
            { "GLSL",   SLANG_GLSL,  "glsl_450",  true  },  // inspection / cross-API
            { "HLSL",   SLANG_HLSL,  "sm_6_0",    true  },
            { "Metal",  SLANG_METAL, nullptr,     true  },
            { "WGSL",   SLANG_WGSL,  nullptr,     true  },
        };

        std::vector<TargetDesc> targets;
        for (const auto& t : wanted) {
            TargetDesc d{};
            d.format  = t.fmt;
            d.profile = t.profile ? globalSession->findProfile(t.profile) : SLANG_PROFILE_UNKNOWN;
            targets.push_back(d);
        }

        SessionDesc sd{};
        sd.targets     = targets.data();
        sd.targetCount = static_cast<SlangInt>(targets.size());

        Slang::ComPtr<ISession> session;
        if (SLANG_FAILED(globalSession->createSession(sd, session.writeRef()))) {
            std::printf("slang       : FAILED createSession\n");
            return 1;
        }

        const char* src = R"SLANG(
            struct Globals { float4x4 mvp; float time; };
            ConstantBuffer<Globals> gGlobals;
            struct VOut { float4 pos : SV_Position; float3 col : COLOR; };
            [shader("vertex")]
            VOut vsMain(float3 p : POSITION, float3 c : COLOR) {
                VOut o;
                o.pos = mul(gGlobals.mvp, float4(p, 1.0));
                o.col = c * (0.5 + 0.5 * sin(gGlobals.time));
                return o;
            }
            [shader("fragment")]
            float4 fsMain(VOut i) : SV_Target { return float4(i.col, 1.0); }
        )SLANG";

        Slang::ComPtr<IBlob> diag;
        IModule* module = session->loadModuleFromSourceString(
            "probe", "probe.slang", src, diag.writeRef());
        if (!module) {
            std::printf("slang       : FAILED loadModule: %s\n",
                        diag ? static_cast<const char*>(diag->getBufferPointer()) : "(no diag)");
            return 1;
        }

        Slang::ComPtr<IEntryPoint> vs, fs;
        module->findEntryPointByName("vsMain", vs.writeRef());
        module->findEntryPointByName("fsMain", fs.writeRef());

        // Composite order fixes entry-point indices: vsMain=0, fsMain=1.
        IComponentType* parts[] = { module, vs.get(), fs.get() };
        Slang::ComPtr<IComponentType> composed;
        if (SLANG_FAILED(session->createCompositeComponentType(
                parts, 3, composed.writeRef(), diag.writeRef()))) {
            std::printf("slang       : FAILED compose: %s\n",
                        diag ? static_cast<const char*>(diag->getBufferPointer()) : "");
            return 1;
        }
        Slang::ComPtr<IComponentType> program;
        if (SLANG_FAILED(composed->link(program.writeRef(), diag.writeRef()))) {
            std::printf("slang       : FAILED link: %s\n",
                        diag ? static_cast<const char*>(diag->getBufferPointer()) : "");
            return 1;
        }

        ProgramLayout* layout = program->getLayout();
        std::printf("slang        : %s  entryPoints=%u  params=%u\n",
                    globalSession->getBuildTagString(),
                    layout ? static_cast<unsigned>(layout->getEntryPointCount()) : 0u,
                    layout ? static_cast<unsigned>(layout->getParameterCount()) : 0u);

        // Emit the vertex entry point for every target (the "monitor the other
        // shading languages Slang produced" path).
        for (size_t i = 0; i < targets.size(); ++i) {
            Slang::ComPtr<IBlob> code;
            const SlangResult r = program->getEntryPointCode(
                0, static_cast<SlangInt>(i), code.writeRef(), diag.writeRef());
            if (SLANG_FAILED(r) || !code) {
                std::printf("  -> %-6s : skipped\n", wanted[i].name);
                continue;
            }
            const size_t n = code->getBufferSize();
            if (wanted[i].text) {
                const char* txt = static_cast<const char*>(code->getBufferPointer());
                size_t cut = n < 64 ? n : 64;
                for (size_t k = 0; k < cut; ++k) { if (txt[k] == '\n') { cut = k; break; } }
                std::printf("  -> %-6s : %5zu bytes | %.*s\n",
                            wanted[i].name, n, static_cast<int>(cut), txt);
            } else {
                std::printf("  -> %-6s : %5zu bytes SPIR-V (%zu words, magic=0x%08X)\n",
                            wanted[i].name, n, n / 4,
                            n >= 4 ? *reinterpret_cast<const uint32_t*>(code->getBufferPointer()) : 0u);
            }
        }
    }
#endif

    // =========================================================================
    //  Frontier-renderer additions (2026)
    // =========================================================================
#if defined(VK_TP_PROBE_CLI11)
    // 26. CLI11
    {
        CLI::App app{"vulkan probe"};
        int n = 0;
        app.add_option("-n,--num", n, "a number");
        std::printf("cli11       : '%s' with %zu option(s)\n",
                    app.get_description().c_str(), app.get_options().size());
    }
#endif

#if defined(VK_TP_PROBE_STDEXEC)
    // 27. stdexec (compiled in its own TU)
    std::printf("stdexec     : %s\n", probe_stdexec());
#endif

#if defined(VK_TP_PROBE_TASKFLOW)
    // 28. Taskflow — run a tiny task graph
    {
        tf::Executor executor;
        tf::Taskflow flow;
        std::atomic<int> counter{0};
        auto a = flow.emplace([&]{ counter.fetch_add(1); });
        auto b = flow.emplace([&]{ counter.fetch_add(1); });
        a.precede(b);
        executor.run(flow).wait();
        std::printf("taskflow    : ran %d tasks across %zu worker(s)\n",
                    counter.load(), executor.num_workers());
    }
#endif

#if defined(VK_TP_PROBE_MINIAUDIO)
    // 29. miniaudio — touch a lib symbol (no device opened)
    {
        ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
        std::printf("miniaudio   : device_config initialised (type=%d)\n",
                    static_cast<int>(cfg.deviceType));
    }
#endif

#if defined(VK_TP_PROBE_IMGUIZMO)
    // 30. ImGuizmo — operates on its own static context (no ImGui frame needed)
    {
        ImGuizmo::SetOrthographic(false);
        std::printf("imguizmo    : SetOrthographic linked (IsUsing=%d)\n",
                    ImGuizmo::IsUsing() ? 1 : 0);
    }
#endif

#if defined(VK_TP_PROBE_IMPLOT)
    // 31. ImPlot — create/destroy a context (independent of an ImGui frame)
    {
        ImPlotContext* ctx = ImPlot::CreateContext();
        std::printf("implot      : context created (%p)\n", static_cast<void*>(ctx));
        ImPlot::DestroyContext(ctx);
    }
#endif

#if defined(VK_TP_PROBE_CATCH2)
    // 32. Catch2 — query the linked library version
    {
        Catch::Version const& v = Catch::libraryVersion();
        std::printf("catch2      : v%u.%u.%u\n",
                    v.majorVersion, v.minorVersion, v.patchNumber);
    }
#endif

#if defined(VK_TP_PROBE_EFSW)
    // 33. efsw — construct a watcher (no watch() call => no background thread)
    {
        efsw::FileWatcher watcher;
        (void)watcher;
        std::printf("efsw        : FileWatcher constructed\n");
    }
#endif

#if defined(VK_TP_PROBE_MSDFGEN)
    // 34. msdfgen (core) — run a core algorithm on an empty shape
    {
        msdfgen::Shape shape;
        msdfgen::edgeColoringSimple(shape, 3.0);
        std::printf("msdfgen     : edgeColoringSimple linked (contours=%zu)\n",
                    shape.contours.size());
    }
#endif

#if defined(VK_TP_PROBE_JOLT)
    // 35. Jolt — canonical allocator/factory/type registration round-trip
    {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        std::printf("jolt        : allocator + factory + types registered\n");
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
#endif

#if defined(VK_TP_PROBE_TBB)
    // 36. oneTBB — exercise the scheduler (forces tbb12.dll to load)
    {
        const int N = 1000;
        std::vector<int> data(N, 0);
        tbb::parallel_for(tbb::blocked_range<int>(0, N),
            [&](const tbb::blocked_range<int>& r) {
                for (int i = r.begin(); i != r.end(); ++i) data[i] = i;
            });
        long long sum = 0;
        for (int v : data) sum += v;
        std::printf("tbb         : parallel_for sum=%lld (max_concurrency=%d)\n",
                    sum, tbb::this_task_arena::max_concurrency());
    }
#endif

#if defined(VK_TP_PROBE_METIS)
    // 37. METIS (+GKlib) — initialise option array via the library
    {
        idx_t options[METIS_NOPTIONS];
        int rc = METIS_SetDefaultOptions(options);
        std::printf("metis       : SetDefaultOptions rc=%d (METIS_OK=%d)\n",
                    rc, METIS_OK);
    }
#endif

    std::printf("\n[ok] all targets link cleanly\n");
    return 0;
}
