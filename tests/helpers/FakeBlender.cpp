#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

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
    const std::vector<std::string> args(argv + 1, argv + argc);
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
