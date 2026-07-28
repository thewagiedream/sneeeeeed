// ds4_correlate.cpp
//
// STANDALONE DIAGNOSTIC TOOL - replaces ds4_mapper.exe.
//
// The previous tool (ds4_mapper.exe) recorded whatever order Windows
// happened to list your 8 DS4 devices in. That turned out to NOT be the
// same as the order your Python script created them in (pad 0, 1, 2...7)
// - there was never a guarantee those two orders would match, they just
// coincidentally lined up before.
//
// This tool fixes that by watching the ACTUAL button presses. Your Python
// tool's "auto" command presses pad 0, then pad 1, then pad 2... in strict
// order, with gaps in between. This tool watches all 8 real DS4 devices at
// once, and whichever physical device lights up 1st is genuinely pad 0,
// whichever lights up 2nd is genuinely pad 1, and so on. No guessing.
//
// HOW TO USE:
//   1. Make sure your 8 virtual pads already exist (controllertesttool.py
//      is running and you're at its "> " prompt).
//   2. Run ds4_correlate.exe. It will find all DS4 devices and say
//      "Ready - waiting for presses..."
//   3. In the Python tool, type "auto" and press Enter. It waits 3 seconds
//      then presses pad 0, then 1, then 2... through 7, with small gaps.
//   4. Watch this tool's output - it will report each press as it detects
//      it, in the order it saw them, and write the result to
//      ds4_devices.txt in that (now VERIFIED CORRECT) order.
//   5. Copy ds4_devices.txt into your PES2018 folder as before.
//
// BUILD:
//   cl /EHsc ds4_correlate.cpp setupapi.lib hid.lib

#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <cstdio>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

#define DS4_VID 0x054C
#define DS4_PID 0x05C4

struct Watched
{
    std::string devicePath;
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::thread reader;
    std::atomic<bool> running{false};
    std::atomic<bool> pressed{false}; // current dpad-east state
    std::atomic<bool> assigned{false}; // already matched to a creation index
};

static void ReaderThread(Watched* w)
{
    BYTE buf[64];
    while (w->running)
    {
        DWORD read = 0;
        BOOL ok = ReadFile(w->handle, buf, sizeof(buf), &read, nullptr);
        if (ok && read > 0 && buf[0] == 0x01)
        {
            int dpad = buf[5] & 0x0F;
            w->pressed = (dpad == 2); // 2 = East / Right, matches the DLL's decode
        }
        else if (!ok)
        {
            break;
        }
    }
}

int main()
{
    // When stdout is redirected to a pipe (as ds4_bootstrap.py does, so it
    // can watch for "Ready" and capture output) instead of a real console,
    // the C runtime silently switches from line-buffered to fully-buffered
    // - printf output then sits in an internal buffer and never reaches the
    // reader until that buffer fills or the process exits. That means the
    // launcher waits for "Ready", nothing ever arrives, and it times out
    // and kills this process before the buffer is ever flushed - which
    // also looks like "the exe never opened" from the outside, since there
    // was never a visible console for it to begin with. Force fully
    // unbuffered stdout so every printf shows up immediately.
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("DS4 Correlate - finding DS4-class HID devices (VID %04X / PID %04X)...\n\n",
           DS4_VID, DS4_PID);

    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO devInfo = SetupDiGetClassDevsA(&hidGuid, nullptr, nullptr,
                                             DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE)
    {
        printf("ERROR: SetupDiGetClassDevsA failed (%lu)\n", GetLastError());
        return 1;
    }

    SP_DEVICE_INTERFACE_DATA ifData{};
    ifData.cbSize = sizeof(ifData);

    std::vector<Watched*> watched;

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devInfo, nullptr, &hidGuid, i, &ifData); i++)
    {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailA(devInfo, &ifData, nullptr, 0, &needed, nullptr);
        if (!needed) continue;

        auto detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)malloc(needed);
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

        if (SetupDiGetDeviceInterfaceDetailA(devInfo, &ifData, detail, needed, nullptr, nullptr))
        {
            HANDLE h = CreateFileA(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE)
            {
                HIDD_ATTRIBUTES attr{ sizeof(HIDD_ATTRIBUTES) };
                if (HidD_GetAttributes(h, &attr) && attr.VendorID == DS4_VID && attr.ProductID == DS4_PID)
                {
                    auto w = new Watched();
                    w->devicePath = detail->DevicePath;
                    w->handle = h;
                    watched.push_back(w);
                }
                else
                {
                    CloseHandle(h);
                }
            }
        }
        free(detail);
    }
    SetupDiDestroyDeviceInfoList(devInfo);

    if (watched.empty())
    {
        printf("No DS4 devices found. Make sure your virtual pads are created first.\n");
        return 1;
    }

    printf("Found %zu DS4 device(s). Starting watcher threads...\n", watched.size());
    for (auto* w : watched)
    {
        w->running = true;
        w->reader = std::thread(ReaderThread, w);
    }

    printf("\nReady. Now go to your Python tool and type: auto\n");
    printf("Waiting for presses (this will run for up to 60 seconds)...\n\n");

    std::vector<std::string> orderedPaths; // index = creation order, value = device path
    auto start = std::chrono::steady_clock::now();

    while (orderedPaths.size() < watched.size())
    {
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > std::chrono::seconds(60))
        {
            printf("\nTimed out waiting for presses. Got %zu of %zu.\n",
                   orderedPaths.size(), watched.size());
            break;
        }

        for (auto* w : watched)
        {
            if (w->pressed && !w->assigned)
            {
                w->assigned = true;
                int detectedIndex = (int)orderedPaths.size();
                orderedPaths.push_back(w->devicePath);
                printf("Detected press #%d -> %s\n", detectedIndex, w->devicePath.c_str());
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // Shut down watcher threads
    for (auto* w : watched)
    {
        w->running = false;
        CancelIoEx(w->handle, nullptr);
        if (w->reader.joinable()) w->reader.join();
        CloseHandle(w->handle);
    }

    if (orderedPaths.size() < 8)
    {
        printf("\nWARNING: only detected %zu of 8 expected presses.\n", orderedPaths.size());
        printf("Nothing was written. Fix the issue below and re-run:\n");
        printf(" - Did you type 'auto' in the Python tool AFTER seeing 'Ready' here?\n");
        printf(" - Are all 8 pads from controllertesttool.py still alive (don't close that window)?\n");
        return 1;
    }

    FILE* f = fopen("ds4_devices.txt", "w");
    if (!f)
    {
        printf("\nERROR: could not write ds4_devices.txt.\n");
        return 1;
    }
    for (auto& p : orderedPaths) fprintf(f, "%s\n", p.c_str());
    fclose(f);

    printf("\nSuccess. Wrote ds4_devices.txt with %zu entries in VERIFIED creation order.\n",
           orderedPaths.size());
    printf("Copy it into your PES2018 folder now.\n");

    return 0;
}
