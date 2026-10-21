#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "framework.h"

extern "C" {

    struct DiscordUser {
        const char* userId;
        const char* username;
        const char* discriminator;
        const char* avatar;
    };

    struct DiscordEventHandlers {
        void (*ready)(const DiscordUser*);
        void (*disconnected)(int, const char*);
        void (*errored)(int, const char*);
        void (*joinGame)(const char*);
        void (*spectateGame)(const char*);
        void (*joinRequest)(const DiscordUser*);
    };

    struct DiscordRichPresence {
        const char* state;
        const char* details;
        int64_t startTimestamp;
        int64_t endTimestamp;
        const char* largeImageKey;
        const char* largeImageText;
        const char* smallImageKey;
        const char* smallImageText;
        const char* partyId;
        int partySize;
        int partyMax;
        const char* matchSecret;
        const char* joinSecret;
        const char* spectateSecret;
        int8_t instance;
    };
}

static std::atomic<bool> g_running{ false };
static std::atomic<bool> g_readyFired{ false };
static std::atomic<bool> g_connected{ false };
static std::thread g_thread;
static DiscordEventHandlers g_handlers{};
static std::string g_iniPath;
static std::string JsonEscape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n";  break;
        case '\r': o += "\\r";  break;
        case '\t': o += "\\t";  break;
        default:
            if (c < 0x20) {
                char b[8];
                sprintf_s(b, "\\u%04x", c);
                o += b;
            }
            else {
                o += (char)c;
            }
        }
    }
    return o;
}

static std::string ReadIni(const char* key, const char* def = "") {
    char buf[512];
    GetPrivateProfileStringA("rpc", key, def, buf, sizeof(buf), g_iniPath.c_str());
    return std::string(buf);
}

static bool SendFrame(HANDLE h, uint32_t op, const std::string& json) {
    std::string pkt(8 + json.size(), '\0');
    uint32_t len = (uint32_t)json.size();
    memcpy(&pkt[0], &op, 4);
    memcpy(&pkt[4], &len, 4);
    if (!json.empty()) memcpy(&pkt[8], json.data(), json.size());
    DWORD written = 0;
    return WriteFile(h, pkt.data(), (DWORD)pkt.size(), &written, nullptr) && written == pkt.size();
}

static bool ReadFrame(HANDLE h, std::string& out) {
    uint32_t hdr[2];
    DWORD read = 0;
    if (!ReadFile(h, hdr, 8, &read, nullptr) || read != 8) return false;
    if (hdr[1] > (1u << 20)) return false;
    out.assign(hdr[1], '\0');
    if (hdr[1] && (!ReadFile(h, &out[0], hdr[1], &read, nullptr) || read != hdr[1])) return false;
    return true;
}

static HANDLE ConnectPipe() {
    for (int i = 0; i < 10; i++) {
        std::string name = "\\\\.\\pipe\\discord-ipc-" + std::to_string(i);
        HANDLE h = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
            nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) return h;
    }
    return INVALID_HANDLE_VALUE;
}

static std::string BuildActivity(int64_t startTs) {
    std::string a;
    auto add = [&](const char* k, const std::string& v) {
        if (v.empty()) return;
        if (!a.empty()) a += ",";
        a += "\"" + std::string(k) + "\":\"" + JsonEscape(v) + "\"";
        };
    add("details", ReadIni("details"));
    add("state", ReadIni("state"));

    std::string assets;
    auto addAsset = [&](const char* k, const std::string& v) {
        if (v.empty()) return;
        if (!assets.empty()) assets += ",";
        assets += "\"" + std::string(k) + "\":\"" + JsonEscape(v) + "\"";
        };
    addAsset("large_image", ReadIni("large_image"));
    addAsset("large_text", ReadIni("large_text"));
    addAsset("small_image", ReadIni("small_image"));
    addAsset("small_text", ReadIni("small_text"));
    if (!assets.empty()) {
        if (!a.empty()) a += ",";
        a += "\"assets\":{" + assets + "}";
    }

    if (ReadIni("show_timer", "1") == "1") {
        if (!a.empty()) a += ",";
        a += "\"timestamps\":{\"start\":" + std::to_string(startTs) + "}";
    }

    return "{" + a + "}";
}

static void Worker(std::string clientId) {
    int64_t startTs = (int64_t)time(nullptr);
    HANDLE pipe = INVALID_HANDLE_VALUE;
    int nonce = 0;

    while (g_running) {
        if (pipe == INVALID_HANDLE_VALUE) {
            pipe = ConnectPipe();
            if (pipe != INVALID_HANDLE_VALUE) {
                std::string hs = "{\"v\":1,\"client_id\":\"" + clientId + "\"}";
                std::string resp;
                bool ok = false;
                if (SendFrame(pipe, 0, hs) && ReadFrame(pipe, resp)) {
                    std::string msg =
                        "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" +
                        std::to_string(GetCurrentProcessId()) +
                        ",\"activity\":" + BuildActivity(startTs) +
                        "},\"nonce\":\"" + std::to_string(++nonce) + "\"}";
                    if (SendFrame(pipe, 1, msg) && ReadFrame(pipe, resp)) ok = true;
                }
                if (ok) {
                    g_connected = true;
                }
                else {
                    CloseHandle(pipe);
                    pipe = INVALID_HANDLE_VALUE;
                }
            }
        }
        else {
            DWORD avail = 0;
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr)) {
                CloseHandle(pipe);
                pipe = INVALID_HANDLE_VALUE;
                g_connected = false;
            }
            else if (avail) {
                std::string tmp;
                if (!ReadFrame(pipe, tmp)) {
                    CloseHandle(pipe);
                    pipe = INVALID_HANDLE_VALUE;
                    g_connected = false;
                }
            }
        }
        Sleep(1000);
    }
    if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
}

extern "C" {

    __declspec(dllexport) void Discord_Initialize(const char*, DiscordEventHandlers* handlers,
        int, const char*) {
        if (g_running.exchange(true)) return;
        if (handlers) g_handlers = *handlers;
        std::string clientId = ReadIni("client_id");
        g_thread = std::thread(Worker, clientId);
    }

    __declspec(dllexport) void Discord_Shutdown() {
        if (!g_running.exchange(false)) return;
        if (g_thread.joinable()) g_thread.join();
        g_connected = false;
    }

    __declspec(dllexport) void Discord_RunCallbacks() {
        if (g_connected && !g_readyFired.exchange(true) && g_handlers.ready) {
            DiscordUser u{ "0", "User", "0", "" };
            g_handlers.ready(&u);
        }
    }

    __declspec(dllexport) void Discord_UpdateConnection() {}
    __declspec(dllexport) void Discord_UpdatePresence(const DiscordRichPresence*) {}
    __declspec(dllexport) void Discord_ClearPresence() {}
    __declspec(dllexport) void Discord_Respond(const char*, int) {}
    __declspec(dllexport) void Discord_UpdateHandlers(DiscordEventHandlers* h) { if (h) g_handlers = *h; }
    __declspec(dllexport) void Discord_Register(const char*, const char*) {}
    __declspec(dllexport) void Discord_RegisterSteamGame(const char*, const char*) {}

}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        char path[MAX_PATH];
        GetModuleFileNameA(hModule, path, MAX_PATH);
        std::string p = path;
        g_iniPath = p.substr(0, p.find_last_of("\\/") + 1) + "settings.ini";
    }
    return TRUE;
}