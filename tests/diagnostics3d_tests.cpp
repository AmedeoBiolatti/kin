#include <kin/render3d/debug_diagnostics3d.hpp>
#include "../tools/asset_preview/options.hpp"
#include <cassert>
#include <map>
#include <iostream>

int main() {
    using namespace kin;
    Render3DDebugStats stats;stats.cpu_ms=1.25;stats.draws=7;stats.triangles=12345;
    stats.shadow_cached=true;stats.feature_bytes=4096;stats.local_shadow_bytes=8192;
    // Disabled sinks accept arbitrary stats without touching resources/state.
    record_render3d_debug_diagnostics(nullptr,stats);
    RuntimeDebugOptions debug;record_render3d_debug_diagnostics(&debug,stats);
    std::map<std::string,std::string> rows;std::map<std::string,double> timings;
    std::map<std::string,int> calls;
    debug.record_status=[&](std::string_view name,std::string value){++calls[std::string(name)];rows[std::string(name)]=std::move(value);};
    debug.record_timing=[&](std::string_view name,double value){timings[std::string(name)]=value;};
    stats.triangles_complete=false;
    record_render3d_diagnostics(&debug,stats,{1920,1080});
    assert(rows.at("3D main").find("partial")!=std::string::npos);
    assert(!rows.contains("3D uploads/frame") && !rows.contains("3D dispatches"));
    stats.triangles_complete=true;
    calls.clear();
    record_render3d_debug_diagnostics(&debug,stats,{1920,1080});
    for(const auto& [name,count]:calls)assert(count==1);
    assert(timings.at("render3d.cpu")==1.25);
    assert(rows.at("3D main").find("12345 triangles")!=std::string::npos);
    assert(rows.at("3D shadows").find("cached")!=std::string::npos);
    assert(rows.at("3D feature targets")=="4 KiB");
    assert(rows.at("3D local shadows")=="8 KiB");
    const auto count=rows.size();stats.draws=9;stats.cpu_ms=2;
    record_render3d_debug_diagnostics(&debug,stats,{640,480});
    assert(rows.size()==count&&timings.size()==1&&timings.at("render3d.cpu")==2);
    assert(rows.at("3D main").starts_with("9 draws"));
    // Reject bad CLI modes, dimensions and NaNs before creating an app/device.
    auto parse=[](std::initializer_list<const char*> args) {
        std::vector<char*> raw;for(const auto* arg:args)raw.push_back(const_cast<char*>(arg));
        return asset_preview::parse(int(raw.size()),raw.data());
    };
    auto options=parse({"preview","--root",".","--model","crate.glb","--mode=normals","--width=1920","--headless","--frames=2"});
    assert(options.mode==2&&options.width==1920&&options.runtime.size()==3);
    assert(parse({"preview","--help"}).help);
    for(const auto* bad:{"--mode=unknown","--width=999999","--yaw=nan","--part=-1","--time=1junk"}) {
        bool refused=false;try{parse({"preview","--model","crate.glb",bad});}catch(const std::invalid_argument&){refused=true;}
        assert(refused);
    }
    std::cout<<"3D diagnostics sinks and asset-preview argument validation passed\n";
}
