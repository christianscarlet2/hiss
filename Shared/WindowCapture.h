//******************************************************************************
//
// Shared window capture helpers.
// Windows 11/DWM friendly replacement for direct GetDC(hwnd)+BitBlt scraping.
//
//******************************************************************************

#pragma once

#include <windows.h>

#ifndef DWMWA_EXTENDED_FRAME_BOUNDS
#define DWMWA_EXTENDED_FRAME_BOUNDS 9
#endif

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

#ifndef PW_CLIENTONLY
#define PW_CLIENTONLY 0x00000001
#endif

namespace WindowCapture {

typedef HRESULT (WINAPI *DwmGetWindowAttributeFn)(HWND, DWORD, PVOID, DWORD);
typedef BOOL (WINAPI *PrintWindowFn)(HWND, HDC, UINT);
typedef HANDLE (WINAPI *SetThreadDpiAwarenessContextFn)(HANDLE);

class ScopedDpiAwareness {
public:
  ScopedDpiAwareness()
    : _set_thread_dpi_awareness_context(NULL),
      _previous(NULL) {
    HMODULE user32 = GetModuleHandle(TEXT("user32.dll"));
    if (user32 != NULL) {
      _set_thread_dpi_awareness_context =
        (SetThreadDpiAwarenessContextFn)GetProcAddress(user32, "SetThreadDpiAwarenessContext");
      if (_set_thread_dpi_awareness_context != NULL) {
        _previous = _set_thread_dpi_awareness_context((HANDLE)-4);
      }
    }
  }

