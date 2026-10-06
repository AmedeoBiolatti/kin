#include "workloads.hpp"
#include "example_common.hpp"
#include <kin/l10n/localization.hpp>
#include <algorithm>
#include <cmath>

namespace examples {
bool render_power_grid(Arena& arena, Input& input, Renderer2D& renderer,
                       PowerGridState& state, float display_scale) {
    const auto native=renderer.scoped_native_coordinates();
    const auto output=renderer.output_size();
    if (output.x<=0 || output.y<=0) return false;
    const float dpi=std::isfinite(display_scale) && display_scale>0 ? display_scale : 1;
    const float w=output.x/dpi, h=output.y/dpi;
    const auto font=ui2::system_ui_font(15);
    constexpr auto ink=Color::rgb(226,237,244), muted=Color::rgb(149,170,188);
    constexpr auto accent=Color::rgb(105,230,201), locked=Color::rgb(68,84,104);
    constexpr std::array<std::string_view,6> branches{"mobility","weapons","salvage","ballistics","defense","recovery"};
    const auto fill=[&](Rectf r,Color c){renderer.fill_rect({r.x*dpi,r.y*dpi,r.w*dpi,r.h*dpi},c);};
    const auto outline=[&](Rectf r,Color c){renderer.draw_rect({r.x*dpi,r.y*dpi,r.w*dpi,r.h*dpi},c);};
    const auto text=[&](std::string_view s,float x,float y,float size,Color c){
        ui2::draw_text(renderer,font,s,{x*dpi,y*dpi},size/15*dpi,c);
    };
    const auto center=[&](std::string_view s,float x,float y,float size,Color c){
        ui2::draw_text_centered(renderer,font,s,{x*dpi,y*dpi},size/15*dpi,c);
    };
    const auto pixels=renderer.window_to_logical(input.mouse_pos());
    const Vec2f mouse{pixels.x/dpi,pixels.y/dpi};
    const auto over=[&](Rectf r){return mouse.x>=r.x && mouse.x<r.x+r.w && mouse.y>=r.y && mouse.y<r.y+r.h;};
    const bool click=input.mouse_frame_pressed(MouseButton::Left);
    if (click) input.consume_mouse_frame_pressed(MouseButton::Left);
    int branch=0,tier=0;
    for (int row=0;row<6;++row) for (int col=0;col<6;++col)
        if (power_branches[row][col]==state.selected) {branch=row; tier=col;}
    const int before=state.selected;
    if (input.frame_pressed("grid_next")) tier=(tier+1)%6;
    if (input.frame_pressed("grid_previous")) tier=(tier+5)%6;
    if (input.frame_pressed("grid_down")) branch=(branch+1)%6;
    if (input.frame_pressed("grid_up")) branch=(branch+5)%6;
    const Rectf map{16,154,w-32,h-294};
    if (over(map) && input.mouse_wheel_y()!=0) branch=(branch+(input.mouse_wheel_y()<0?1:5))%6;
    const float tab_w=(w-40)/3;
    std::array<Rectf,6> tabs{};
    for (int row=0;row<6;++row) {
        tabs[row]={16+(row%3)*(tab_w+4),66+(row/3)*36.0f,tab_w,30};
        if (click && over(tabs[row])) {branch=row; tier=0;}
    }
    state.selected=power_branches[branch][tier];
    const int cols=w<1000?3:6, rows=6/cols;
    const float cell_w=map.w/cols, cell_h=map.h/rows;
    std::array<Vec2f,6> nodes{};
    for (int i=0;i<6;++i) {
        const int row=i/cols, col=row%2?cols-1-i%cols:i%cols;
        nodes[i]={map.x+(col+.5f)*cell_w,map.y+row*cell_h+cell_h*.5f-14};
        if (click && over({nodes[i].x-56,nodes[i].y-26,112,70})) {
            tier=i; state.selected=power_branches[branch][i];
        }
    }
    if (state.selected!=before) state.feedback.clear();
    const Rectf back{w-146,16,130,34}, buy{w-190,h-45,174,32};
    const auto& power=power_ups[state.selected];
    const auto reason=[&]() -> std::string {
        if (arena.has_power(state.selected)) return tr("grid.reason.installed");
        if (arena.health<=0 || arena.won()) return tr("grid.reason.ended");
        if (!arena.has_power(power.parent)) return tr("grid.reason.requires",{{"name",power_name(power.parent)}});
        if (!arena.can_buy_power(state.selected)) return tr("grid.reason.need",{{"n",power.cost-arena.available_cores()}});
        return tr("grid.reason.ready",{{"n",power.cost}});
    };
    if ((click && over(buy)) || input.frame_pressed("grid_buy"))
        state.feedback=arena.buy_power(state.selected)?tr("grid.feedback.installed",{{"name",power_name(state.selected)}}):reason();
    fill({0,0,w,h},Color::rgb(12,20,30));
    text(tr("grid.title"),16,14,24,accent);
    text(tr("grid.summary",{{"count",36},{"cores",arena.available_cores()}}),16,43,12,muted);
    fill(back,Color::rgb(26,43,56)); center(tr("grid.return"),w-81,33,13,ink);
    for (int row=0;row<6;++row) {
        int owned=0; for (int id:power_branches[row]) owned+=arena.has_power(id);
        const auto r=tabs[row];
        fill(r,row==branch?Color::rgb(28,76,70):Color::rgb(24,36,51));
        outline(r,row==branch?accent:locked);
        center(tr("grid.tab",{{"name",tr("grid.branch."+std::string(branches[row]))},{"owned",owned},{"total",6}}),
               r.x+r.w*.5f,r.y+15,12,ink);
    }
    text(tr("grid.help"),16,138,12,muted);
    fill(map,Color::rgb(16,27,39));
    for (int i=1;i<6;++i) {
        auto a=nodes[i-1],b=nodes[i];
        const auto color=arena.has_power(power_branches[branch][i])?accent:locked;
        if (i%cols) {
            const float direction=b.x>a.x?1.0f:-1.0f;
            a.x+=26*direction; b.x-=26*direction;
            renderer.draw_line({a.x*dpi,a.y*dpi},{b.x*dpi,b.y*dpi},color);
        } else {
            const float x=map.x+map.w-8;
            renderer.draw_line({(a.x+26)*dpi,a.y*dpi},{x*dpi,a.y*dpi},color);
            renderer.draw_line({x*dpi,a.y*dpi},{x*dpi,b.y*dpi},color);
            renderer.draw_line({x*dpi,b.y*dpi},{(b.x+26)*dpi,b.y*dpi},color);
        }
    }
    for (int i=0;i<6;++i) {
        const int id=power_branches[branch][i]; const auto p=nodes[i];
        const Rectf box{p.x-24,p.y-24,48,48};
        const bool owned=arena.has_power(id), ready=arena.can_buy_power(id);
        fill(box,owned?Color::rgb(24,76,69):Color::rgb(26,38,53));
        outline(box,owned?accent:ready?ink:locked);
        if (id==state.selected) outline({p.x-28,p.y-28,56,56},Color::rgb(246,205,120));
        center(std::to_string(i+1),p.x,p.y-7,20,owned?accent:ink);
        center(owned?tr("grid.owned"):tr("grid.cost",{{"n",power_ups[id].cost}}),p.x,p.y+13,9,muted);
        center(power_name(id),p.x,p.y+38,12,ink);
    }
    fill({0,h-132,w,132},Color::rgb(21,33,47));
    text(tr("grid.tier",{{"name",power_name(state.selected)},{"tier",tier+1}}),16,h-122,18,ink);
    float y=h-96;
    for (const auto& line:ui2::wrap_text(font,power_description(state.selected),(w-32)*dpi,14.0f/15*dpi)) {
        text(line,16,y,14,muted); y+=18;
    }
    text(state.feedback.empty()?reason():state.feedback,16,h-32,12,accent);
    const bool ready=arena.can_buy_power(state.selected);
    fill(buy,ready?Color::rgb(29,108,91):Color::rgb(38,49,63));
    center(tr(ready?"grid.install":arena.has_power(state.selected)?"grid.installed":"grid.locked"),buy.x+buy.w*.5f,buy.y+16,12,ready?ink:muted);
    return click && over(back);
}
}
