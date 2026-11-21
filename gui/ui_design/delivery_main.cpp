// Command-line U3 workflow, independent of the VGUI/SDL editor host.
#include "delivery.h"

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace {
namespace fs = std::filesystem;
namespace ui = vgui::ui_design;

void usage(FILE* file)
{
    std::fputs("Usage:\n"
        "  vgui_ui_pack export <design.vgui.json> <new-delivery-directory>\n"
        "  vgui_ui_pack export --single-json <design.vgui.json> <new.vgui.package.json>\n"
        "  vgui_ui_pack verify <delivery-directory-or-file>\n", file);
}

int failure(const ui::diagnostic& error)
{
    if (error.path.empty()) std::fprintf(stderr, "vgui_ui_pack: %s\n", error.message.c_str());
    else std::fprintf(stderr, "vgui_ui_pack: %s: %s\n", error.path.c_str(), error.message.c_str());
    return 1;
}

template<class Char>
int run(int argc, Char** argv)
{
    try {
        if (argc == 2 && fs::path(argv[1]) == "--help") {
            usage(stdout);
            return 0;
        }
        ui::diagnostic error;
        ui::document doc;
        const bool single_json = argc == 5 && fs::path(argv[2]) == "--single-json";
        if ((argc == 4 || single_json) && fs::path(argv[1]) == "export") {
            const auto source = fs::absolute(fs::path(argv[single_json ? 3 : 2]));
            if (!fs::is_regular_file(source)) return failure({{}, "source is not a regular design file"});
            if (!ui::read_document(source, doc, &error)) return failure(error);
            const fs::path destination(argv[single_json ? 4 : 3]);
            if (single_json) {
                if (!ui::export_json_delivery(doc, source.parent_path(), destination, &error)) return failure(error);
                std::printf("Exported %zu nodes to a standalone package.\n", doc.nodes.size());
                return 0;
            }
            if (!ui::export_delivery(doc, source.parent_path(), destination, &error)) return failure(error);
            std::printf("Exported %zu nodes and %zu resources.\n", doc.nodes.size(), doc.resources.size());
            return 0;
        }
        if (argc == 3 && fs::path(argv[1]) == "verify") {
            const fs::path package(argv[2]);
            if (fs::is_directory(package)) {
                if (!ui::read_delivery(package, doc, &error)) return failure(error);
            } else {
                ui::embedded_delivery embedded;
                if (!ui::read_json_delivery(package, embedded, &error)) return failure(error);
                doc = std::move(embedded.design);
            }
            std::printf("Verified %zu nodes and %zu resources.\n", doc.nodes.size(), doc.resources.size());
            return 0;
        }
        usage(stderr);
        return 2;
    } catch (const std::exception& error) {
        return failure({{}, error.what()});
    }
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) { return run(argc, argv); }
#else
int main(int argc, char** argv) { return run(argc, argv); }
#endif
