#pragma once

#include <kin/physics3d/world3d.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace kin {
struct SpringArm3DSettings {
    f32 distance=4.2f,shoulder=.35f,probe_radius=.22f,margin=.12f,ease_out=5;
    f32 shoulder_radius_scale=1;
};
struct SpringArm3DFrame {
    Vec3f pivot{},position{};
    f32 length=0;
    bool obstructed=false,shoulder_obstructed=false,initial_overlap=false;
};
// Fixed-size spring-arm state shared by raw-world and scene-aware cameras.
// Probe shapes are retained; configure only allocates when their radii change.
class SpringArm3D {
public:
    explicit SpringArm3D(SpringArm3DSettings settings={}) {
        if(!configure(settings))throw std::invalid_argument("invalid spring arm settings");
    }
    bool configure(SpringArm3DSettings settings) {
        if(!std::isfinite(settings.distance)||settings.distance<0||!std::isfinite(settings.shoulder)||
            !std::isfinite(settings.probe_radius)||settings.probe_radius<=0||!std::isfinite(settings.margin)||settings.margin<0||
            !std::isfinite(settings.ease_out)||settings.ease_out<0||!std::isfinite(settings.shoulder_radius_scale)||
            settings.shoulder_radius_scale<=0||!std::isfinite(settings.probe_radius*settings.shoulder_radius_scale)||settings.probe_radius*settings.shoulder_radius_scale<=0)return false;
        auto probe=_probe,side=_side;
        if(!probe||settings.probe_radius!=_settings.probe_radius)probe=Shape3D::sphere(settings.probe_radius);
        if(settings.shoulder_radius_scale==1)side=probe;
        else if(!side||settings.probe_radius*settings.shoulder_radius_scale!=_settings.probe_radius*_settings.shoulder_radius_scale)
            side=Shape3D::sphere(settings.probe_radius*settings.shoulder_radius_scale);
        if(!probe||!side)return false;
        _probe=std::move(probe);_side=std::move(side);_settings=settings;return true;
    }
    const SpringArm3DSettings& settings()const{return _settings;}
    void reset(){_length=-1;}
    // cast(shape, origin, displacement) returns optional<ShapeHit3D>. It is
    // synchronous on the caller thread; the common solver retains no callback.
    template<class Cast> std::optional<SpringArm3DFrame> frame(Vec3f focus,Quatf rotation,f32 dt,Cast&& cast) {
        const auto finite=[](Vec3f p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);};
        if(!finite(focus)||!std::isfinite(rotation.x)||!std::isfinite(rotation.y)||!std::isfinite(rotation.z)||
            !std::isfinite(rotation.w)||!std::isfinite(dt)||dt<0)return {};
        const auto norm=rotation.x*rotation.x+rotation.y*rotation.y+rotation.z*rotation.z+rotation.w*rotation.w;
        if(!std::isfinite(norm)||norm<1e-12f)return {};
        rotation=rotation.normalized();SpringArm3DFrame result;result.pivot=focus;
        const auto side=rotation.rotate({1,0,0})*_settings.shoulder;
        if(!finite(side)||!std::isfinite(length_squared(side)))return {};
        if(length_squared(side)>0) {
            const auto hit=cast(_side,focus,side);
            if(hit&&!std::isfinite(hit->fraction))return {};
            result.shoulder_obstructed=bool(hit);
            result.initial_overlap=hit&&hit->penetration>0;
            result.pivot=focus+side*(hit?std::max(0.f,std::clamp(hit->fraction,0.f,1.f)-.05f):1.f);
        }
        const auto back=rotation.rotate({0,0,1});
        const auto delta=back*_settings.distance;
        if(!finite(result.pivot)||!finite(delta)||!std::isfinite(length_squared(delta)))return {};
        const auto hit=_settings.distance>0?cast(_probe,result.pivot,delta):std::optional<ShapeHit3D>{};
        if(hit&&!std::isfinite(hit->fraction))return {};
        const float available=hit?std::max(0.f,std::clamp(hit->fraction,0.f,1.f)*_settings.distance-_settings.margin):_settings.distance;
        result.initial_overlap|=hit&&hit->penetration>0;
        result.obstructed=bool(hit)||result.shoulder_obstructed;
        const float arm=_length<0||available<_length?available:std::min(available,_length+_settings.ease_out*dt);
        result.length=std::min(arm,available);result.position=result.pivot+back*result.length;
        if(!finite(result.position))return {};
        _length=result.length;return result;
    }
    std::optional<SpringArm3DFrame> frame(const World3D& world,Vec3f focus,Quatf rotation,f32 dt,const QueryFilter3D& filter={}) {
        auto camera_filter=filter;camera_filter.initial_overlap=true;
        return frame(focus,rotation,dt,[&](const Shape3D& shape,Vec3f from,Vec3f delta){return world.shape_cast(shape,from,{},delta,camera_filter);});
    }
private:
    SpringArm3DSettings _settings;
    Shape3D _probe,_side;
    f32 _length=-1;
};
} // namespace kin
