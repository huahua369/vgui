// Command-line U3 workflow, independent of the VGUI/SDL editor host.
#include "delivery.h"

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace {
namespace fs = std::filesystem;
namespace ui = vgui::ui_design;

void usage(FILE* file)
{
    std::fputs("Usage:\n"
        "  vgui_ui_pack export <design.vgui.json> <new-delivery-directory>\n"
        "  vgui_ui_pack verify <delivery-directory>\n", file);
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
        if (argc == 4 && fs::path(argv[1]) == "export") {
            const auto source = fs::absolute(fs::path(argv[2]));
            if (!fs::is_regular_file(source)) return failure({{}, "source is not a regular design file"});
            if (!ui::read_document(source, doc, &error) ||
                !ui::export_delivery(doc, source.parent_path(), fs::path(argv[3]), &error))
                return failure(error);
            std::printf("Exported %zu nodes and %zu resources.\n", doc.nodes.size(), doc.resources.size());
            return 0;
        }
        if (argc == 3 && fs::path(argv[1]) == "verify") {
            if (!ui::read_delivery(fs::path(argv[2]), doc, &error)) return failure(error);
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
