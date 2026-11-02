/*
 * dsh-shim — native worker CLI shim for dsh-superagent on Windows.
 *
 * A worker session resolves `gh`, `lark-cli` or a blocked command by name, so the shim directory comes first on PATH.
 * This launcher replaces the POSIX `#!/bin/sh` shim, which Windows cannot execute: it removes its own directory from the
 * child's PATH and then either runs the deployment's CLI or refuses with the blocked-command message.
 *
 * The mapping lives in shims.xml beside the executable, keyed by this executable's own file name. XML is chosen over a
 * mutable key-value format so that the contract does not drift between restarts.
 *
 * Exit codes: the child's own code, or 1 for a refusal (with the message on stderr) and 70 when the shim is misconfigured.
 * Every run ends within the configured wall-clock limit; nothing waits without a bound.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define MAX_SHIM_XML (256 * 1024)

static void fail(wchar_t *format, ...) {
    wchar_t buffer[2048];
    va_list args;
    va_start(args, format);
    vswprintf(buffer, 2048, format, args);
    va_end(args);
    fwprintf(stderr, L"%ls\n", buffer);
}

/* Reads a whole file as UTF-8 bytes; returns NULL when it is absent or larger than the cap. */
static char *slurp(const wchar_t *path, DWORD *size) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER length;
    char *text = NULL;
    if (GetFileSizeEx(file, &length) && length.QuadPart < MAX_SHIM_XML) {
        *size = (DWORD)length.QuadPart;
        text = (char *)malloc(*size + 1);
        DWORD read = 0;
        if (text && (!*size || (ReadFile(file, text, *size, &read, NULL) && read == *size))) {
            text[*size] = 0;
        } else {
            free(text);
            text = NULL;
        }
    }
    CloseHandle(file);
    return text;
}

/* Copies the text of the first <name …>…</name> element into out. */
static int element(const char *xml, const char *name, char *out, size_t cap) {
    char open[128];
    snprintf(open, sizeof open, "<%s", name);
    const char *start = strstr(xml, open);
    if (!start) return 0;
    start = strchr(start, '>');
    if (!start) return 0;
    start++;
    char close[128];
    snprintf(close, sizeof close, "</%s>", name);
    const char *end = strstr(start, close);
    if (!end) return 0;
    size_t length = (size_t)(end - start);
    if (length >= cap) return 0;
    memcpy(out, start, length);
    out[length] = 0;
    return 1;
}

/* Copies the target of the <shim> entry whose name attribute equals `name`. */
static int lookup(const char *xml, const char *name, char *out, size_t cap) {
    const char *cursor = xml;
    char wanted[1024];
    snprintf(wanted, sizeof wanted, "name=\"%s\"", name);
    for (;;) {
        const char *entry = strstr(cursor, "<shim");
        if (!entry) return 0;
        const char *entry_end = strstr(entry, "</shim>");
        if (!entry_end) return 0;
        if (strstr(entry, wanted) && strstr(entry, wanted) < entry_end) {
            /* Re-read the entry body alone so nested lookups cannot wander into the next one. */
            size_t length = (size_t)(entry_end - entry);
            char *body = (char *)malloc(length + 1);
            if (!body) return 0;
            memcpy(body, entry, length);
            body[length] = 0;
            int found = element(body, "run", out, cap) || element(body, "refuse", out, cap);
            free(body);
            return found;
        }
        cursor = entry_end + 7;
    }
}

static void strip_directory(wchar_t *path, const wchar_t *directory) {
    wchar_t result[32768 * 2];
    result[0] = 0;
    size_t used = 0;
    wchar_t *cursor = path;
    while (cursor && *cursor) {
        wchar_t *end = wcschr(cursor, L';');
        size_t length = end ? (size_t)(end - cursor) : wcslen(cursor);
        wchar_t entry[32768];
        if (length >= 32768) length = 32767;
        wcsncpy(entry, cursor, length);
        entry[length] = 0;
        while (length && entry[length - 1] == L'\\') entry[--length] = 0;
        if (length && _wcsicmp(entry, directory) != 0) {
            if (used && used + 1 < sizeof result / sizeof result[0]) result[used++] = L';';
            if (used + length < sizeof result / sizeof result[0]) {
                wcscpy(result + used, entry);
                used += length;
            }
        }
        if (!end) break;
        cursor = end + 1;
    }
    result[used] = 0;
    wcscpy(path, result);
}

