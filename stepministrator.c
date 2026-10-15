#include <windows.h>

#include <stdlib.h>
#include <string.h>
#include <strsafe.h>
#include <wchar.h>
#include <winhttp.h>

#include "vendor/puff.h"

#define TITLE L"stepministrator"
#define LAUNCHER_ENTRY "Games.exe"
#define CMDLINE_MAX 32768

static const WCHAR USAGE[] =
    L"usage: stepministrator --api <url> --appcode <code> --channel <n> --sub-channel <n> --game <exe> [-- <game args>...]";

enum {
    EOCD_SIGNATURE = 0x06054b50,
    EOCD_SIZE = 22,
    EOCD_DIR_SIZE = 12,
    EOCD_DIR_OFFSET = 16,
    ZIP_MAX_COMMENT = 0xffff,
};

enum {
    CD_SIGNATURE = 0x02014b50,
    CD_METHOD = 10,
    CD_CRC = 16,
    CD_PACKED_SIZE = 20,
    CD_SIZE = 24,
    CD_NAME_LEN = 28,
    CD_EXTRA_LEN = 30,
    CD_COMMENT_LEN = 32,
    CD_LOCAL_OFFSET = 42,
    CD_HEADER_SIZE = 46,
};

enum {
    LOCAL_NAME_LEN = 26,
    LOCAL_EXTRA_LEN = 28,
    LOCAL_HEADER_SIZE = 30,
};

enum {
    ZIP_STORED = 0,
    ZIP_DEFLATED = 8,
};

DWORD WINAPI RtlComputeCrc32(DWORD initial, const BYTE *data, INT len);

typedef WCHAR *(CDECL *WineGetDosFileName)(const char *);

typedef struct {
    BYTE *data;
    DWORD len;
} Buffer;

typedef struct {
    UINT32 local_offset;
    UINT32 packed_size;
    UINT32 size;
    UINT32 crc;
    UINT16 method;
} ZipEntry;

typedef struct {
    const WCHAR *api;
    const WCHAR *appcode;
    const WCHAR *channel;
    const WCHAR *sub_channel;
    const WCHAR *game;
    WCHAR **game_args;
    int game_argc;
} Options;

typedef struct {
    WCHAR *buf;
    size_t size;
    size_t len;
    BOOL overflow;
} Writer;

static void fail(DWORD err, const WCHAR *fmt, ...) {
    WCHAR msg[1024], sys[512] = L"";
    va_list args;

    va_start(args, fmt);
    StringCchVPrintfW(msg, ARRAYSIZE(msg), fmt, args);
    va_end(args);

    if (err &&
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err, 0, sys, ARRAYSIZE(sys), NULL)) {
        size_t len = wcslen(msg);
        StringCchPrintfW(msg + len, ARRAYSIZE(msg) - len, L"\n\n%ls", sys);
    }
    MessageBoxW(NULL, msg, TITLE, MB_OK | MB_ICONERROR);
}

static void buffer_free(Buffer *buf) {
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
}

static UINT16 le16(const BYTE *p) {
    return (UINT16)(p[0] | p[1] << 8);
}

static UINT32 le32(const BYTE *p) {
    return le16(p) | (UINT32)le16(p + 2) << 16;
}

static BOOL read_response(HINTERNET request, Buffer *out) {
    DWORD avail, got, cap = 1 << 16;
    BYTE *grown;

    out->len = 0;
    if (!(out->data = malloc(cap)))
        return FALSE;
    while (WinHttpQueryDataAvailable(request, &avail)) {
        if (!avail) {
            out->data[out->len] = 0;
            return TRUE;
        }
        while (out->len + avail + 1 > cap)
            cap *= 2;
        if (!(grown = realloc(out->data, cap)))
            break;
        out->data = grown;
        if (!WinHttpReadData(request, out->data + out->len, avail, &got))
            break;
        out->len += got;
    }
    buffer_free(out);
    return FALSE;
}

