// ASCII only: included by MSVC and FXC.

#ifndef KD_PARTICLE_GPU_LAYOUT_HLSLI
#define KD_PARTICLE_GPU_LAYOUT_HLSLI

#define KD_RIBBON_GPU_MAX_POINTS 12

#ifndef __cplusplus
// Match ParticleConst.h field order.
struct ParticleData
{
	float3 position;
	float life;

	float3 velocity;
	float lifeMax;

	float4 colorStart;
	float4 colorEnd;

	float sizeStart;
	float sizeEnd;
	float rotation;
	float rotationSpeed;

	float emissiveIntensity;
	float3 gravity;
};

struct RibbonVertexData
{
	float3 position;
	float width;

	float4 color;

	float emissiveIntensity;
	float groupId;
	float uCoord;
	float pad;
};

struct RibbonTrailPointData
{
	float3 position;
	float age;
};

struct RibbonTrailState
{
	uint count;
	float3 pad;
	float3 lastAddedPos;
	float pad2;
};
#endif

#endif
