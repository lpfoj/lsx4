// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/config.h"
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"

#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

#include <miniz.h>

#include <algorithm>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>

namespace {

std::mutex submit_mutex{};
std::condition_variable_any request_cv{};
std::queue<std::packaged_task<void()>> req_queue{};

mz_zip_archive zip_ar{};
bool ar_is_read_only{true};

}

namespace Storage {

void ProcessIO(const std::stop_token& stoken) {
    Common::SetCurrentThreadName("shadPS4:PipelineCacheIO");

    while (true) {
        std::packaged_task<void()> request{};
        {
            std::unique_lock lock{submit_mutex};
            Common::CondvarWait(request_cv, lock, stoken,
                                [] { return !req_queue.empty(); });
            // Stop closes admission through DataBase::opened, but every write
            // accepted before Close must reach disk. Exit only after draining
            // the already-published queue.
            if (req_queue.empty()) {
                if (stoken.stop_requested()) {
                    break;
                }
                continue;
            }
            request = std::move(req_queue.front());
            req_queue.pop();
        }

        if (request.valid()) {
            request();
        }
    }
}

constexpr std::string GetBlobFileExtension(BlobType type) {
    switch (type) {
    case BlobType::ShaderMeta: {
        return "meta";
    }
    case BlobType::ShaderBinary: {
        return "spv";
    }
    case BlobType::ShaderBinaryStatus: {
        return "status";
    }
    case BlobType::PipelineKey: {
        return "key";
    }
    case BlobType::ShaderProfile: {
        return "bin";
    }
    default:
        UNREACHABLE();
    }
}

constexpr bool IsShareableShaderArtifact(const BlobType type) {
    return type == BlobType::ShaderMeta || type == BlobType::ShaderBinary;
}

void DataBase::Open(const std::string_view pipeline_storage_namespace,
                    const std::string_view shader_storage_namespace,
                    const std::span<const u8> shader_storage_identity) {
    if (opened.load()) {
        return;
    }

    const auto& game_info = Common::ElfInfo::Instance();

    using namespace Common::FS;
    ASSERT_MSG(!pipeline_storage_namespace.empty(),
               "Pipeline cache namespace must not be empty");
    ASSERT_MSG(!shader_storage_namespace.empty(),
               "Shader cache namespace must not be empty");
    ASSERT_MSG(!shader_storage_identity.empty(),
               "Shader cache identity must not be empty");
    const auto game_cache_path =
        GetUserPath(PathType::CacheDir) / game_info.GameSerial();
    if (Config::isPipelineCacheArchived()) {
        ar_is_read_only = true;
        mz_zip_zero_struct(&zip_ar);

        std::filesystem::create_directories(game_cache_path);
        cache_path = game_cache_path /
                     std::filesystem::path{std::string{pipeline_storage_namespace}}
                         .replace_extension(".zip");
        // miniz currently owns one archive handle. Keep archived caches fully
        // driver-scoped until storage supports two independently writable
        // archives; this is conservative and preserves the old archive model.
        shader_cache_path = cache_path;

        if (!mz_zip_reader_init_file(&zip_ar, cache_path.string().c_str(),
                                     MZ_ZIP_FLAG_READ_ALLOW_WRITING) ||
            !mz_zip_validate_archive(&zip_ar, 0)) {
            LOG_INFO(Render, "Cache archive {} is not found or archive is corrupted",
                     cache_path.string().c_str());
            mz_zip_reader_end(&zip_ar);
            mz_zip_writer_init_file(&zip_ar, cache_path.string().c_str(), 0);
        }
    } else {
        cache_path = game_cache_path / pipeline_storage_namespace;
        if (!std::filesystem::exists(cache_path)) {
            std::filesystem::create_directories(cache_path);
        }

        shader_cache_path = game_cache_path / shader_storage_namespace;
        if (!std::filesystem::exists(shader_cache_path)) {
            std::filesystem::create_directories(shader_cache_path);
        }

        // The directory name is a compact fingerprint. Keep the canonical
        // identity beside it so a collision, stale partial migration, or
        // manual cache move fails closed instead of loading foreign SPIR-V.
        const auto identity_path = shader_cache_path / "identity.bin";
        const bool identity_exists = std::filesystem::exists(identity_path);
        bool identity_matches = false;
        if (identity_exists) {
            const IOFile identity_file{identity_path, FileAccessMode::Read};
            if (identity_file.IsOpen() &&
                identity_file.GetSize() == shader_storage_identity.size()) {
                std::vector<u8> cached_identity(shader_storage_identity.size());
                identity_matches =
                    identity_file.Read(cached_identity) == cached_identity.size() &&
                    std::ranges::equal(cached_identity, shader_storage_identity);
            }
        } else {
            bool directory_is_empty = true;
            std::error_code directory_error;
            const auto first_entry =
                std::filesystem::directory_iterator{shader_cache_path, directory_error};
            directory_is_empty =
                !directory_error &&
                first_entry == std::filesystem::directory_iterator{};
            if (directory_is_empty) {
                IOFile identity_file{identity_path, FileAccessMode::Create};
                identity_matches =
                    identity_file.IsOpen() &&
                    identity_file.Write(shader_storage_identity) == shader_storage_identity.size() &&
                    identity_file.Commit();
            }
        }
        if (!identity_matches) {
            LOG_ERROR(Render,
                      "Shader cache identity mismatch for {}. Falling back to the "
                      "driver-scoped cache",
                      shader_cache_path.string());
            shader_cache_path = cache_path;
        }
    }

    pipeline_shader_fallback_enabled = false;
    io_worker = std::jthread{ProcessIO};
    opened.store(true);
}

void DataBase::EnablePipelineShaderFallback() {
    // This is enabled only after PipelineCache has compared the complete
    // profile stored in the driver namespace. It deliberately never points at
    // the pre-namespace legacy root, whose files may have mixed provenance.
    pipeline_shader_fallback_enabled =
        !Config::isPipelineCacheArchived() && shader_cache_path != cache_path;
}

void DataBase::Close() {
    {
        // Closing admission and publishing a write use the same mutex. Once
        // this store is visible, no Save can append behind the drain point.
        std::scoped_lock lock{submit_mutex};
        if (!opened.load()) {
            return;
        }
        opened.store(false);
    }

    io_worker.request_stop();
    io_worker.join();

    if (Config::isPipelineCacheArchived()) {
        mz_zip_writer_finalize_archive(&zip_ar);
        mz_zip_writer_end(&zip_ar);
        ar_is_read_only = true;
    }

    pipeline_shader_fallback_enabled = false;
    LOG_INFO(Render, "Cache dumped");
}

template <typename T>
bool WriteVector(const std::atomic_bool& opened, const BlobType type,
                 std::filesystem::path&& path_, std::vector<T>&& v) {
    // Preserve the cheap rejection path once storage is already closed. The
    // check under submit_mutex below is still the authoritative admission.
    if (!opened.load()) {
        return false;
    }

    {
        std::scoped_lock lock{submit_mutex};
        // This check and publication are the Save admission point. Close uses
        // submit_mutex too, so an accepted request is always ahead of Close's
        // drain, while a later request is rejected instead of being stranded.
        if (!opened.load()) {
            return false;
        }

        // The caller relinquishes both values to the background writer. A
        // default copy capture of an rvalue-reference parameter duplicated complete SPIR-V and
        // pipeline blobs synchronously on the render/compile path, exactly when
        // a new room creates pipelines. Move them into the queued task instead.
        auto request = std::packaged_task<void()>{
            [type, path = std::move(path_), data = std::move(v)]() mutable {
                path.replace_extension(GetBlobFileExtension(type));
                if (Config::isPipelineCacheArchived()) {
                    ASSERT_MSG(!ar_is_read_only,
                               "The archive is read-only. Did you forget to call `FinishPreload`?");
                    if (!mz_zip_writer_add_mem(&zip_ar, path.string().c_str(), data.data(),
                                               data.size() * sizeof(T), MZ_BEST_COMPRESSION)) {
                        LOG_ERROR(Render, "Failed to add {} to the archive",
                                  path.string().c_str());
                    }
                } else {
                    using namespace Common::FS;
                    const auto file = IOFile{path, FileAccessMode::Create};
                    file.Write(data);
                }
            }};
        req_queue.emplace(std::move(request));
    }

    request_cv.notify_one();
    return true;
}

template <typename T>
bool LoadVector(BlobType type, std::filesystem::path path, std::vector<T>& v) {
    using namespace Common::FS;
    path.replace_extension(GetBlobFileExtension(type));
    if (Config::isPipelineCacheArchived()) {
        int index{-1};
        index = mz_zip_reader_locate_file(&zip_ar, path.string().c_str(), nullptr, 0);
        if (index < 0) {
            LOG_WARNING(Render, "File {} is not found in the archive", path.string().c_str());
            return false;
        }
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip_ar, index, &stat) ||
            stat.m_uncomp_size % sizeof(T) != 0) {
            return false;
        }
        v.resize(stat.m_uncomp_size / sizeof(T));
        if (!mz_zip_reader_extract_to_mem(&zip_ar, index, v.data(), stat.m_uncomp_size, 0)) {
            v.clear();
            return false;
        }
    } else {
        const auto file = IOFile{path, FileAccessMode::Read};
        if (!file.IsOpen() || file.GetSize() % sizeof(T) != 0) {
            return false;
        }
        v.resize(file.GetSize() / sizeof(T));
        if (file.Read(v) != v.size()) {
            v.clear();
            return false;
        }
    }
    return true;
}

