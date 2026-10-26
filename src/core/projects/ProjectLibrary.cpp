#include "projects/ProjectLibrary.h"
#include "archive/ZipService.h"
#include "blender/Process.h"
#include "config/Utf8Path.h"

#include <nlohmann/json.hpp>
#include <zip.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <set>

namespace blendnet::projects {
namespace {
bool stopped(const std::atomic_bool* flag) { return flag && flag->load(); }
std::string text(const std::filesystem::path& p) { return blender::utf8(p); }
constexpr auto inspection = R"PY(
import bpy, json, os
s=bpy.context.scene
deps=set(bpy.utils.blend_paths(absolute=True, packed=False, local=False))
# Blender 4.3+ reports Paint's Essentials brush weak references as file paths,
# even in a new scene with no linked data. They are editing preferences, resolved
# against each installation's bundled assets, not files required by the render.
# Match the declared Essentials reference, never all paths containing "assets".
essential_brush_suffixes=set()
for scene in bpy.data.scenes:
    for prop in scene.tool_settings.bl_rna.properties:
        if prop.type != 'POINTER': continue
        paint=getattr(scene.tool_settings, prop.identifier, None)
        if not isinstance(paint, bpy.types.Paint): continue
        ref=getattr(paint, 'brush_asset_reference', None)
        if not ref or ref.asset_library_type != 'ESSENTIALS': continue
        relative=ref.relative_asset_identifier.replace('\\', '/')
        library, separator, asset=relative.partition('.blend/')
        if not separator or not asset.startswith('Brush/'): continue
        parts=library.split('/')
        if any(part in ('', '.', '..') for part in parts): continue
        suffix='/datafiles/assets/'+library+'.blend'
        essential_brush_suffixes.add(os.path.normcase(suffix).replace('\\', '/'))
linked_libraries={os.path.normcase(os.path.realpath(bpy.path.abspath(lib.filepath, library=lib.parent)))
                  for lib in bpy.data.libraries}
deps={p for p in deps
      if os.path.normcase(os.path.realpath(p)) in linked_libraries
      or not any(os.path.normcase(os.path.normpath(p)).replace('\\', '/').endswith(suffix)
                 for suffix in essential_brush_suffixes)}
for obj in bpy.data.objects:
    for mod in obj.modifiers:
        domain=getattr(mod, 'domain_settings', None)
        if domain and domain.cache_directory:
            deps.add(bpy.path.abspath(domain.cache_directory))
        pc=getattr(mod, 'point_cache', None)
        if pc and getattr(pc, 'use_disk_cache', False):
            path=getattr(pc, 'filepath', '')
            if path: deps.add(bpy.path.abspath(path))
print('BLENDNET_PROJECT:'+json.dumps({'engine':'CYCLES' if s.render.engine=='CYCLES' else 'EEVEE' if s.render.engine.startswith('BLENDER_EEVEE') else s.render.engine,
 'start':s.frame_start,'end':s.frame_end,'step':s.frame_step,
 'width':s.render.resolution_x*s.render.resolution_percentage//100,
 'height':s.render.resolution_y*s.render.resolution_percentage//100,
 'format':s.render.image_settings.file_format,'version':'.'.join(map(str,bpy.app.version)),
 'dependencies':sorted(os.path.abspath(p) for p in deps if p)}),flush=True)
)PY";
constexpr auto portable = R"PY(
import bpy, sys, os
target=sys.argv[sys.argv.index('--')+1]
bpy.ops.file.make_paths_relative()
bpy.ops.wm.save_as_mainfile(filepath=target, copy=True, relative_remap=False)
)PY";
bool within(const std::filesystem::path& root, const std::filesystem::path& path) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}
struct Cleanup {
    std::filesystem::path staging, archive;
    bool keep = false;
    ~Cleanup() {
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
        if (!keep)
            std::filesystem::remove(archive, ignored);
    }
};
} // namespace
std::vector<ProjectEntry>
ProjectLibrary::scan(const std::vector<std::filesystem::path>& directories,
                     const std::vector<std::filesystem::path>& hidden,
                     const std::atomic_bool* cancelled) {
    std::map<std::filesystem::path, ProjectEntry> found;
    std::set<std::filesystem::path> excluded;
    for (const auto& path : hidden)
        excluded.insert(std::filesystem::weakly_canonical(path));
    for (const auto& directory : directories) {
        if (stopped(cancelled))
            break;
        if (!std::filesystem::is_directory(directory))
            continue;
        for (auto it = std::filesystem::recursive_directory_iterator(
                 directory, std::filesystem::directory_options::skip_permission_denied);
             it != decltype(it){}; ++it) {
            if (stopped(cancelled))
                return {};
            if (it.depth() > 32 || it->is_symlink() ||
                it->path().filename().string().starts_with('.')) {
                if (it->is_directory())
                    it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file() || it->path().extension() != ".blend")
                continue;
            const auto path = std::filesystem::canonical(it->path());
            if (!excluded.contains(path))
                found[path] = {text(path.stem()), path, it->last_write_time(), it->file_size()};
            if (found.size() >= 10000)
                break;
        }
    }
    std::vector<ProjectEntry> result;
    for (auto& [path, entry] : found)
        result.push_back(std::move(entry));
    return result;
}
blender::BlenderResult<ProjectInfo> ProjectLibrary::inspect(const blender::VerifiedBlender& binary,
                                                            const std::filesystem::path& scene,
                                                            const std::atomic_bool* cancelled) {
    using blender::BlenderError;
    try {
        if (scene.extension() != ".blend" || !std::filesystem::is_regular_file(scene) ||
            std::filesystem::is_symlink(scene))
            return {{}, BlenderError::InvalidRequest};
        std::optional<ProjectInfo> info;
        const auto path = std::filesystem::canonical(scene);
        const auto result = blender::detail::runBlender(
            binary,
            {"--factory-startup", "--disable-autoexec", "-noaudio", "-b", text(path),
             "--python-exit-code", "72", "--python-expr", inspection},
            cancelled, std::chrono::minutes(2), [&](std::string_view line, bool) {
                constexpr std::string_view prefix = "BLENDNET_PROJECT:";
                if (!line.starts_with(prefix))
                    return;
                const auto data = nlohmann::json::parse(line.substr(prefix.size()));
                ProjectInfo p;
                p.entry = {text(path.stem()), path, std::filesystem::last_write_time(path),
                           std::filesystem::file_size(path)};
                p.engine = data.at("engine");
                p.format = data.at("format");
                p.version = data.at("version");
                p.start = data.at("start");
                p.end = data.at("end");
                p.step = data.at("step");
                p.width = data.at("width");
                p.height = data.at("height");
                p.estimatedBytes = p.entry.bytes;
                for (const auto& dep : data.at("dependencies")) {
                    const auto dependency = pathFromUtf8(dep.get<std::string>());
                    p.dependencies.push_back(dependency);
                    if (std::filesystem::is_regular_file(dependency))
                        p.estimatedBytes += std::filesystem::file_size(dependency);
                }
                if (!jobs::FrameRange{p.start, p.end, p.step}.count() || p.width < 1 ||
                    p.height < 1)
                    throw std::runtime_error("Invalid project metadata");
                info = std::move(p);
            });
        if (result.error != BlenderError::None)
            return {{}, result.error};
        if (!info)
            return {{}, BlenderError::ProcessFailed};
        return {std::move(info), BlenderError::None};
    } catch (const std::exception&) {
        return {{}, BlenderError::IoError};
    }
}
blender::BlenderError ProjectLibrary::package(const blender::VerifiedBlender& binary,
                                              const ProjectInfo& p,
                                              const std::filesystem::path& destination,
                                              const std::atomic_bool* cancelled) {
    using blender::BlenderError;
    try {
        const auto root = std::filesystem::canonical(p.entry.path.parent_path());
        for (const auto& dep : p.dependencies)
            if (!std::filesystem::exists(dep) || !within(root, std::filesystem::canonical(dep)))
                return BlenderError::InvalidRequest;
        if (std::filesystem::exists(destination) ||
            within(root, std::filesystem::absolute(destination)))
            return BlenderError::InvalidRequest;
        if (!archive::ZipService::privateDirectory(destination.parent_path()))
            return BlenderError::IoError;
        auto stage = destination;
        stage += ".stage";
        if (std::filesystem::exists(stage) || !archive::ZipService::privateDirectory(stage))
            return BlenderError::IoError;
        Cleanup cleanup{stage, destination};
        std::set<std::filesystem::path> inputs{std::filesystem::canonical(p.entry.path)};
        for (const auto& dep : p.dependencies) {
            if (std::filesystem::is_symlink(dep))
                return BlenderError::ArchiveRejected;
            if (std::filesystem::is_directory(dep)) {
                for (auto it = std::filesystem::recursive_directory_iterator(dep);
                     it != decltype(it){}; ++it) {
                    if (stopped(cancelled))
                        return BlenderError::Cancelled;
                    if (it.depth() > 32 || it->is_symlink())
                        return BlenderError::ArchiveRejected;
                    if (it->is_regular_file())
                        inputs.insert(std::filesystem::canonical(it->path()));
                    if (inputs.size() > 99998)
                        return BlenderError::ArchiveRejected;
                }
            } else
                inputs.insert(std::filesystem::canonical(dep));
        }
        std::vector<std::pair<std::string, std::filesystem::path>> files;
        std::uint64_t total = 0;
        for (const auto& path : inputs) {
            if (stopped(cancelled))
                return BlenderError::Cancelled;
            if (!within(root, path) || std::filesystem::is_symlink(path) ||
                !std::filesystem::is_regular_file(path))
                return BlenderError::ArchiveRejected;
            const auto bytes = std::filesystem::file_size(path);
            if (files.size() >= 99998 || bytes > 2ULL * 1024 * 1024 * 1024 ||
                bytes > 8ULL * 1024 * 1024 * 1024 - total)
                return BlenderError::ArchiveRejected;
            total += bytes;
            const auto relative = path.lexically_relative(root);
            if (relative == "blendnet-project.json")
                return BlenderError::ArchiveRejected;
            auto input = path;
            if (path.extension() == ".blend") {
                input = stage / relative;
                if (!archive::ZipService::privateDirectory(input.parent_path()))
                    return BlenderError::IoError;
                const auto result = blender::detail::runBlender(
                    binary,
                    {"--factory-startup", "--disable-autoexec", "-noaudio", "-b", text(path),
                     "--python-exit-code", "72", "--python-expr", portable, "--", text(input)},
                    cancelled, std::chrono::minutes(2));
                if (result.error != BlenderError::None)
                    return result.error;
            }
            files.emplace_back(text(relative), input);
        }
        auto* zip = zip_open(text(destination).c_str(), ZIP_CREATE | ZIP_EXCL, nullptr);
        if (!zip)
            return BlenderError::IoError;
        std::unique_ptr<zip_t, decltype(&zip_discard)> handle(zip, zip_discard);
        zip_register_cancel_callback_with_state(
            zip,
            [](zip_t*, void* flag) {
                return stopped(static_cast<const std::atomic_bool*>(flag)) ? 1 : 0;
            },
            nullptr, const_cast<std::atomic_bool*>(cancelled));
        const auto manifest =
            nlohmann::json{{"schemaVersion", 1}, {"blendPath", text(p.entry.path.filename())}}
                .dump();
        for (const auto& [name, path] : files) {
            if (name == "blendnet-project.json")
                continue;
            auto* source = zip_source_file(zip, text(path).c_str(), 0, ZIP_LENGTH_TO_END);
            if (!source)
                return BlenderError::IoError;
            if (zip_file_add(zip, name.c_str(), source, ZIP_FL_ENC_UTF_8) < 0) {
                zip_source_free(source);
                return BlenderError::ArchiveRejected;
            }
        }
        auto* source = zip_source_buffer(zip, manifest.data(), manifest.size(), 0);
        if (!source)
            return BlenderError::IoError;
        if (zip_file_add(zip, "blendnet-project.json", source, 0) < 0) {
            zip_source_free(source);
            return BlenderError::IoError;
        }
        if (zip_close(zip) < 0)
            return stopped(cancelled) ? BlenderError::Cancelled : BlenderError::IoError;
        handle.release();
        // Apply the same receiver policy before publication, including names and scene selection.
        const auto checked =
            archive::ZipService::extractProject(destination, stage / "check", {}, cancelled);
        if (!checked)
            return stopped(cancelled) ? BlenderError::Cancelled : BlenderError::ArchiveRejected;
        cleanup.keep = true;
        return BlenderError::None;
    } catch (const std::exception&) {
        return BlenderError::IoError;
    }
}
} // namespace blendnet::projects
