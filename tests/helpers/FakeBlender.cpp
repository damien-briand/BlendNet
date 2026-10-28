#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

namespace {
constexpr unsigned char png[] = {137, 80,  78,  71,  13, 10, 26, 10, 0,   0,  0,   13,  73,  72,
                                 68,  82,  0,   0,   0,  1,  0,  0,  0,   1,  8,   4,   0,   0,
                                 0,   181, 28,  12,  2,  0,  0,  0,  11,  73, 68,  65,  84,  120,
                                 218, 99,  252, 255, 31, 0,  3,  3,  2,   0,  239, 162, 167, 91,
                                 0,   0,   0,   0,   73, 69, 78, 68, 174, 66, 96,  130};
std::string option(const std::vector<std::string>& args, const std::string& flag) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == flag)
            return args[i + 1];
    return {};
}
std::filesystem::path pathFromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
} // namespace
int main(int argc, char** argv) {
    std::string executable = argv[0];
    std::vector<std::string> args;
#ifdef _WIN32
    int wideCount = 0;
    auto** wideArguments = CommandLineToArgvW(GetCommandLineW(), &wideCount);
    if (!wideArguments)
        return 2;
    for (int i = 0; i < wideCount; ++i) {
        const auto text = std::filesystem::path(wideArguments[i]).u8string();
        const std::string utf8(text.begin(), text.end());
        if (i == 0)
            executable = utf8;
        else
            args.push_back(utf8);
    }
    LocalFree(wideArguments);
#else
    args.assign(argv + 1, argv + argc);
#endif
    const bool systemPython = std::find(args.begin(), args.end(), "--python-use-system-env") != args.end();
    if (std::find(args.begin(), args.end(), "startup-policy") != args.end()) {
        std::cout << "SYSTEM_ENV=" << systemPython << '\n';
        for (const auto* key : {"PYTHONDONTWRITEBYTECODE", "PYTHONNOUSERSITE", "PYTHONPYCACHEPREFIX",
                                "PYTHONPATH", "PYTHONHOME", "PYTHONINSPECT", "PYTHONBLENDNET_INHERITED",
                                "_PYTHON_PROJECT_BASE"}) {
            const auto* value = std::getenv(key);
            std::cout << key << '=' << (value ? value : "<unset>") << '\n';
        }
        return 0;
    }
    // Model Blender's startup imports, before any --python-expr script executes.
    const auto module = pathFromUtf8(executable).parent_path() / "blendnet_startup_probe.py";
    const auto* noBytecode = std::getenv("PYTHONDONTWRITEBYTECODE");
    if (std::filesystem::exists(module) &&
        (!systemPython || !noBytecode || std::string(noBytecode) != "1")) {
        const auto cache = module.parent_path() / "__pycache__";
        std::filesystem::create_directory(cache);
        std::ofstream(cache / "blendnet_startup_probe.pyc", std::ios::binary) << "startup bytecode";
    }
    if (!args.empty() && args[0] == "environment") {
        const auto* temp = std::getenv("TEMP");
        const auto* root = std::getenv("SystemRoot");
        std::cout << "TEMP=" << (temp ? temp : "") << '\n'
                  << "SYSTEM_ROOT=" << (root ? root : "") << '\n';
        return 0;
    }
    if (!args.empty() && args[0] == "--version") {
        std::cout << "Blender " << (pathFromUtf8(executable).stem() == "wrong-version" ? "4.3.0" : "4.2.1") << '\n';
        return 0;
    }
    if (!args.empty() && args[0] == "close-streams") {
        std::fclose(stdout);
        std::fclose(stderr);
        std::this_thread::sleep_for(std::chrono::seconds(10));
        return 0;
    }
    if (!args.empty() && args[0] == "flood") {
        for (int i = 0; i < 128; ++i) {
            std::cout << std::string(8192, 'o') << '\n';
            std::cerr << std::string(8192, 'e') << '\n';
        }
        return 0;
    }
    const auto script = option(args, "--python-expr");
    if (script.find("BLENDNET_PROJECT:") != script.npos) {
        std::cout
            << "BLENDNET_PROJECT:{\"engine\":\"CYCLES\",\"start\":1,\"end\":3,\"step\":1,\"width\":"
               "1,\"height\":1,\"format\":\"PNG\",\"version\":\"4.2.1\",\"dependencies\":[]}\n";
        return 0;
    }
    if (script.find("BLENDNET_GPU:") != script.npos) {
        std::cout << "BLENDNET_GPU:{\"version\":\"4.2.1\",\"devices\":[{\"name\":\"Fixture "
                     "CPU\",\"backend\":\"CPU\"},{\"name\":\"Fixture "
                     "GPU\",\"backend\":\"CUDA\"}],\"eeveeEngine\":\"BLENDER_EEVEE_NEXT\"}\n";
        return 0;
    }
    if (script.find("save_as_mainfile") != script.npos) {
        const auto path = pathFromUtf8(option(args, "--"));
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << "Fixture scene";
        return 0;
    }
    const auto scene = pathFromUtf8(option(args, "-b"));
    if (scene.filename() == "failure.blend") {
        std::cerr << "Simulated renderer failure\n";
        return 7;
    }
    if (scene.filename() == "sleep.blend") {
        std::cout << "Fra:1 | rendering\n" << std::flush;
        std::this_thread::sleep_for(std::chrono::seconds(10));
        return 0;
    }
    const int start = std::stoi(option(args, "-s")), end = std::stoi(option(args, "-e")),
              step = std::stoi(option(args, "-j"));
    const auto output = pathFromUtf8(option(args, "-o")).parent_path();
    if (option(args, "--cycles-device").empty())
        return 9;
    std::filesystem::create_directories(output);
    for (int frame = start; frame <= end; frame += step) {
        if (scene.filename() == "missing.blend" && frame != start)
            continue;
        char name[64]{};
        std::snprintf(name, sizeof(name), "frame_%04d.png", frame);
        const auto target = output / name;
        std::ofstream file(target, std::ios::binary);
        file.write(reinterpret_cast<const char*>(png), sizeof(png));
        file.close();
        std::cout << "Fr" << std::flush;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::cout << "a:" << frame << " | renderer started\nSaved: '" << target.string() << "'\n"
                  << std::flush;
    }
    return 0;
}
