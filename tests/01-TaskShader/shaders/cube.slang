// cube.slang — Task/Mesh/Fragment shader pipeline for a colored unit cube.
//
// Vulkan coordinate system:
//   NDC: X right, Y DOWN, Z into screen [0, 1]
//   Front face: counter-clockwise (VK_FRONT_FACE_COUNTER_CLOCKWISE)
//   Back-face culling: VK_CULL_MODE_BACK_BIT
//
// The MVP matrix is expected to use GLM_FORCE_DEPTH_ZERO_TO_ONE + GLM_FORCE_LEFT_HANDED
// on the CPU side, which matches Vulkan's clip space conventions.

struct PushConstants {
    float4x4 mvp;
};
[[vk::push_constant]] ConstantBuffer<PushConstants> pc;

// ─── Cube geometry ───────────────────────────────────────────────────────────

static const float3 cubeVertices[8] = {
    float3(-1, -1, -1),  // 0: left-bottom-near
    float3( 1, -1, -1),  // 1: right-bottom-near
    float3( 1,  1, -1),  // 2: right-top-near
    float3(-1,  1, -1),  // 3: left-top-near
    float3(-1, -1,  1),  // 4: left-bottom-far
    float3( 1, -1,  1),  // 5: right-bottom-far
    float3( 1,  1,  1),  // 6: right-top-far
    float3(-1,  1,  1),  // 7: left-top-far
};

// 6 faces × 4 vertex indices.
// Winding order: counter-clockwise as seen from outside the cube,
// after Vulkan's Y-down projection (i.e., the face with outward normal
// toward the camera has CCW winding in screen space).
static const uint4 faceIndices[6] = {
    uint4(0, 3, 2, 1),  // Front  (-Z)
    uint4(5, 6, 7, 4),  // Back   (+Z)
    uint4(4, 7, 3, 0),  // Left   (-X)
    uint4(1, 2, 6, 5),  // Right  (+X)
    uint4(4, 0, 1, 5),  // Bottom (-Y, screen-up in Vulkan)
    uint4(3, 7, 6, 2),  // Top    (+Y, screen-down in Vulkan)
};

static const float3 faceColors[6] = {
    float3(1.0, 0.2, 0.2),  // Front:  red
    float3(0.2, 1.0, 0.2),  // Back:   green
    float3(0.2, 0.2, 1.0),  // Left:   blue
    float3(1.0, 1.0, 0.2),  // Right:  yellow
    float3(1.0, 0.2, 1.0),  // Bottom: magenta
    float3(0.2, 1.0, 1.0),  // Top:    cyan
};

// ─── Payload between task and mesh shaders ───────────────────────────────────

struct TaskPayload {
    uint dummy;  // Slang requires non-empty payload
};

// ─── Mesh shader output ──────────────────────────────────────────────────────

struct MeshVertex {
    float4 position : SV_Position;
    float3 color    : COLOR;
};

// ─── Task shader ─────────────────────────────────────────────────────────────
// Dispatches 6 mesh shader workgroups (one per cube face).

[shader("amplification")]
[numthreads(1, 1, 1)]
void taskMain(uniform uint3 dtid : SV_DispatchThreadID) {
    TaskPayload payload;
    payload.dummy = 0;
    DispatchMesh(6, 1, 1, payload);
}

// ─── Mesh shader ─────────────────────────────────────────────────────────────
// Each workgroup generates one face (4 vertices, 2 triangles).

[shader("mesh")]
[numthreads(1, 1, 1)]
[outputtopology("triangle")]
void meshMain(
    in uint groupID : SV_GroupID,
    in TaskPayload payload,
    OutputVertices<MeshVertex, 4> verts,
    OutputIndices<uint3, 2> tris
) {
    SetMeshOutputCounts(4, 2);

    uint faceIdx = groupID;
    uint4 idx = faceIndices[faceIdx];
    float3 color = faceColors[faceIdx];

    // Emit 4 vertices
    verts[0].position = mul(pc.mvp, float4(cubeVertices[idx.x], 1.0));
    verts[0].color = color;

    verts[1].position = mul(pc.mvp, float4(cubeVertices[idx.y], 1.0));
    verts[1].color = color;

    verts[2].position = mul(pc.mvp, float4(cubeVertices[idx.z], 1.0));
    verts[2].color = color;

    verts[3].position = mul(pc.mvp, float4(cubeVertices[idx.w], 1.0));
    verts[3].color = color;

    // Two triangles forming the quad (CCW)
    tris[0] = uint3(0, 1, 2);
    tris[1] = uint3(0, 2, 3);
}

// ─── Fragment shader ─────────────────────────────────────────────────────────

struct FragInput {
    float3 color : COLOR;
};

[shader("fragment")]
float4 fragMain(FragInput input) : SV_Target {
    return float4(input.color, 1.0);
}