  ~ScopedDpiAwareness() {
    if ((_set_thread_dpi_awareness_context != NULL) && (_previous != NULL)) {
      _set_thread_dpi_awareness_context(_previous);
    }
  }

private:
  SetThreadDpiAwarenessContextFn _set_thread_dpi_awareness_context;
  HANDLE _previous;
};

inline int Width(const RECT &rect) {
  return rect.right - rect.left;
}

inline int Height(const RECT &rect) {
  return rect.bottom - rect.top;
}

inline bool GetFullWindowScreenRect(HWND hwnd, RECT *rect) {
  if ((hwnd == NULL) || (rect == NULL) || !IsWindow(hwnd)) {
    return false;
  }

  ScopedDpiAwareness dpi_awareness;
  ZeroMemory(rect, sizeof(*rect));

  HMODULE dwmapi = LoadLibrary(TEXT("dwmapi.dll"));
  if (dwmapi != NULL) {
    DwmGetWindowAttributeFn dwm_get_window_attribute =
      (DwmGetWindowAttributeFn)GetProcAddress(dwmapi, "DwmGetWindowAttribute");
    if (dwm_get_window_attribute != NULL) {
      RECT frame_rect = { 0 };
      HRESULT result = dwm_get_window_attribute(
        hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame_rect, sizeof(frame_rect));
      if (SUCCEEDED(result) && (Width(frame_rect) > 0) && (Height(frame_rect) > 0)) {
        *rect = frame_rect;
        FreeLibrary(dwmapi);
        return true;
      }
    }
    FreeLibrary(dwmapi);
  }

  return GetWindowRect(hwnd, rect) && (Width(*rect) > 0) && (Height(*rect) > 0);
}

inline bool GetFullWindowCaptureRect(HWND hwnd, RECT *rect) {
  RECT screen_rect = { 0 };
  if (!GetFullWindowScreenRect(hwnd, &screen_rect)) {
    return false;
  }
  rect->left = 0;
  rect->top = 0;
  rect->right = Width(screen_rect);
  rect->bottom = Height(screen_rect);
  return true;
}

inline bool GetClientScreenRect(HWND hwnd, RECT *rect) {
  if ((hwnd == NULL) || (rect == NULL) || !IsWindow(hwnd)) {
    return false;
  }

  ScopedDpiAwareness dpi_awareness;
  RECT client_rect = { 0 };
  if (!GetClientRect(hwnd, &client_rect)) {
    return false;
  }

  POINT upper_left = { client_rect.left, client_rect.top };
  POINT lower_right = { client_rect.right, client_rect.bottom };
  if (!ClientToScreen(hwnd, &upper_left) || !ClientToScreen(hwnd, &lower_right)) {
    return false;
  }

  rect->left = upper_left.x;
  rect->top = upper_left.y;
  rect->right = lower_right.x;
  rect->bottom = lower_right.y;
  return (Width(*rect) > 0) && (Height(*rect) > 0);
}

inline bool GetClientCaptureRect(HWND hwnd, RECT *rect) {
  if ((hwnd == NULL) || (rect == NULL) || !IsWindow(hwnd)) {
    return false;
  }

  ScopedDpiAwareness dpi_awareness;
  RECT client_rect = { 0 };
  if (!GetClientRect(hwnd, &client_rect)) {
    return false;
  }

  rect->left = 0;
  rect->top = 0;
  rect->right = Width(client_rect);
  rect->bottom = Height(client_rect);
  return (Width(*rect) > 0) && (Height(*rect) > 0);
}

inline HBITMAP CreateDIBSectionBitmap(int width, int height, void **bits) {
  if ((width <= 0) || (height <= 0)) {
    return NULL;
  }

  BITMAPINFO bmi;
  ZeroMemory(&bmi, sizeof(bmi));
  bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
  bmi.bmiHeader.biWidth = width;
  bmi.bmiHeader.biHeight = -height;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  bmi.bmiHeader.biSizeImage = width * height * 4;

  void *local_bits = NULL;
  void **section_bits = (bits != NULL) ? bits : &local_bits;
  HDC desktop_dc = GetDC(NULL);
  HBITMAP bitmap = CreateDIBSection(desktop_dc, &bmi, DIB_RGB_COLORS, section_bits, NULL, 0);
  ReleaseDC(NULL, desktop_dc);
  return bitmap;
}

inline bool CopyBitmap(HBITMAP destination, HBITMAP source, int width, int height) {
  if ((destination == NULL) || (source == NULL) || (width <= 0) || (height <= 0)) {
    return false;
  }

  HDC display_dc = CreateDC(TEXT("DISPLAY"), NULL, NULL, NULL);
  HDC source_dc = CreateCompatibleDC(display_dc);
  HDC destination_dc = CreateCompatibleDC(display_dc);
  HBITMAP old_source = (HBITMAP)SelectObject(source_dc, source);
  HBITMAP old_destination = (HBITMAP)SelectObject(destination_dc, destination);
  BOOL copied = BitBlt(destination_dc, 0, 0, width, height, source_dc, 0, 0, SRCCOPY);
  SelectObject(source_dc, old_source);
  SelectObject(destination_dc, old_destination);
  DeleteDC(destination_dc);
  DeleteDC(source_dc);
  DeleteDC(display_dc);
  return copied != FALSE;
}

inline bool BitmapLooksUseful(HBITMAP bitmap, int width, int height) {
  if ((bitmap == NULL) || (width <= 0) || (height <= 0)) {
    return false;
  }

  HDC display_dc = CreateDC(TEXT("DISPLAY"), NULL, NULL, NULL);
  HDC memory_dc = CreateCompatibleDC(display_dc);
  HBITMAP old_bitmap = (HBITMAP)SelectObject(memory_dc, bitmap);

  BITMAPINFO bmi;
  ZeroMemory(&bmi, sizeof(bmi));
  bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
  bmi.bmiHeader.biWidth = width;
  bmi.bmiHeader.biHeight = -height;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  int image_size = width * height * 4;
  BYTE *sample_bits = new BYTE[image_size];
  ZeroMemory(sample_bits, image_size);
  bool useful = false;
  if (GetDIBits(memory_dc, bitmap, 0, height, sample_bits, &bmi, DIB_RGB_COLORS) != 0) {
    DWORD first_pixel = *((DWORD*)sample_bits);
    int different_pixels = 0;
    int non_black_pixels = 0;
    int total_pixels = width * height;
    int step = (total_pixels / 4096) + 1;
    for (int i = 0; i < total_pixels; i += step) {
      DWORD pixel = ((DWORD*)sample_bits)[i] & 0x00ffffff;
      if (pixel != (first_pixel & 0x00ffffff)) {
        ++different_pixels;
      }
      if (pixel != 0) {
        ++non_black_pixels;
      }
      if ((different_pixels > 4) || (non_black_pixels > 16)) {
        useful = true;
        break;
      }
    }
  }

  delete[] sample_bits;
  SelectObject(memory_dc, old_bitmap);
  DeleteDC(memory_dc);
  DeleteDC(display_dc);
  return useful;
}

inline bool CaptureDesktopPixels(HWND hwnd, HBITMAP bitmap, int width, int height) {
  RECT screen_rect = { 0 };
  if (!GetFullWindowScreenRect(hwnd, &screen_rect)) {
    return false;
  }

  HDC screen_dc = GetDC(NULL);
  HDC memory_dc = CreateCompatibleDC(screen_dc);
  HBITMAP old_bitmap = (HBITMAP)SelectObject(memory_dc, bitmap);
  BOOL copied = BitBlt(memory_dc, 0, 0, width, height,
    screen_dc, screen_rect.left, screen_rect.top, SRCCOPY | CAPTUREBLT);
  SelectObject(memory_dc, old_bitmap);
  DeleteDC(memory_dc);
  ReleaseDC(NULL, screen_dc);
  return copied && BitmapLooksUseful(bitmap, width, height);
}

inline bool CaptureClientDesktopPixels(HWND hwnd, HBITMAP bitmap, int width, int height) {
  RECT screen_rect = { 0 };
  if (!GetClientScreenRect(hwnd, &screen_rect)) {
    return false;
  }

  HDC screen_dc = GetDC(NULL);
  HDC memory_dc = CreateCompatibleDC(screen_dc);
  HBITMAP old_bitmap = (HBITMAP)SelectObject(memory_dc, bitmap);
  BOOL copied = BitBlt(memory_dc, 0, 0, width, height,
    screen_dc, screen_rect.left, screen_rect.top, SRCCOPY | CAPTUREBLT);
  SelectObject(memory_dc, old_bitmap);
  DeleteDC(memory_dc);
  ReleaseDC(NULL, screen_dc);
  return copied && BitmapLooksUseful(bitmap, width, height);
}

inline bool CapturePrintWindow(HWND hwnd, HBITMAP bitmap, int width, int height) {
  HMODULE user32 = GetModuleHandle(TEXT("user32.dll"));
  if (user32 == NULL) {
    return false;
  }

  PrintWindowFn print_window = (PrintWindowFn)GetProcAddress(user32, "PrintWindow");
  if (print_window == NULL) {
    return false;
  }

  HDC display_dc = CreateDC(TEXT("DISPLAY"), NULL, NULL, NULL);
  HDC memory_dc = CreateCompatibleDC(display_dc);
  HBITMAP old_bitmap = (HBITMAP)SelectObject(memory_dc, bitmap);
  PatBlt(memory_dc, 0, 0, width, height, BLACKNESS);
  BOOL printed = print_window(hwnd, memory_dc, PW_RENDERFULLCONTENT);
  if (!printed) {
    PatBlt(memory_dc, 0, 0, width, height, BLACKNESS);
    printed = print_window(hwnd, memory_dc, 0);
  }
  SelectObject(memory_dc, old_bitmap);
  DeleteDC(memory_dc);
  DeleteDC(display_dc);
  return printed && BitmapLooksUseful(bitmap, width, height);
}

inline bool CaptureClientPrintWindow(HWND hwnd, HBITMAP bitmap, int width, int height) {
  HMODULE user32 = GetModuleHandle(TEXT("user32.dll"));
  if (user32 == NULL) {
    return false;
  }

  PrintWindowFn print_window = (PrintWindowFn)GetProcAddress(user32, "PrintWindow");
  if (print_window == NULL) {
    return false;
  }

  HDC display_dc = CreateDC(TEXT("DISPLAY"), NULL, NULL, NULL);
  HDC memory_dc = CreateCompatibleDC(display_dc);
  HBITMAP old_bitmap = (HBITMAP)SelectObject(memory_dc, bitmap);
  PatBlt(memory_dc, 0, 0, width, height, BLACKNESS);
  BOOL printed = print_window(hwnd, memory_dc, PW_CLIENTONLY | PW_RENDERFULLCONTENT);
  if (!printed) {
    PatBlt(memory_dc, 0, 0, width, height, BLACKNESS);
    printed = print_window(hwnd, memory_dc, PW_CLIENTONLY);
  }
  SelectObject(memory_dc, old_bitmap);
  DeleteDC(memory_dc);
  DeleteDC(display_dc);
  return printed && BitmapLooksUseful(bitmap, width, height);
}

inline bool CaptureWindowIntoBitmap(HWND hwnd, HBITMAP bitmap, int width, int height, bool print_window_first) {
  if ((hwnd == NULL) || (bitmap == NULL) || (width <= 0) || (height <= 0)) {
    return false;
  }

  ScopedDpiAwareness dpi_awareness;
  if (print_window_first) {
    if (CapturePrintWindow(hwnd, bitmap, width, height)) {
      return true;
    }
    return CaptureDesktopPixels(hwnd, bitmap, width, height);
  }

  if (CaptureDesktopPixels(hwnd, bitmap, width, height)) {
    return true;
  }
  return CapturePrintWindow(hwnd, bitmap, width, height);
}

inline bool CaptureClientIntoBitmap(HWND hwnd, HBITMAP bitmap, int width, int height, bool print_window_first) {
  if ((hwnd == NULL) || (bitmap == NULL) || (width <= 0) || (height <= 0)) {
    return false;
  }

  ScopedDpiAwareness dpi_awareness;
  if (print_window_first) {
    if (CaptureClientPrintWindow(hwnd, bitmap, width, height)) {
      return true;
    }
    return CaptureClientDesktopPixels(hwnd, bitmap, width, height);
  }

  if (CaptureClientDesktopPixels(hwnd, bitmap, width, height)) {
    return true;
  }
  return CaptureClientPrintWindow(hwnd, bitmap, width, height);
}

inline HBITMAP CaptureWindowBitmap(HWND hwnd, int *width, int *height, bool print_window_first) {
  RECT rect = { 0 };
  if (!GetFullWindowCaptureRect(hwnd, &rect)) {
    return NULL;
  }

  int capture_width = Width(rect);
  int capture_height = Height(rect);
  void *bits = NULL;
  HBITMAP bitmap = CreateDIBSectionBitmap(capture_width, capture_height, &bits);
  if (bitmap == NULL) {
    return NULL;
  }

  if (!CaptureWindowIntoBitmap(hwnd, bitmap, capture_width, capture_height, print_window_first)) {
    DeleteObject(bitmap);
    return NULL;
  }

  if (width != NULL) {
    *width = capture_width;
  }
  if (height != NULL) {
    *height = capture_height;
  }
  return bitmap;
}

inline HBITMAP CaptureClientBitmap(HWND hwnd, int *width, int *height, bool print_window_first) {
  RECT rect = { 0 };
  if (!GetClientCaptureRect(hwnd, &rect)) {
    return NULL;
  }

  int capture_width = Width(rect);
  int capture_height = Height(rect);
  void *bits = NULL;
  HBITMAP bitmap = CreateDIBSectionBitmap(capture_width, capture_height, &bits);
  if (bitmap == NULL) {
    return NULL;
  }

  if (!CaptureClientIntoBitmap(hwnd, bitmap, capture_width, capture_height, print_window_first)) {
    DeleteObject(bitmap);
    return NULL;
  }

  if (width != NULL) {
    *width = capture_width;
  }
  if (height != NULL) {
    *height = capture_height;
  }
  return bitmap;
}

}  // namespace WindowCapture

