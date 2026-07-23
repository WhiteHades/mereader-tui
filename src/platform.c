#include "mereader-tui/platform.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static const int MEREADER_TUI_PLATFORM_EXIT_SIGNALS[] = {SIGINT, SIGTERM, SIGHUP};

int mereader_tui_platform_block_exit_signals(sigset_t *previous);

int mereader_tui_platform_block_exit_signals(sigset_t *previous) {
    if (previous == nullptr) {
        return EINVAL;
    }
    sigset_t signals;
    if (sigemptyset(&signals) != 0) {
        return errno;
    }
    for (size_t index = 0U; index < MEREADER_TUI_ARRAY_LEN(MEREADER_TUI_PLATFORM_EXIT_SIGNALS); ++index) {
        if (sigaddset(&signals, MEREADER_TUI_PLATFORM_EXIT_SIGNALS[index]) != 0) {
            return errno;
        }
    }
    return pthread_sigmask(SIG_BLOCK, &signals, previous);
}

static bool mereader_tui_platform_is_executable(const char *path) {
    struct stat status;
    return access(path, X_OK) == 0 && stat(path, &status) == 0 && !S_ISDIR(status.st_mode);
}

const char *mereader_tui_platform_find_executable(const char *name) {
    if (name == nullptr || name[0] == '\0') {
        return nullptr;
    }
    if (strchr(name, '/') != nullptr) {
        return mereader_tui_platform_is_executable(name) ? name : nullptr;
    }

    const char *path_value = getenv("PATH");
    if (path_value == nullptr) {
        path_value = "/bin:/usr/bin";
    }

    size_t name_length = strlen(name);
    const char *segment = path_value;
    for (const char *cursor = path_value;; ++cursor) {
        if (*cursor != ':' && *cursor != '\0') {
            continue;
        }

        size_t directory_length = (size_t)(cursor - segment);
        const char *directory = segment;
        if (directory_length == 0U) {
            directory = ".";
            directory_length = 1U;
        }
        if (name_length <= SIZE_MAX - 2U && directory_length <= SIZE_MAX - name_length - 2U) {
            size_t candidate_length = directory_length + name_length + 2U;
            char *candidate = malloc(candidate_length);
            if (candidate == nullptr) {
                return nullptr;
            }
            memcpy(candidate, directory, directory_length);
            candidate[directory_length] = '/';
            memcpy(candidate + directory_length + 1U, name, name_length + 1U);
            bool found = mereader_tui_platform_is_executable(candidate);
            free(candidate);
            if (found) {
                return name;
            }
        }

        if (*cursor == '\0') {
            break;
        }
        segment = cursor + 1;
    }
    return nullptr;
}

typedef struct MereaderTuiPlatformReaper {
    pid_t process;
} MereaderTuiPlatformReaper;

static void *mereader_tui_platform_reap(void *context) {
    MereaderTuiPlatformReaper *reaper = context;
    pid_t process = reaper->process;
    free(reaper);

    while (waitpid(process, nullptr, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        break;
    }
    return nullptr;
}

static void mereader_tui_platform_stop_unreaped(pid_t process) {
    (void)kill(process, SIGKILL);
    while (waitpid(process, nullptr, 0) < 0 && errno == EINTR) {
    }
}

static bool mereader_tui_platform_spawn_opener(const char *launcher, const char *target, MereaderTuiError *error) {
    char *regular_arguments[] = {(char *)launcher, (char *)target, nullptr};
    char *gio_arguments[] = {(char *)launcher, "open", (char *)target, nullptr};
    char **arguments = strcmp(launcher, "gio") == 0 ? gio_arguments : regular_arguments;

    MereaderTuiPlatformReaper *reaper = mereader_tui_reallocarray(nullptr, 1U, sizeof(*reaper), error);
    if (reaper == nullptr) {
        return false;
    }
    pthread_attr_t attributes;
    int status = pthread_attr_init(&attributes);
    if (status != 0) {
        free(reaper);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL, "could not initialize opener reaper: %s", strerror(status));
        return false;
    }
    status = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    if (status != 0) {
        (void)pthread_attr_destroy(&attributes);
        free(reaper);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL, "could not detach opener reaper: %s", strerror(status));
        return false;
    }

    pid_t process = 0;
    status = posix_spawnp(&process, launcher, nullptr, nullptr, arguments, environ);
    if (status != 0) {
        (void)pthread_attr_destroy(&attributes);
        free(reaper);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL, "could not start '%s': %s", launcher, strerror(status));
        return false;
    }

    reaper->process = process;
    /* The detached reaper inherits this mask before its first instruction. */
    sigset_t previous_signal_mask;
    status = mereader_tui_platform_block_exit_signals(&previous_signal_mask);
    if (status != 0) {
        (void)pthread_attr_destroy(&attributes);
        free(reaper);
        mereader_tui_platform_stop_unreaped(process);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL, "could not block opener reaper signals: %s", strerror(status));
        return false;
    }
    pthread_t reaper_thread;
    status = pthread_create(&reaper_thread, &attributes, mereader_tui_platform_reap, reaper);
    const int restore_status = pthread_sigmask(SIG_SETMASK, &previous_signal_mask, nullptr);
    (void)pthread_attr_destroy(&attributes);
    if (restore_status != 0) {
        if (status != 0) {
            free(reaper);
            mereader_tui_platform_stop_unreaped(process);
        }
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL, "could not restore opener signal mask: %s",
                       strerror(restore_status));
        return false;
    }
    if (status != 0) {
        free(reaper);
        mereader_tui_platform_stop_unreaped(process);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL, "could not create opener reaper: %s", strerror(status));
        return false;
    }
    return true;
}

