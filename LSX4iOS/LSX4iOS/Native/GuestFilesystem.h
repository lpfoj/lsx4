// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "GuestAddressSpace.h"
#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace Lsx4::Apple {
// Every descriptor belongs to this guest session. Paths are resolved from an
// opened mount directory with no symlink traversal; guest integers are never
// used as host descriptors or pointers.
class GuestFilesystem {
public:
    explicit GuestFilesystem(const std::filesystem::path& game = {});
    ~GuestFilesystem();
    GuestFilesystem(const GuestFilesystem&) = delete;
    GuestFilesystem& operator=(const GuestFilesystem&) = delete;
    bool MountDirectory(std::string name,int directory,bool writable);
    bool UnmountDirectory(std::string_view name);
    std::optional<std::uint64_t> Call(const std::string& identity,
        std::uint64_t a,std::uint64_t b,std::uint64_t c,std::uint64_t d,GuestAddressSpace& memory);
    std::uint64_t PosixWrite(std::uint64_t fd,std::uint64_t buffer,std::uint64_t size,
                             std::uint64_t errnoAddress,GuestAddressSpace& memory);
    std::string consoleOutput;
    std::uint64_t consoleBytes{};
    std::vector<std::string> openedPaths, attemptedPaths;
    std::uint64_t bytesRead{},bytesWritten{};
private:
    struct Mount {int descriptor{-1};bool writable{};};
    std::map<std::string,Mount> mounts_;
    std::map<int,int> descriptors_;
    std::map<int,std::string> descriptorMounts_;
    int Open(const std::string& path,int flags,unsigned mode);
    int ReadPath(GuestAddressSpace& memory,std::uint64_t address,std::string& out);
};
}
