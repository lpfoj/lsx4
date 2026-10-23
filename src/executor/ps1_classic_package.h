#pragma once
#include <filesystem>
namespace Executor::Ps1Classic::Detail {
inline bool IsPackageLayout(const std::filesystem::path& executable) {
    const auto app0=executable.parent_path();
    return executable.filename()=="eboot.bin" &&
        std::filesystem::is_regular_file(app0/"conf/PS1HD/package-ps4.conf") &&
        std::filesystem::is_directory(app0/"assets/PS1HD/bios") &&
        std::filesystem::is_directory(app0/"data");
}
}