static wchar_t *quote(const wchar_t *value) {
    size_t length = wcslen(value);
    /* Worst case halves the argument with backslash escapes, then doubles the quote escapes. */
    wchar_t *out = (wchar_t *)malloc((length * 2 + 8) * sizeof(wchar_t));
    if (!out) return NULL;
    wchar_t *write = out;
    *write++ = L'"';
    size_t slashes = 0;
    for (const wchar_t *read = value; *read; read++) {
        if (*read == L'\\') {
            slashes++;
            *write++ = L'\\';
            continue;
        }
        /* Backslashes before a quote must be escaped too, and the quote itself twice. */
        if (*read == L'"') {
            for (size_t index = 0; index < slashes; index++) *write++ = L'\\';
            *write++ = L'\\';
        }
        slashes = 0;
        *write++ = *read;
    }
    /* Backslashes before the closing quote are escaped as well. */
    for (size_t index = 0; index < slashes; index++) *write++ = L'\\';
    *write++ = L'"';
    *write = 0;
    return out;
}

int wmain(int argc, wchar_t **argv) {
    wchar_t exe[32768];
    if (!GetModuleFileNameW(NULL, exe, 32768)) {
        fail(L"dsh-shim: cannot identify its own path (%lu)", GetLastError());
        return 70;
    }
    wchar_t directory[32768];
    wcscpy(directory, exe);
    wchar_t *slash = wcsrchr(directory, L'\\');
    if (slash) *slash = 0;

    wchar_t name[512];
    const wchar_t *base = slash ? slash + 1 : exe;
    wcsncpy(name, base, 511);
    name[511] = 0;
    wchar_t *dot = wcsrchr(name, L'.');
    if (dot) *dot = 0;

    wchar_t xml_path[32768];
    swprintf(xml_path, 32768, L"%ls\\shims.xml", directory);
    DWORD xml_size = 0;
    char *xml = slurp(xml_path, &xml_size);
    if (!xml) {
        fail(L"dsh-shim: %ls is missing; restart dsh-superagent to write it", xml_path);
        return 70;
    }

    char narrow_name[512];
    WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow_name, sizeof narrow_name, NULL, NULL);

    char target[32768];
    if (!lookup(xml, narrow_name, target, sizeof target)) {
        free(xml);
        fail(L"dsh-shim: shims.xml defines no entry for %ls", name);
        return 70;
    }
    wchar_t wide[32768];
    MultiByteToWideChar(CP_UTF8, 0, target, -1, wide, 32768);

    /* A refusal carries its message as the target text and never starts a program. */
    const char *cursor = xml;
    char wanted[1024];
    snprintf(wanted, sizeof wanted, "name=\"%s\"", narrow_name);
    int refuse = 0;
    {
        const char *entry = strstr(cursor, wanted);
        if (entry) {
            const char *refuse_at = strstr(entry, "<refuse>");
            const char *end = strstr(entry, "</shim>");
            refuse = refuse_at && end && refuse_at < end;
        }
    }
    free(xml);

    if (refuse) {
        fail(L"%ls", wide);
        return 1;
    }

    wchar_t path[65536];
    DWORD path_length = GetEnvironmentVariableW(L"PATH", path, 65536);
    if (path_length && path_length < 65536) strip_directory(path, directory);
    else path[0] = 0;
    SetEnvironmentVariableW(L"PATH", path);

    wchar_t *command_line = (wchar_t *)malloc(sizeof(wchar_t) * 65536);
    if (!command_line) {
        fail(L"dsh-shim: out of memory");
        return 70;
    }
    wchar_t *quoted = quote(wide);
    if (!quoted) {
        free(command_line);
        fail(L"dsh-shim: out of memory");
        return 70;
    }
    swprintf(command_line, 65536, L"%ls", quoted);
    free(quoted);
    for (int index = 1; index < argc; index++) {
        wchar_t *argument = quote(argv[index]);
        if (!argument) continue;
        size_t used = wcslen(command_line);
        if (used + wcslen(argument) + 2 < 65536) {
            command_line[used] = L' ';
            wcscpy(command_line + used + 1, argument);
        }
        free(argument);
    }

    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    ZeroMemory(&startup, sizeof startup);
    startup.cb = sizeof startup;
    ZeroMemory(&process, sizeof process);

    /* The target may be a .exe or a .cmd/.bat that the deployment names; both go through the same bounded wait. */
    if (!CreateProcessW(wide, command_line, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process)) {
        fail(L"dsh-shim: cannot start %ls (%lu)", wide, GetLastError());
        free(command_line);
        return 1;
    }
    free(command_line);

    DWORD limit_ms = 0;
    wchar_t limit[64];
    if (GetEnvironmentVariableW(L"DSH_SHIM_TIMEOUT_MS", limit, 64)) limit_ms = (DWORD)_wtoi(limit);
    DWORD waited = WaitForSingleObject(process.hProcess, limit_ms ? limit_ms : 600000);
    if (waited == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 124);
        WaitForSingleObject(process.hProcess, 5000);
        fail(L"dsh-shim: %ls exceeded its time limit", wide);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return 124;
    }
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return (int)code;
}
