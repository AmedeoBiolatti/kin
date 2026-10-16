#include <kin/scene/interaction3d.hpp>
#include <cmath>
#include <stdexcept>

namespace kin {
namespace {
bool finite(Vec3f p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
Shape3D reach_shape(f32 reach) {
    if(!std::isfinite(reach)||reach<=0||!std::isfinite(reach*reach))
        throw std::invalid_argument("interaction reach must be positive and finite");
    return Shape3D::sphere(reach);
}
}
InteractionQuery3D::InteractionQuery3D(InteractionSettings3D settings):_settings(settings),_reach(reach_shape(settings.reach)){}
InteractionStatus3D InteractionQuery3D::observer_status(const SceneQueries3D& queries,const InteractionObserver3D& observer)const {
    if(!queries)return InteractionStatus3D::Unavailable;
    if(!finite(observer.origin)||(observer.view&&!finite(*observer.view))||!queries.resolve(observer.actor))
        return InteractionStatus3D::InvalidObserver;
    return InteractionStatus3D::Ready;
}
std::optional<InteractionFocus3D> InteractionQuery3D::check(const SceneQueries3D& queries,const InteractionObserver3D& observer,
                                                         SceneEntity3D target,const InteractionOffer3D& offer)const {
    if(!offer.verb||!finite(offer.point))return {};
    const auto distance=length_squared(offer.point-observer.origin);
    if(!std::isfinite(distance)||distance>_settings.reach*_settings.reach)return {};
    if(_settings.require_sight) {
        const auto visible=[&](Vec3f from) {
            // A zero-length ray has no geometric hit to prove visibility.
            if(length_squared(offer.point-from)<1e-12f)return false;
            const auto hit=queries.raycast(from,offer.point,
                {.mask=_settings.sight,.ignore=observer.actor,.sensors=_settings.sight_sensors});
            return hit&&hit->entity==target&&bool(queries.resolve(*hit));
        };
        if(!visible(observer.origin)||(observer.view&&!visible(*observer.view)))return {};
    }
    return InteractionFocus3D{target,offer.verb,offer.point,distance};
}
} // namespace kin
