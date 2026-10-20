#if defined(__ANDROID__) && __ANDROID_API__ < 28

#include <glob.h>

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool IsEscaped(const std::string& text, size_t position) {
    size_t slashes = 0;
    while (position > slashes && text[position - slashes - 1] == '\\') {
        ++slashes;
    }
    return (slashes & 1U) != 0;
}

bool HasMagic(const std::string& pattern, bool no_escape) {
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (!no_escape && IsEscaped(pattern, i)) {
            continue;
        }
        if (pattern[i] == '*' || pattern[i] == '?' || pattern[i] == '[') {
            return true;
        }
    }
    return false;
}

bool ExpandOneBrace(const std::string& pattern, std::vector<std::string>* output) {
    size_t open = std::string::npos;
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '{' && !IsEscaped(pattern, i)) {
            open = i;
            break;
        }
    }
    if (open == std::string::npos) {
        output->push_back(pattern);
        return false;
    }

    int depth = 0;
    size_t close = std::string::npos;
    bool has_comma = false;
    for (size_t i = open; i < pattern.size(); ++i) {
        if (IsEscaped(pattern, i)) {
            continue;
        }
        if (pattern[i] == '{') {
            ++depth;
        } else if (pattern[i] == '}') {
            if (--depth == 0) {
                close = i;
                break;
            }
        } else if (pattern[i] == ',' && depth == 1) {
            has_comma = true;
        }
    }
    if (close == std::string::npos || !has_comma) {
        output->push_back(pattern);
        return false;
    }

    const std::string prefix = pattern.substr(0, open);
    const std::string suffix = pattern.substr(close + 1);
    size_t part_start = open + 1;
    depth = 0;
    for (size_t i = open + 1; i <= close; ++i) {
        const bool at_end = i == close;
        if (!at_end && !IsEscaped(pattern, i)) {
            if (pattern[i] == '{') {
                ++depth;
            } else if (pattern[i] == '}') {
                --depth;
            }
        }
        if (at_end || (pattern[i] == ',' && depth == 0 && !IsEscaped(pattern, i))) {
            const std::string expanded =
                prefix + pattern.substr(part_start, i - part_start) + suffix;
            std::vector<std::string> nested;
            ExpandOneBrace(expanded, &nested);
            output->insert(output->end(), nested.begin(), nested.end());
            part_start = i + 1;
        }
    }
    return true;
}

struct DirectoryHandle {
    glob_t* state{};
    void* handle{};
};

DirectoryHandle OpenDirectory(glob_t* state, const char* path) {
    DirectoryHandle result{state, nullptr};
#if defined(GLOB_ALTDIRFUNC)
    if ((state->gl_flags & GLOB_ALTDIRFUNC) != 0 && state->gl_opendir != nullptr) {
        result.handle = state->gl_opendir(path);
        return result;
    }
#endif
    result.handle = opendir(path);
    return result;
}

dirent* ReadDirectory(const DirectoryHandle& directory) {
#if defined(GLOB_ALTDIRFUNC)
    if ((directory.state->gl_flags & GLOB_ALTDIRFUNC) != 0 &&
        directory.state->gl_readdir != nullptr) {
        return directory.state->gl_readdir(directory.handle);
    }
#endif
    return readdir(static_cast<DIR*>(directory.handle));
}

void CloseDirectory(const DirectoryHandle& directory) {
    if (directory.handle == nullptr) {
        return;
    }
#if defined(GLOB_ALTDIRFUNC)
    if ((directory.state->gl_flags & GLOB_ALTDIRFUNC) != 0 &&
        directory.state->gl_closedir != nullptr) {
        directory.state->gl_closedir(directory.handle);
        return;
    }
#endif
    closedir(static_cast<DIR*>(directory.handle));
}

int StatPath(glob_t* state, const char* path, struct stat* status, bool follow) {
#if defined(GLOB_ALTDIRFUNC)
    if ((state->gl_flags & GLOB_ALTDIRFUNC) != 0) {
        if (follow && state->gl_stat != nullptr) {
            return state->gl_stat(path, status);
        }
        if (!follow && state->gl_lstat != nullptr) {
            return state->gl_lstat(path, status);
        }
    }
#endif
    return follow ? stat(path, status) : lstat(path, status);
}

std::vector<std::string> SplitComponents(const std::string& pattern) {
    std::vector<std::string> components;
    size_t begin = 0;
    while (begin < pattern.size()) {
        while (begin < pattern.size() && pattern[begin] == '/') {
            ++begin;
        }
        if (begin == pattern.size()) {
            break;
        }
        const size_t end = pattern.find('/', begin);
        components.push_back(pattern.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin));
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return components;
}

std::string JoinPath(const std::string& base, const std::string& name) {
    if (base.empty()) {
        return name;
    }
    if (base == "/") {
        return base + name;
    }
    return base + "/" + name;
}

int ReportDirectoryError(glob_t* state, const std::string& path, int error) {
    const bool callback_aborted =
        state->gl_errfunc != nullptr && state->gl_errfunc(path.c_str(), error) != 0;
    return (callback_aborted || (state->gl_flags & GLOB_ERR) != 0) ? GLOB_ABORTED : 0;
}