const std::filesystem::path& BlobBasePath(
    const BlobType type, const std::filesystem::path& pipeline_path,
    const std::filesystem::path& shader_path) {
    return IsShareableShaderArtifact(type) ? shader_path : pipeline_path;
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u8>&& data) {
    const auto& base_path = BlobBasePath(type, cache_path, shader_cache_path);
    auto path = Config::isPipelineCacheArchived() ? std::filesystem::path{name} : base_path / name;
    return WriteVector(opened, type, std::move(path), std::move(data));
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u32>&& data) {
    const auto& base_path = BlobBasePath(type, cache_path, shader_cache_path);
    auto path = Config::isPipelineCacheArchived() ? std::filesystem::path{name} : base_path / name;
    return WriteVector(opened, type, std::move(path), std::move(data));
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u8>& data) {
    if (!opened.load()) {
        return;
    }

    const auto& base_path = BlobBasePath(type, cache_path, shader_cache_path);
    auto path = Config::isPipelineCacheArchived() ? std::filesystem::path{name} : base_path / name;
    if (LoadVector(type, path, data) ||
        !pipeline_shader_fallback_enabled ||
        !IsShareableShaderArtifact(type)) {
        return;
    }
    if (LoadVector(type, cache_path / name, data)) {
        // Safe one-time migration from the already profile-validated driver
        // namespace. Writes remain in the shared shader namespace.
        Save(type, name, std::vector<u8>{data});
    }
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u32>& data) {
    if (!opened.load()) {
        return;
    }

    const auto& base_path = BlobBasePath(type, cache_path, shader_cache_path);
    auto path = Config::isPipelineCacheArchived() ? std::filesystem::path{name} : base_path / name;
    if (LoadVector(type, path, data) ||
        !pipeline_shader_fallback_enabled ||
        !IsShareableShaderArtifact(type)) {
        return;
    }
    if (LoadVector(type, cache_path / name, data)) {
        Save(type, name, std::vector<u32>{data});
    }
}