bool mereader_tui_platform_open(const char *target, const char *preferred, MereaderTuiError *error) {
    if (target == nullptr || target[0] == '\0') {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "open target is empty");
        return false;
    }

#if defined(__APPLE__)
    const char *launchers[] = {nullptr, "open", "gio", "xdg-open"};
#else
    const char *launchers[] = {nullptr, "gio", "xdg-open"};
#endif
    if (preferred != nullptr && preferred[0] != '\0' && mereader_tui_casecmp(preferred, "auto") != 0) {
        launchers[0] = preferred;
    }

    for (size_t index = 0U; index < MEREADER_TUI_ARRAY_LEN(launchers); ++index) {
        const char *launcher = launchers[index];
        if (launcher == nullptr) {
            continue;
        }

        bool duplicate = false;
        for (size_t previous = 0U; previous < index; ++previous) {
            if (launchers[previous] != nullptr && strcmp(launchers[previous], launcher) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate || mereader_tui_platform_find_executable(launcher) == nullptr) {
            continue;
        }
        return mereader_tui_platform_spawn_opener(launcher, target, error);
    }

    mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL, "no external opener was found");
    return false;
}

static bool mereader_tui_svg_append_escaped(MereaderTuiString *svg, const char *text, MereaderTuiError *error) {
    for (const unsigned char *cursor = (const unsigned char *)text; *cursor != '\0'; ++cursor) {
        const char *escape = nullptr;
        switch (*cursor) {
        case '&':
            escape = "&amp;";
            break;
        case '<':
            escape = "&lt;";
            break;
        case '>':
            escape = "&gt;";
            break;
        case '"':
            escape = "&quot;";
            break;
        case '\'':
            escape = "&apos;";
            break;
        case '\t':
            escape = "&#9;";
            break;
        case '\n':
            escape = "&#10;";
            break;
        case '\r':
            escape = "&#13;";
            break;
        default:
            if (*cursor < 0x20U) {
                escape = "&#xfffd;";
            }
            break;
        }

        if (escape != nullptr) {
            if (!mereader_tui_string_append(svg, escape, error)) {
                return false;
            }
        } else if (!mereader_tui_string_append_char(svg, (char)*cursor, error)) {
            return false;
        }
    }
    return true;
}

bool mereader_tui_platform_save_svg(const char *path, const char *const *lines, size_t line_count, uint32_t background,
                            uint32_t foreground, MereaderTuiError *error) {
    if (path == nullptr || path[0] == '\0' || (line_count != 0U && lines == nullptr)) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "invalid SVG screenshot arguments");
        return false;
    }

    size_t max_columns = 0U;
    for (size_t index = 0U; index < line_count; ++index) {
        if (lines[index] == nullptr) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "SVG line %zu is null", index);
            return false;
        }
        size_t columns = mereader_tui_utf8_width(lines[index], strlen(lines[index]));
        if (columns > max_columns) {
            max_columns = columns;
        }
    }
    if (max_columns > (SIZE_MAX - 32U) / 8U || line_count > (SIZE_MAX - 32U) / 18U) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_MEMORY, "SVG dimensions overflow");
        return false;
    }

    size_t width = max_columns * 8U + 32U;
    size_t height = line_count * 18U + 32U;
    if (width < 320U) {
        width = 320U;
    }
    if (height < 64U) {
        height = 64U;
    }

    char header[512];
    int header_length = snprintf(
        header, sizeof(header),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%zu\" height=\"%zu\" viewBox=\"0 0 %zu %zu\">\n"
        "  <rect width=\"100%%\" height=\"100%%\" fill=\"#%06x\"/>\n"
        "  <g fill=\"#%06x\" font-family=\"monospace\" font-size=\"14\">\n",
        width, height, width, height, (unsigned int)(background & 0xffffffU),
        (unsigned int)(foreground & 0xffffffU));
    if (header_length < 0 || (size_t)header_length >= sizeof(header)) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_INTERNAL, "could not format SVG header");
        return false;
    }

    MereaderTuiString svg = {0};
    if (!mereader_tui_string_append_n(&svg, header, (size_t)header_length, error)) {
        return false;
    }

    for (size_t index = 0U; index < line_count; ++index) {
        char line_header[96];
        size_t y = 25U + index * 18U;
        int line_header_length = snprintf(line_header, sizeof(line_header),
                                          "    <text x=\"16\" y=\"%zu\" xml:space=\"preserve\">", y);
        if (line_header_length < 0 || (size_t)line_header_length >= sizeof(line_header) ||
            !mereader_tui_string_append_n(&svg, line_header, (size_t)line_header_length, error) ||
            !mereader_tui_svg_append_escaped(&svg, lines[index], error) ||
            !mereader_tui_string_append(&svg, "</text>\n", error)) {
            if (!mereader_tui_error_is_set(error)) {
                mereader_tui_error_set(error, MEREADER_TUI_ERROR_INTERNAL, "could not format SVG line");
            }
            mereader_tui_string_free(&svg);
            return false;
        }
    }

    if (!mereader_tui_string_append(&svg, "  </g>\n</svg>\n", error)) {
        mereader_tui_string_free(&svg);
        return false;
    }
    bool written = mereader_tui_write_file(path, svg.data, svg.length, error);
    mereader_tui_string_free(&svg);
    return written;
}
