// Synthetic fragment-pipeline partner, not a recovered game vertex shader.
struct FixtureOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float4 varying0 : TEXCOORD0;
    [[vk::location(1)]] float4 varying1 : TEXCOORD1;
    [[vk::location(2)]] float4 varying2 : TEXCOORD2;
    [[vk::location(3)]] float4 varying3 : TEXCOORD3;
    [[vk::location(4)]] float4 varying4 : TEXCOORD4;
    [[vk::location(5)]] float4 varying5 : TEXCOORD5;
    [[vk::location(6)]] float4 varying6 : TEXCOORD6;
    [[vk::location(7)]] float4 varying7 : TEXCOORD7;
    [[vk::location(8)]] float4 varying8 : TEXCOORD8;
    [[vk::location(9)]] float4 varying9 : TEXCOORD9;
    [[vk::location(10)]] float4 varying10 : TEXCOORD10;
    [[vk::location(11)]] float4 varying11 : TEXCOORD11;
    [[vk::location(12)]] float4 varying12 : TEXCOORD12;
    [[vk::location(13)]] float4 varying13 : TEXCOORD13;
    [[vk::location(14)]] float4 varying14 : TEXCOORD14;
    [[vk::location(15)]] float4 varying15 : TEXCOORD15;
    [[vk::location(16)]] float4 varying16 : TEXCOORD16;
    [[vk::location(17)]] float4 varying17 : TEXCOORD17;
};
FixtureOutput shaderMain(uint index : SV_VertexID) {
    FixtureOutput result;
    result.position = float4(index == 2 ? 3.0 : -1.0, index == 1 ? 3.0 : -1.0, 0, 1);
    result.varying0 = float4(0, 0, 0, 1);
    result.varying1 = float4(0, 0, 0, 1);
    result.varying2 = float4(0, 0, 0, 1);
    result.varying3 = float4(0, 0, 0, 1);
    result.varying4 = float4(0, 0, 0, 1);
    result.varying5 = float4(0, 0, 0, 1);
    result.varying6 = float4(0, 0, 0, 1);
    result.varying7 = float4(0, 0, 0, 1);
    result.varying8 = float4(0, 0, 0, 1);
    result.varying9 = float4(0, 0, 0, 1);
    result.varying10 = float4(0, 0, 0, 1);
    result.varying11 = float4(0, 0, 0, 1);
    result.varying12 = float4(0, 0, 0, 1);
    result.varying13 = float4(0, 0, 0, 1);
    result.varying14 = float4(0, 0, 0, 1);
    result.varying15 = float4(0, 0, 0, 1);
    result.varying16 = float4(0, 0, 0, 1);
    result.varying17 = float4(0, 0, 0, 1);
    return result;
}
