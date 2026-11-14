#include <windows.h>
#include <iostream>
#include <string>
#include <chrono>
#include <thread>

struct SharedLobbyData {
    uint32_t sequence;
    char link[512];
};

int main() {
    HANDLE hMapFile = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\OpenSteamToolLobbyMap");
    if (!hMapFile) {
        HANDLE hEvent = CreateEventA(NULL, FALSE, FALSE, "Local\\OpenSteamToolLobbyEvent");
        while (!hMapFile) {
            hMapFile = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\OpenSteamToolLobbyMap");
            if (hMapFile) break;
            if (hEvent) WaitForSingleObject(hEvent, 500);
            else std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (hEvent) CloseHandle(hEvent);
    }

    const auto* pData = static_cast<const SharedLobbyData*>(MapViewOfFile(hMapFile, FILE_MAP_READ, 0, 0, sizeof(SharedLobbyData)));
    if (!pData) {
        if (hMapFile) CloseHandle(hMapFile);
        return 1;
    }

    HANDLE hEvent = OpenEventA(SYNCHRONIZE, FALSE, "Local\\OpenSteamToolLobbyEvent");
    uint32_t lastSeq = 0;

    while (true) {
        if (hEvent) {
            WaitForSingleObject(hEvent, 1000);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            hEvent = OpenEventA(SYNCHRONIZE, FALSE, "Local\\OpenSteamToolLobbyEvent");
        }

        uint32_t currentSeq = pData->sequence;
        if (currentSeq != 0 && currentSeq != lastSeq) {
            lastSeq = currentSeq;
            std::string link = pData->link;
            if (!link.empty()) {
                std::cout << link << std::endl;
            }
        }
    }

    UnmapViewOfFile(pData);
    if (hMapFile) CloseHandle(hMapFile);
    if (hEvent) CloseHandle(hEvent);
    return 0;
}