void DataBase::ForEachBlob(BlobType type, const std::function<bool(std::vector<u8>&& data)>& func) {
    const auto& ext = GetBlobFileExtension(type);
    if (Config::isPipelineCacheArchived()) {
        const auto num_files = mz_zip_reader_get_num_files(&zip_ar);
        // New pipeline keys are the ones most likely to belong to the last
        // streamed gameplay area. Prefer them over old boot/menu entries when
        // startup warm-up has a bounded budget.
        for (int index = num_files; index-- > 0;) {
            std::array<char, MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE> file_name{};
            file_name.fill(0);
            mz_zip_reader_get_filename(&zip_ar, index, file_name.data(), file_name.size());
            if (std::string{file_name.data()}.ends_with(ext)) {
                mz_zip_archive_file_stat stat{};
                mz_zip_reader_file_stat(&zip_ar, index, &stat);
                std::vector<u8> data(stat.m_uncomp_size);
                mz_zip_reader_extract_to_mem(&zip_ar, index, data.data(), data.size(), 0);
                if (!func(std::move(data))) {
                    break;
                }
            }
        }
    } else {
        const auto& base_path = BlobBasePath(type, cache_path, shader_cache_path);
        std::vector<std::filesystem::directory_entry> files;
        for (const auto& file : std::filesystem::directory_iterator{base_path}) {
            if (file.path().extension().string().ends_with(ext)) {
                files.emplace_back(file);
            }
        }
        std::ranges::sort(files, [](const auto& lhs, const auto& rhs) {
            std::error_code lhs_error;
            std::error_code rhs_error;
            const auto lhs_time = lhs.last_write_time(lhs_error);
            const auto rhs_time = rhs.last_write_time(rhs_error);
            if (!lhs_error && !rhs_error && lhs_time != rhs_time) {
                return lhs_time > rhs_time;
            }
            return lhs.path().native() > rhs.path().native();
        });
        for (const auto& file_name : files) {
            using namespace Common::FS;
            const auto& file = IOFile{file_name, FileAccessMode::Read};
            if (file.IsOpen()) {
                std::vector<u8> data(file.GetSize());
                file.Read(data);
                if (!func(std::move(data))) {
                    break;
                }
            }
        }
    }
}

void DataBase::FinishPreload() {
    if (Config::isPipelineCacheArchived()) {
        mz_zip_writer_init_from_reader(&zip_ar, cache_path.string().c_str());
        ar_is_read_only = false;
    }
}

}
