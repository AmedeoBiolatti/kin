#pragma once

#include <kin/audio/audio.hpp>
#include <cmath>

namespace camp_walk {
// One short, synthesized mono clip; eight voices, no downloaded audio/cache.
class FootstepAudio {
    kin::AudioEngine _engine;
    kin::AudioCatalog _catalog;
    static std::unique_ptr<kin::IAudioBackend> backend(bool muted) {
        if(!muted)try{return kin::create_sdl_audio_backend(24000,2);}catch(const std::exception&){}
        return kin::create_null_audio_backend(24000,2);
    }
public:
    explicit FootstepAudio(bool muted):_engine(backend(muted),{.sample_rate=24000,.channels=2,
        .music_voices=0,.ambient_voices=0,.sound_effect_voices=8,.ui_voices=0}) {
        std::vector<kin::f32> samples(2400);kin::u32 seed=17;
        for(size_t i=0;i<samples.size();++i) {
            seed=seed*1664525u+1013904223u;
            const float t=float(i)/24000.f,noise=float(seed>>8)/8388608.f-1.f;
            samples[i]=(.28f*noise+.32f*std::sin(6.2831853f*95*t))*std::exp(-t*65)*std::min(t*1000,1.f);
        }
        _engine.add_clip("camp.step",kin::make_memory_audio_clip("camp.step",std::move(samples),1,24000));
        _catalog.add_bus({.id="sfx"});
        _catalog.add_cue({.id="footstep",.clips={"camp.step"},.category=kin::AudioCategory::Effect,
            .volume=.5f,.max_instances=8,.spatial=true,.min_distance=3,.max_distance=20});
    }
    void step(std::span<const kin::Vec3f> positions,const kin::Camera3D& camera,float dt) {
        _engine.set_listener_3d({camera.position,camera.forward(),camera.up()});
        for(const auto position:positions)_engine.play(_catalog,{.cue="footstep",.position_3d=position,.has_position_3d=true});
        _engine.update(dt);
    }
    kin::AudioEngineStats stats() const {return _engine.stats();}
};
} // namespace camp_walk
