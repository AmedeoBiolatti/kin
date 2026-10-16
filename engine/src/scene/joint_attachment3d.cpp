#include <kin/scene/joint_attachment3d.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace kin {
JointAttachment3D::JointAttachment3D(SceneEntity3D target,std::string_view joint,Mat4f offset) {
    if(!configure(target,joint,offset))throw std::invalid_argument("invalid joint attachment binding");
}
bool JointAttachment3D::configure(SceneEntity3D target,std::string_view joint,Mat4f offset) {
    if(!target||joint.empty()||joint.size()>max_attachment_joint_name3d||joint.find('\0')!=std::string_view::npos)return false;
    for(auto value:offset.m)if(!std::isfinite(value))return false;
    if(offset.m[3]!=0||offset.m[7]!=0||offset.m[11]!=0||offset.m[15]!=1)return false;
    _target=target;_joint.fill(0);std::copy(joint.begin(),joint.end(),_joint.begin());_joint_length=joint.size();_offset=offset;
    _model.reset();_index=-1;_frame=0;_status=JointAttachmentStatus3D::Unbound;return true;
}
} // namespace kin
