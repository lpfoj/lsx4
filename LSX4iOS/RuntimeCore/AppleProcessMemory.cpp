// SPDX-License-Identifier: GPL-2.0-or-later
#include "executor/dynamic_translation/process_memory.h"

#include <algorithm>
#include <cstring>
#include <dlfcn.h>
#include <limits>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <unistd.h>

namespace Lsx4::Translation {
namespace {
bool ValidRange(std::uint64_t address, std::size_t bytes) noexcept {
    return address >= 0x10000 && bytes != 0 &&
           bytes <= std::numeric_limits<std::uint64_t>::max() - address;
}

struct ImageBounds { std::uint64_t first{}, last{}; };
ImageBounds OwningImageBounds() noexcept {
    static const ImageBounds bounds = [] {
        Dl_info info{};
        if (!dladdr(reinterpret_cast<const void*>(&ReadProcessGuestMemory), &info))
            return ImageBounds{};
        const auto* header = static_cast<const mach_header_64*>(info.dli_fbase);
        if (header->magic != MH_MAGIC_64) return ImageBounds{};
        std::intptr_t slide{};
        bool found = false;
        for (std::uint32_t i = 0; i < _dyld_image_count(); ++i) {
            if (_dyld_get_image_header(i) == reinterpret_cast<const mach_header*>(header)) {
                slide = _dyld_get_image_vmaddr_slide(i);
                found = true;
                break;
            }
        }
        if (!found) return ImageBounds{};
        ImageBounds result{UINT64_MAX, 0};
        const auto* command = reinterpret_cast<const load_command*>(header + 1);
        for (std::uint32_t i = 0; i < header->ncmds; ++i) {
            if (command->cmd == LC_SEGMENT_64) {
                const auto* segment = reinterpret_cast<const segment_command_64*>(command);
                // PAGEZERO is a reservation, not part of the loaded host image.
                if (segment->vmsize && segment->initprot != VM_PROT_NONE) {
                    const auto start = segment->vmaddr + slide;
                    result.first = std::min(result.first, start);
                    result.last = std::max(result.last, start + segment->vmsize);
                }
            }
            command = reinterpret_cast<const load_command*>(
                reinterpret_cast<const char*>(command) + command->cmdsize);
        }
        return result.last > result.first ? result : ImageBounds{};
    }();
    return bounds;
}
}

bool IsAddressRangeRepresentable(std::uint64_t address, std::size_t bytes) noexcept {
    return bytes == 0 || bytes - 1 <= UINT64_MAX - address;
}
bool QueryProcessImageBounds(std::uint64_t& first, std::uint64_t& last) noexcept {
    const auto bounds = OwningImageBounds();
    first = bounds.first;
    last = bounds.last;
    return last > first;
}
bool IsProcessImageMemory(std::uint64_t address, std::size_t bytes) noexcept {
    const auto bounds = OwningImageBounds();
    return ValidRange(address, bytes) && bounds.last > bounds.first &&
           address < bounds.last && address + bytes > bounds.first;
}

void* MapProcessMemoryBelow4GiB(std::size_t bytes) noexcept {
    const std::uint64_t page = getpagesize();
    if (!bytes || bytes > UINT32_MAX - page) return nullptr;
    const auto rounded = (bytes + page - 1) / page * page;
    // VM_FLAGS_FIXED without OVERWRITE never replaces an existing host mapping.
    // An iOS PAGEZERO reservation may make this impossible; the common stack
    // allocator then uses a full-width pointer. Never remove that reservation.
    for (std::uint64_t candidate = (UINT64_C(1) << 32) - rounded;
         candidate >= std::max<std::uint64_t>(rounded, 0x10000); candidate -= rounded) {
        vm_address_t address = candidate;
        if (vm_allocate(mach_task_self(), &address, rounded, VM_FLAGS_FIXED) == KERN_SUCCESS)
            return reinterpret_cast<void*>(address);
    }
    return nullptr;
}

bool ReadProcessGuestMemory(std::uint64_t address, void* destination,
                            std::size_t bytes) noexcept {
    if (!destination || !ValidRange(address, bytes)) return false;
    vm_size_t copied{};
    return vm_read_overwrite(mach_task_self(), address, bytes,
                             reinterpret_cast<vm_address_t>(destination), &copied) == KERN_SUCCESS &&
           copied == bytes;
}
bool WriteProcessGuestMemory(std::uint64_t address, const void* source,
                             std::size_t bytes) noexcept {
    if (!source || !ValidRange(address, bytes) || IsProcessImageMemory(address, bytes) ||
        bytes > std::numeric_limits<mach_msg_type_number_t>::max()) return false;
    return vm_write(mach_task_self(), address,
                    reinterpret_cast<vm_offset_t>(source),
                    static_cast<mach_msg_type_number_t>(bytes)) == KERN_SUCCESS;
}

// The dispatch entries are called after validation of the guest VMA. The shared
// page tracker may temporarily protect that VMA to observe CPU accesses to
// GPU-owned memory. Ordinary checked access APIs above remain non-faulting.
bool ReadProcessGuestMemoryWithFaultDispatch(std::uint64_t address, void* destination,
                                            std::size_t bytes) {
    if (!destination || !ValidRange(address, bytes)) return false;
    std::copy_n(reinterpret_cast<const std::byte*>(address), bytes,
                static_cast<std::byte*>(destination));
    return true;
}
bool WriteProcessGuestMemoryWithFaultDispatch(std::uint64_t address, const void* source,
                                             std::size_t bytes) {
    if (!source || !ValidRange(address, bytes) || IsProcessImageMemory(address, bytes))
        return false;
    // A Mach vm_write fails on watched pages without delivering SIGBUS/SIGSEGV
    // to this process. Match the common host contract: let the installed owner
    // handle the synchronous write fault and retry the actual store.
    std::copy_n(static_cast<const std::byte*>(source), bytes,
                reinterpret_cast<std::byte*>(address));
    return true;
}
bool ReadProcessGuestScalar(std::uint64_t address, std::uint64_t& value) noexcept {
    value = 0;
    return ReadProcessGuestMemory(address, &value, sizeof(value));
}
bool WriteProcessGuestScalar(std::uint64_t address, std::uint64_t value) noexcept {
    return WriteProcessGuestMemory(address, &value, sizeof(value));
}
}
