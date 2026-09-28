#pragma once

#include <kin/core/types.hpp>
#include <kin/renderer/color.hpp>
#include <kin/renderer/renderer2d.hpp>

#include <array>

namespace isometric_demo {

kin::Color shade(kin::Color color, kin::i32 delta);

void draw_diamond(kin::Renderer2D& renderer,
                  kin::Vec2f center,
                  kin::f32 w,
                  kin::f32 h,
                  kin::Color fill,
                  kin::Color edge);

void fill_convex_quad(kin::Renderer2D& renderer,
                      std::array<kin::Vec2f, 4> points,
                      kin::Color fill,
                      kin::Color edge);

void fill_convex_quad_body(kin::Renderer2D& renderer,
                           std::array<kin::Vec2f, 4> points,
                           kin::Color fill);

void draw_column_sides(kin::Renderer2D& renderer,
                       kin::Vec2f center,
                       kin::f32 w,
                       kin::f32 h,
                       kin::f32 depth,
                       kin::Color base);

void draw_disk(kin::Renderer2D& renderer,
               kin::Vec2f center,
               kin::f32 radius,
               kin::Color fill,
               kin::Color edge);

void draw_door_on_south_wall(kin::Renderer2D& renderer,
                             kin::Vec2f top_left,
                             kin::Vec2f top_right,
                             kin::Vec2f bottom_left,
                             kin::Vec2f bottom_right,
                             kin::f32 width,
                             kin::f32 height,
                             kin::Color color);

} // namespace isometric_demo
