//////////////////////////////////////////////////////////////////////

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>
#include <map>
#include <string>

#include <nlohmann/json.hpp>

#include "util.h"
#include "step_model.h"
#include "model_check.h"

namespace
{
    std::string utf8(std::filesystem::path const &path)
    {
        std::u8string s = path.generic_u8string();
        return { s.begin(), s.end() };
    }

    std::string lower_extension(std::filesystem::path const &path)
    {
        std::string extension = path.extension().string();
        if(!extension.empty()) {
            extension.erase(0, 1);
        }
        std::transform(extension.begin(), extension.end(), extension.begin(), [](char c) { return (char)std::tolower((unsigned char)c); });
        return extension;
    }

    //////////////////////////////////////////////////////////////////////
    // what a model looks like, as far as the check is concerned

    nlohmann::json describe(std::filesystem::path const &file)
    {
        std::atomic<float> progress;
        auto result = load_step_model(file, std::stop_token(), progress);
        if(!result.has_value()) {
            return { { "error", true } };
        }
        step_model const &model = *result.value();
        // to 0.01mm (through text, so it's exactly what would be written)
        auto size = [&](int i) { return std::stod(std::format("{:.2f}", model.extent_max[i] - model.extent_min[i])); };
        return {
            { "parts", model.parts.size() },
            { "triangles", model.indices.size() / 3 },
            { "materials", model.materials.size() },
            { "textures", model.textures.size() },
            { "realistic", model.has_pbr_materials },
            { "size", { size(0), size(1), size(2) } },
        };
    }

    //////////////////////////////////////////////////////////////////////
    // sizes can be a hair different (bounding box tolerances), everything else must match

    bool same(nlohmann::json const &expected, nlohmann::json const &actual, std::string &why)
    {
        if(expected.value("error", false) || actual.value("error", false)) {
            if(expected.value("error", false) != actual.value("error", false)) {
                why = expected.value("error", false) ? "should fail to load but it loaded" : "failed to load";
                return false;
            }
            return true;
        }
        for(char const *key : { "parts", "triangles", "materials", "textures", "realistic" }) {
            if(expected.value(key, nlohmann::json()) != actual.value(key, nlohmann::json())) {
                why = std::format("{} {} (expected {})", key, actual.value(key, nlohmann::json()).dump(), expected.value(key, nlohmann::json()).dump());
                return false;
            }
        }
        for(int i = 0; i < 3; ++i) {
            double e = expected["size"][i].get<double>();
            double a = actual["size"][i].get<double>();
            if(std::abs(e - a) > 0.02 + std::abs(e) * 1e-4) {
                why = std::format("size {} (expected {})", actual["size"].dump(), expected["size"].dump());
                return false;
            }
        }
        return true;
    }
}    // namespace

//////////////////////////////////////////////////////////////////////

int check_models(std::filesystem::path const &directory, bool update)
{
    std::vector<std::string> const supported = supported_file_extensions();

    std::map<std::string, std::filesystem::path> files;    // by name relative to the directory
    std::error_code error;
    for(auto const &entry : std::filesystem::recursive_directory_iterator(directory, error)) {
        if(entry.is_regular_file() && std::find(supported.begin(), supported.end(), lower_extension(entry.path())) != supported.end()) {
            files[utf8(std::filesystem::relative(entry.path(), directory))] = entry.path();
        }
    }
    if(error) {
        printf("Can't read %s: %s\n", utf8(directory).c_str(), error.message().c_str());
        return 1;
    }

    std::filesystem::path const expected_path = directory / "expected.json";
    nlohmann::json expected = nlohmann::json::object();
    if(!update) {
        std::ifstream stream(expected_path);
        if(!stream) {
            printf("No %s, run with --update to make it\n", utf8(expected_path).c_str());
            return 1;
        }
        expected = nlohmann::json::parse(stream, nullptr, false);
        if(expected.is_discarded() || !expected.is_object()) {
            printf("Can't parse %s\n", utf8(expected_path).c_str());
            return 1;
        }
    }

    nlohmann::json actual = nlohmann::json::object();
    int failures = 0;
    double start = get_time();

    for(auto const &[name, path] : files) {
        double t = get_time();
        actual[name] = describe(path);
        double seconds = get_time() - t;

        std::string summary = actual[name].value("error", false) ? std::string("doesn't load")
                                                                  : std::format("{} parts, {} triangles, size {}",
                                                                                actual[name]["parts"].get<int>(),
                                                                                actual[name]["triangles"].get<int>(),
                                                                                actual[name]["size"].dump());
        if(update) {
            printf("  %-48s %s (%.2fs)\n", name.c_str(), summary.c_str(), seconds);
        } else if(!expected.contains(name)) {
            printf("NEW   %-48s %s (not in expected.json)\n", name.c_str(), summary.c_str());
            failures += 1;
        } else {
            std::string why;
            if(same(expected[name], actual[name], why)) {
                printf("ok    %-48s %s (%.2fs)\n", name.c_str(), summary.c_str(), seconds);
            } else {
                printf("FAIL  %-48s %s\n", name.c_str(), why.c_str());
                failures += 1;
            }
        }
        fflush(stdout);
    }

    for(auto const &[name, value] : expected.items()) {
        if(!files.contains(name)) {
            printf("GONE  %s (in expected.json but not found)\n", name.c_str());
            failures += 1;
        }
    }

    if(update) {
        std::ofstream(expected_path, std::ios::binary) << actual.dump(2) << "\n";
        printf("Wrote %s (%zu files, %.1fs)\n", utf8(expected_path).c_str(), files.size(), get_time() - start);
        return 0;
    }
    printf("%zu files, %d failed (%.1fs)\n", files.size(), failures, get_time() - start);
    return failures == 0 ? 0 : 1;
}
