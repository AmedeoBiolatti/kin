#pragma once
// Full-scene post-processing for the GPU backend. A chain of fragment-shader passes
// is applied to the rendered scene texture at present() time (scene -> ping/pong
// render targets -> swapchain). Each pass samples the previous result at fragment
// sampler 0 (set=2 b=0); a pass may additionally bind the un-processed scene at
// sampler 1 (set=2 b=1) via `sample_original` (e.g. a bloom "combine" pass that adds a
// blurred bright-pass back onto the original). Backends without material support
// ignore the chain and present normally (graceful degradation).
#include <kin/renderer/shader.hpp>

namespace kin {

struct PostProcessPass {
    ShaderHandle shader;          // fragment material; invalid handles are skipped
    ShaderParams params{};        // fragment uniform slot 0 (16 floats / four vec4s)
    bool sample_original = false; // also bind the original scene at fragment sampler 1
};

} // namespace kin