static BOOL http_get(const WCHAR *url, const WCHAR *header, Buffer *out) {
    URL_COMPONENTS uc = {.dwStructSize = sizeof(uc), .dwHostNameLength = (DWORD)-1, .dwUrlPathLength = (DWORD)-1};
    HINTERNET session = NULL, connect = NULL, request = NULL;
    DWORD status = 0, status_size = sizeof(status);
    WCHAR host[256];
    BOOL ok = FALSE;

    if (!WinHttpCrackUrl(url, 0, 0, &uc) || uc.dwHostNameLength >= ARRAYSIZE(host))
        return FALSE;
    wmemcpy(host, uc.lpszHostName, uc.dwHostNameLength);
    host[uc.dwHostNameLength] = 0;

    if (!(session = WinHttpOpen(TITLE, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)))
        goto done;
    if (!(connect = WinHttpConnect(session, host, uc.nPort, 0)))
        goto done;
    if (!(request = WinHttpOpenRequest(connect,
                                       L"GET",
                                       uc.lpszUrlPath,
                                       NULL,
                                       WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)))
        goto done;
    if (!WinHttpSendRequest(
            request, header ? header : WINHTTP_NO_ADDITIONAL_HEADERS, header ? (DWORD)-1 : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        goto done;
    if (!WinHttpReceiveResponse(request, NULL))
        goto done;
    if (!WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &status,
                             &status_size,
                             WINHTTP_NO_HEADER_INDEX))
        goto done;
    if (status != (header ? 206 : 200))
        goto done;
    ok = read_response(request, out);

done:
    if (request)
        WinHttpCloseHandle(request);
    if (connect)
        WinHttpCloseHandle(connect);
    if (session)
        WinHttpCloseHandle(session);
    return ok;
}

static BOOL http_get_range(const WCHAR *url, UINT32 first, UINT32 count, Buffer *out) {
    WCHAR header[64];

    StringCchPrintfW(
        header, ARRAYSIZE(header), L"Range: bytes=%lu-%lu", (unsigned long)first, (unsigned long)(first + count - 1));
    if (!http_get(url, header, out))
        return FALSE;
    if (out->len == count)
        return TRUE;
    buffer_free(out);
    return FALSE;
}

static BOOL json_string(const char *json, const char *key, char *out, size_t size) {
    char needle[64];
    const char *p;
    size_t n = 0;

    StringCchPrintfA(needle, sizeof(needle), "\"%s\":\"", key);
    if (!(p = strstr(json, needle)))
        return FALSE;
    for (p += strlen(needle); *p && *p != '"'; p++) {
        if (*p == '\\')
            continue;
        if (n + 1 >= size)
            return FALSE;
        out[n++] = *p;
    }
    out[n] = 0;
    return *p == '"';
}

static BOOL zip_find(const WCHAR *url, const char *name, ZipEntry *entry) {
    Buffer tail = {0}, dir = {0};
    const BYTE *eocd = NULL, *p, *end;
    size_t name_len = strlen(name);
    WCHAR tail_range[64];
    DWORD i;
    BOOL found = FALSE;

    StringCchPrintfW(tail_range, ARRAYSIZE(tail_range), L"Range: bytes=-%d", EOCD_SIZE + ZIP_MAX_COMMENT);
    if (!http_get(url, tail_range, &tail))
        return FALSE;
    for (i = tail.len >= EOCD_SIZE ? tail.len - EOCD_SIZE + 1 : 0; i-- > 0;) {
        if (le32(tail.data + i) == EOCD_SIGNATURE) {
            eocd = tail.data + i;
            break;
        }
    }
    if (eocd && http_get_range(url, le32(eocd + EOCD_DIR_OFFSET), le32(eocd + EOCD_DIR_SIZE), &dir)) {
        for (p = dir.data, end = dir.data + dir.len; p + CD_HEADER_SIZE <= end && le32(p) == CD_SIGNATURE;
             p += CD_HEADER_SIZE + le16(p + CD_NAME_LEN) + le16(p + CD_EXTRA_LEN) + le16(p + CD_COMMENT_LEN)) {
            if (le16(p + CD_NAME_LEN) != name_len || memcmp(p + CD_HEADER_SIZE, name, name_len))
                continue;
            entry->method = le16(p + CD_METHOD);
            entry->crc = le32(p + CD_CRC);
            entry->packed_size = le32(p + CD_PACKED_SIZE);
            entry->size = le32(p + CD_SIZE);
            entry->local_offset = le32(p + CD_LOCAL_OFFSET);
            found = TRUE;
            break;
        }
    }
    buffer_free(&tail);
    buffer_free(&dir);
    return found;
}

static BOOL zip_extract(const WCHAR *url, const ZipEntry *entry, Buffer *out) {
    Buffer header = {0}, packed = {0};
    unsigned long out_len = entry->size, in_len = entry->packed_size;
    UINT32 data_offset;
    BOOL ok = FALSE;

    if (!http_get_range(url, entry->local_offset, LOCAL_HEADER_SIZE, &header))
        return FALSE;
    data_offset =
        entry->local_offset + LOCAL_HEADER_SIZE + le16(header.data + LOCAL_NAME_LEN) + le16(header.data + LOCAL_EXTRA_LEN);
    if (!http_get_range(url, data_offset, entry->packed_size, &packed))
        goto done;
    if (!(out->data = malloc(entry->size ? entry->size : 1)))
        goto done;
    out->len = entry->size;

    if (entry->method == ZIP_STORED && entry->packed_size == entry->size)
        memcpy(out->data, packed.data, entry->size);
    else if (entry->method != ZIP_DEFLATED || puff(out->data, &out_len, packed.data, &in_len) || out_len != entry->size)
        goto done;
    ok = RtlComputeCrc32(0, out->data, (INT)out->len) == entry->crc;

done:
    if (!ok)
        buffer_free(out);
    buffer_free(&header);
    buffer_free(&packed);
    return ok;
}

static BOOL read_text(const WCHAR *path, char *out, DWORD size) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD got = 0;
    BOOL ok;

    if (file == INVALID_HANDLE_VALUE)
        return FALSE;
    ok = ReadFile(file, out, size - 1, &got, NULL);
    CloseHandle(file);
    out[ok ? got : 0] = 0;
    return ok;
}