int WalkPattern(glob_t* state, const std::vector<std::string>& components,
                size_t index, const std::string& base, bool no_escape,
                std::vector<std::string>* matches) {
    if (index == components.size()) {
        struct stat status {};
        const std::string candidate = base.empty() ? "." : base;
        if (StatPath(state, candidate.c_str(), &status, false) != 0) {
            return 0;
        }
        std::string result = candidate;
        if ((state->gl_flags & GLOB_MARK) != 0 && S_ISDIR(status.st_mode) &&
            (result.empty() || result.back() != '/')) {
            result.push_back('/');
        }
        matches->push_back(std::move(result));
        return 0;
    }

    const std::string& component = components[index];
    if (!HasMagic(component, no_escape)) {
        return WalkPattern(state, components, index + 1, JoinPath(base, component),
                           no_escape, matches);
    }

    const std::string directory_path = base.empty() ? "." : base;
    errno = 0;
    DirectoryHandle directory = OpenDirectory(state, directory_path.c_str());
    if (directory.handle == nullptr) {
        return ReportDirectoryError(state, directory_path, errno);
    }

    const int match_flags = FNM_PERIOD | (no_escape ? FNM_NOESCAPE : 0);
    int result = 0;
    errno = 0;
    while (dirent* entry = ReadDirectory(directory)) {
        if (fnmatch(component.c_str(), entry->d_name, match_flags) != 0) {
            continue;
        }
        const std::string path = JoinPath(base, entry->d_name);
        if (index + 1 < components.size()) {
            struct stat status {};
            if (StatPath(state, path.c_str(), &status, true) != 0 ||
                !S_ISDIR(status.st_mode)) {
                continue;
            }
        }
        result = WalkPattern(state, components, index + 1, path, no_escape, matches);
        if (result != 0) {
            break;
        }
    }
    const int read_error = errno;
    CloseDirectory(directory);
    if (result == 0 && read_error != 0) {
        result = ReportDirectoryError(state, directory_path, read_error);
    }
    return result;
}

void FreePathVector(glob_t* state) {
    if (state->gl_pathv == nullptr) {
        return;
    }
    const size_t offset = (state->gl_flags & GLOB_DOOFFS) != 0 ? state->gl_offs : 0;
    for (size_t i = 0; i < state->gl_pathc; ++i) {
        free(state->gl_pathv[offset + i]);
    }
    free(state->gl_pathv);
    state->gl_pathv = nullptr;
    state->gl_pathc = 0;
    state->gl_matchc = 0;
}

}

extern "C" __attribute__((visibility("default"))) int glob(
    const char* pattern, int flags,
    int (*error_callback)(const char*, int), glob_t* state) {
    if (pattern == nullptr || state == nullptr) {
        errno = EINVAL;
        return GLOB_ABORTED;
    }

    const bool append = (flags & GLOB_APPEND) != 0;
    if (!append) {
        std::memset(state, 0, sizeof(*state));
    }
    state->gl_flags = flags;
    state->gl_errfunc = error_callback;
    const bool no_escape = (flags & GLOB_NOESCAPE) != 0;
    const bool magic = HasMagic(pattern, no_escape);
#if defined(GLOB_MAGCHAR)
    if (magic) {
        state->gl_flags |= GLOB_MAGCHAR;
    }
#endif

    std::vector<std::string> patterns;
#if defined(GLOB_BRACE)
    if ((flags & GLOB_BRACE) != 0) {
        ExpandOneBrace(pattern, &patterns);
    } else
#endif
    {
        patterns.emplace_back(pattern);
    }

    std::vector<std::string> matches;
    for (const std::string& expanded : patterns) {
        const bool absolute = !expanded.empty() && expanded.front() == '/';
        const std::vector<std::string> components = SplitComponents(expanded);
        const int result = WalkPattern(state, components, 0, absolute ? "/" : "",
                                       no_escape, &matches);
        if (result != 0) {
            return result;
        }
    }

    state->gl_matchc = matches.size();
    if (matches.empty()) {
        const bool return_pattern = (flags & GLOB_NOCHECK) != 0
#if defined(GLOB_NOMAGIC)
            || ((flags & GLOB_NOMAGIC) != 0 && !magic)
#endif
            ;
        if (!return_pattern) {
            return GLOB_NOMATCH;
        }
        matches.emplace_back(pattern);
    }
    if ((flags & GLOB_NOSORT) == 0) {
        std::sort(matches.begin(), matches.end());
    }

    const size_t old_count = append ? state->gl_pathc : 0;
    const size_t offset = (flags & GLOB_DOOFFS) != 0 ? state->gl_offs : 0;
    const size_t total_count = old_count + matches.size();
    char** new_paths = static_cast<char**>(
        calloc(offset + total_count + 1, sizeof(char*)));
    if (new_paths == nullptr) {
        return GLOB_NOSPACE;
    }

    if (append && state->gl_pathv != nullptr) {
        for (size_t i = 0; i < old_count; ++i) {
            new_paths[offset + i] = state->gl_pathv[offset + i];
        }
    }
    size_t copied = 0;
    for (; copied < matches.size(); ++copied) {
        new_paths[offset + old_count + copied] = strdup(matches[copied].c_str());
        if (new_paths[offset + old_count + copied] == nullptr) {
            for (size_t i = 0; i < copied; ++i) {
                free(new_paths[offset + old_count + i]);
            }
            free(new_paths);
            return GLOB_NOSPACE;
        }
    }
    if (append) {
        free(state->gl_pathv);
    }
    state->gl_pathv = new_paths;
    state->gl_pathc = total_count;
    return 0;
}

extern "C" __attribute__((visibility("default"))) void globfree(glob_t* state) {
    if (state != nullptr) {
        FreePathVector(state);
    }
}

#endif