inline bool GetFullWindowCaptureRect(HWND hwnd, RECT *rect) {
  return WindowCapture::GetFullWindowCaptureRect(hwnd, rect);
}

inline bool GetClientWindowCaptureRect(HWND hwnd, RECT *rect) {
  return WindowCapture::GetClientCaptureRect(hwnd, rect);
}

inline HBITMAP WindowCaptureCreateDIBSection(int width, int height, void **bits) {
  return WindowCapture::CreateDIBSectionBitmap(width, height, bits);
}

inline bool CaptureBestEffortWindowIntoBitmap(HWND hwnd, HBITMAP bitmap, int width, int height) {
  return WindowCapture::CaptureWindowIntoBitmap(hwnd, bitmap, width, height, true);
}

inline bool CaptureBestEffortClientIntoBitmap(HWND hwnd, HBITMAP bitmap, int width, int height) {
  return WindowCapture::CaptureClientIntoBitmap(hwnd, bitmap, width, height, true);
}

inline bool CaptureCompositedWindowIntoBitmap(HWND hwnd, HBITMAP bitmap, int width, int height) {
  return WindowCapture::CaptureWindowIntoBitmap(hwnd, bitmap, width, height, true);
}

inline bool CaptureCompositedClientIntoBitmap(HWND hwnd, HBITMAP bitmap, int width, int height) {
  return WindowCapture::CaptureClientIntoBitmap(hwnd, bitmap, width, height, true);
}

inline HBITMAP CaptureBestEffortWindowBitmap(HWND hwnd, int *width, int *height) {
  return WindowCapture::CaptureWindowBitmap(hwnd, width, height, true);
}

inline HBITMAP CaptureBestEffortClientBitmap(HWND hwnd, int *width, int *height) {
  return WindowCapture::CaptureClientBitmap(hwnd, width, height, true);
}

inline HBITMAP CaptureCompositedWindowBitmap(HWND hwnd, int *width, int *height) {
  return WindowCapture::CaptureWindowBitmap(hwnd, width, height, true);
}

inline HBITMAP CaptureCompositedClientBitmap(HWND hwnd, int *width, int *height) {
  return WindowCapture::CaptureClientBitmap(hwnd, width, height, true);
}

inline bool WindowCaptureCopyBitmap(HBITMAP destination, HBITMAP source, int width, int height) {
  return WindowCapture::CopyBitmap(destination, source, width, height);
}