static BOOL write_file(const WCHAR *path, const void *data, DWORD len) {
    WCHAR tmp[MAX_PATH];
    HANDLE file;
    DWORD written = 0;
    BOOL ok;

    if (FAILED(StringCchPrintfW(tmp, ARRAYSIZE(tmp), L"%ls.tmp", path)))
        return FALSE;
    file = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return FALSE;
    ok = WriteFile(file, data, len, &written, NULL) && written == len;
    CloseHandle(file);
    return ok && MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING);
}

static BOOL file_exists(const WCHAR *path) {
    DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static BOOL download_launcher(const Options *o, const WCHAR *exe_path, const WCHAR *version_path) {
    WCHAR api_url[1024], zip_url[2048];
    char version[64], cached[64], zip_url_a[2048];
    Buffer json = {0}, launcher = {0};
    ZipEntry entry;
    BOOL ok = FALSE;

    // seems like CN works with Global's one lol
    if (FAILED(StringCchPrintfW(api_url,
                                ARRAYSIZE(api_url),
                                L"%ls/launcher/get_latest?appcode=%ls&channel=%ls&sub_channel=%ls",
                                o->api,
                                o->appcode,
                                o->channel,
                                o->sub_channel)))
        return FALSE;
    if (!http_get(api_url, NULL, &json))
        return FALSE;
    if (!json_string((char *)json.data, "version", version, sizeof(version)))
        goto done;
    if (!json_string((char *)json.data, "zip_package_url", zip_url_a, sizeof(zip_url_a)))
        goto done;
    if (read_text(version_path, cached, sizeof(cached)) && !strcmp(cached, version) && file_exists(exe_path)) {
        ok = TRUE;
        goto done;
    }
    if (!MultiByteToWideChar(CP_UTF8, 0, zip_url_a, -1, zip_url, ARRAYSIZE(zip_url)))
        goto done;
    if (!zip_find(zip_url, LAUNCHER_ENTRY, &entry) || !zip_extract(zip_url, &entry, &launcher))
        goto done;
    ok = write_file(exe_path, launcher.data, launcher.len) && write_file(version_path, version, (DWORD)strlen(version));

done:
    buffer_free(&json);
    buffer_free(&launcher);
    return ok;
}

static BOOL sibling_path(const WCHAR *name, WCHAR *out, size_t size) {
    WCHAR *slash;
    DWORD len = GetModuleFileNameW(NULL, out, (DWORD)size);

    if (!len || len >= size || !(slash = wcsrchr(out, L'\\')))
        return FALSE;
    *(slash + 1) = 0;
    return SUCCEEDED(StringCchCatW(out, size, name));
}

static BOOL dos_path(const WCHAR *path, WCHAR *out, size_t size) {
    WineGetDosFileName to_dos;
    char unix_path[MAX_PATH * 4];
    WCHAR *dos;
    BOOL ok;

    if (path[0] != L'/')
        return SUCCEEDED(StringCchCopyW(out, size, path));
    to_dos = (WineGetDosFileName)(void *)GetProcAddress(GetModuleHandleW(L"kernel32"), "wine_get_dos_file_name");
    if (!to_dos || !WideCharToMultiByte(CP_UTF8, 0, path, -1, unix_path, sizeof(unix_path), NULL, NULL))
        return FALSE;
    if (!(dos = to_dos(unix_path)))
        return FALSE;
    ok = SUCCEEDED(StringCchCopyW(out, size, dos));
    HeapFree(GetProcessHeap(), 0, dos);
    return ok;
}

static void put(Writer *w, WCHAR c) {
    if (w->len + 1 >= w->size)
        w->overflow = TRUE;
    else
        w->buf[w->len++] = c;
}

static BOOL append_arg(WCHAR *cmd, size_t size, const WCHAR *arg) {
    Writer w = {.buf = cmd, .size = size, .len = wcslen(cmd)};
    const WCHAR *p;

    if (w.len)
        put(&w, L' ');
    put(&w, L'"');
    for (p = arg;; p++) {
        unsigned int backslashes = 0;
        while (*p == L'\\') {
            backslashes++;
            p++;
        }
        if (!*p || *p == L'"')
            backslashes *= 2;
        while (backslashes--)
            put(&w, L'\\');
        if (!*p)
            break;
        if (*p == L'"')
            put(&w, L'\\');
        put(&w, *p);
    }
    put(&w, L'"');
    cmd[w.len] = 0;
    return !w.overflow;
}

static HANDLE create_kill_job(void) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {0};
    HANDLE job = CreateJobObjectW(NULL, NULL);

    if (!job)
        return NULL;
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
    if (SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info)))
        return job;
    CloseHandle(job);
    return NULL;
}

