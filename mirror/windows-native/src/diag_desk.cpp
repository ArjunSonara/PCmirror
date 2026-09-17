#include <windows.h>
#include <stdio.h>

int main() {
    HDESK hDesk = OpenInputDesktop(0, FALSE, GENERIC_READ);
    if (hDesk == NULL) {
        DWORD err = GetLastError();
        printf("OpenInputDesktop failed: err=%u (0x%08X)\n", err, err);
    } else {
        printf("OpenInputDesktop succeeded!\n");
        CloseDesktop(hDesk);
    }

    HWND hwnd = GetForegroundWindow();
    printf("GetForegroundWindow: %p\n", hwnd);
    if (hwnd) {
        wchar_t title[256] = {};
        GetWindowTextW(hwnd, title, 255);
        wprintf(L"Foreground window title: %s\n", title);
    }

    return 0;
}
