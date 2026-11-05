// kin_import_material: other engines' materials as kin's .kinmaterials (kin/render3d/material_import3d.hpp).
//
//   kin_import_material godot|unity|unreal <material | folder> --project <its project> --content <kin content root>
//                       --out <folder in the content> [--dry-run] [--quiet]
//
// A folder is searched for Godot's StandardMaterial3D / ORMMaterial3D resources (.tres), Unity's materials (.mat) or
// Unreal's as tools/import_material/unreal_export.py exports them (.json; --project: that export's folder); each is
// written to <out>, in the folder it is in under the one searched. Every material's notes (what kin does not
// show as the source does) are printed; the exit status is 1 when one could not be imported.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <kin/render3d/material_import3d.hpp>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

int usage() {
    std::cerr << "usage: kin_import_material godot|unity|unreal <material | folder> --project <dir> --content <dir> "
                 "--out <dir>"
                 " [--dry-run] [--quiet]\n";
    return 2;
}

// A Godot material resource, by its first line.
bool godot_material(const fs::path& file) {
    std::ifstream in(file);
    std::string first;
    std::getline(in, first);
    return first.rfind("[gd_resource", 0) == 0 && (first.find("type=\"StandardMaterial3D\"") != std::string::npos ||
                                                   first.find("type=\"ORMMaterial3D\"") != std::string::npos);
}

} // namespace

int main(int argc, char** argv) {
    const std::string_view engine = argc >= 2 ? argv[1] : "";
    if (argc < 3 || (engine != "godot" && engine != "unity" && engine != "unreal"))
        return usage();
    const bool unity = engine == "unity", unreal = engine == "unreal";
    const fs::path input = argv[2];
    kin::MaterialImportOptions3D options;
    bool quiet = false;
    for (int i = 3; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto next = [&]() -> fs::path { return i + 1 < argc ? fs::path(argv[++i]) : fs::path(); };
        if (arg == "--project")
            options.project = next();
        else if (arg == "--content")
            options.content = next();
        else if (arg == "--out")
            options.out = next();
        else if (arg == "--dry-run")
            options.write = false;
        else if (arg == "--quiet")
            quiet = true;
        else
            return usage();
    }
    if (options.project.empty() || options.content.empty() || options.out.empty())
        return usage();

    std::vector<std::pair<fs::path, fs::path>> jobs; // the source, where its import goes
    if (fs::is_directory(input)) {
        for (const auto& entry : fs::recursive_directory_iterator(input))
            if (entry.is_regular_file() &&
                (unity    ? entry.path().extension() == ".mat"
                 : unreal ? entry.path().extension() == ".json"
                          : entry.path().extension() == ".tres" && godot_material(entry.path())))
                jobs.emplace_back(entry.path(), options.out / entry.path().parent_path().lexically_relative(input));
        std::sort(jobs.begin(), jobs.end());
    } else {
        jobs.emplace_back(input, options.out);
    }

    int failed = 0, noted = 0;
    std::map<std::string, int> counts; // how many materials each note's kind came up in
    for (const auto& [source, out] : jobs) {
        auto job = options;
        job.out = out;
        const auto result = unity    ? kin::import_unity_material(source, job)
                            : unreal ? kin::import_unreal_material(source, job)
                                     : kin::import_godot_material(source, job);
        if (!result) {
            ++failed;
            for (const auto& d : result.diagnostics)
                std::cerr << "FAILED " << d << '\n';
            continue;
        }
        noted += !result->notes.empty();
        if (!quiet) {
            std::cout << source.string() << " -> " << result->file.string() << '\n';
            for (const auto& n : result->notes)
                std::cout << "  note: " << n << '\n';
        }
        for (const auto& n : result->notes)
            ++counts[n.substr(0, n.find(':'))];
    }
    std::cout << jobs.size() - failed << " of " << jobs.size() << " imported, " << noted << " with notes";
    if (!options.write)
        std::cout << " (dry run: nothing written)";
    std::cout << '\n';
    for (const auto& [what, n] : counts)
        std::cout << "  " << n << " x " << what << '\n';
    return failed ? 1 : 0;
}