static int run_game(const WCHAR *launcher, const WCHAR *game, WCHAR **args, int argc) {
    static WCHAR cmd[CMDLINE_MAX];
    WCHAR launcher_cmd[MAX_PATH + 3] = L"", game_dir[MAX_PATH], *slash;
    STARTUPINFOW launcher_si = {.cb = sizeof(launcher_si)};
    STARTUPINFOEXW si = {.StartupInfo.cb = sizeof(si)};
    PROCESS_INFORMATION launcher_pi = {0}, pi = {0};
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    SIZE_T attrs_size = 0;
    HANDLE job = NULL;
    DWORD exit_code = 1;
    int i;

    if (FAILED(StringCchCopyW(game_dir, ARRAYSIZE(game_dir), game)) || !(slash = wcsrchr(game_dir, L'\\'))) {
        fail(0, L"Not a game path:\n%ls", game);
        return 1;
    }
    *slash = 0;
    if (!append_arg(launcher_cmd, ARRAYSIZE(launcher_cmd), launcher) || !append_arg(cmd, ARRAYSIZE(cmd), game)) {
        fail(0, L"Path too long.");
        return 1;
    }
    for (i = 0; i < argc; i++) {
        if (!append_arg(cmd, ARRAYSIZE(cmd), args[i])) {
            fail(0, L"Game command line too long.");
            return 1;
        }
    }

    if (!(job = create_kill_job())) {
        fail(GetLastError(), L"Couldn't create the launcher job.");
        return 1;
    }
    if (!CreateProcessW(launcher, launcher_cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &launcher_si, &launcher_pi)) {
        fail(GetLastError(), L"Couldn't start the launcher:\n%ls", launcher);
        goto done;
    }
    if (!AssignProcessToJobObject(job, launcher_pi.hProcess)) {
        fail(GetLastError(), L"Couldn't put the launcher in its job.");
        goto done;
    }

    InitializeProcThreadAttributeList(NULL, 1, 0, &attrs_size);
    attrs = HeapAlloc(GetProcessHeap(), 0, attrs_size);
    if (attrs && !InitializeProcThreadAttributeList(attrs, 1, 0, &attrs_size)) {
        HeapFree(GetProcessHeap(), 0, attrs);
        attrs = NULL;
    }
    if (!attrs ||
        !UpdateProcThreadAttribute(
            attrs, 0, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS, &launcher_pi.hProcess, sizeof(launcher_pi.hProcess), NULL, NULL)) {
        fail(GetLastError(), L"Couldn't set the launcher as the game's parent.");
        goto done;
    }
    si.lpAttributeList = attrs;
    if (!CreateProcessW(game, cmd, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT, NULL, game_dir, &si.StartupInfo, &pi)) {
        fail(GetLastError(), L"Couldn't start the game:\n%ls", game);
        goto done;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    if (!GetExitCodeProcess(pi.hProcess, &exit_code))
        exit_code = 1;

done:
    if (attrs) {
        DeleteProcThreadAttributeList(attrs);
        HeapFree(GetProcessHeap(), 0, attrs);
    }
    if (pi.hThread)
        CloseHandle(pi.hThread);
    if (pi.hProcess)
        CloseHandle(pi.hProcess);
    if (launcher_pi.hProcess)
        TerminateProcess(launcher_pi.hProcess, 0);
    if (launcher_pi.hThread)
        CloseHandle(launcher_pi.hThread);
    if (launcher_pi.hProcess)
        CloseHandle(launcher_pi.hProcess);
    CloseHandle(job);
    return (int)exit_code;
}

static BOOL parse_args(int argc, WCHAR **argv, Options *o) {
    const struct {
        const WCHAR *flag;
        const WCHAR **value;
    } flags[] = {
        {L"--api", &o->api},
        {L"--appcode", &o->appcode},
        {L"--channel", &o->channel},
        {L"--sub-channel", &o->sub_channel},
        {L"--game", &o->game},
    };
    int i;
    size_t f;

    for (i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--")) {
            o->game_args = argv + i + 1;
            o->game_argc = argc - i - 1;
            break;
        }
        for (f = 0; f < ARRAYSIZE(flags) && wcscmp(argv[i], flags[f].flag); f++)
            ;
        if (f == ARRAYSIZE(flags) || i + 1 >= argc)
            return FALSE;
        *flags[f].value = argv[++i];
    }
    return o->api && o->appcode && o->channel && o->sub_channel && o->game;
}

int wmain(int argc, WCHAR **argv) {
    Options o = {0};
    WCHAR launcher[MAX_PATH], version[MAX_PATH], game[MAX_PATH];

    if (!parse_args(argc, argv, &o)) {
        fail(0, USAGE);
        return 1;
    }
    if (!dos_path(o.game, game, ARRAYSIZE(game)) || !file_exists(game)) {
        fail(0, L"Game not found:\n%ls", o.game);
        return 1;
    }
    if (!sibling_path(L"Games.exe", launcher, ARRAYSIZE(launcher)) ||
        !sibling_path(L"Games.version", version, ARRAYSIZE(version))) {
        fail(GetLastError(), L"Couldn't lcoate stepministrator's own folder.");
        return 1;
    }
    if (!download_launcher(&o, launcher, version) && !file_exists(launcher)) {
        fail(0, L"Couldn't download Games.exe and there's no cached copy.");
        return 1;
    }
    return run_game(launcher, game, o.game_args, o.game_argc);
}
