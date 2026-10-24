#pragma once
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/renderer2d.hpp>

void check_webgl_materials(kin::Renderer2D& renderer, kin::Renderer3D& renderer3d);
void check_webgl_geometry2d(kin::Renderer2D& renderer);
void check_webgl_instances(kin::Renderer2D& renderer, kin::Renderer3D& renderer3d);
