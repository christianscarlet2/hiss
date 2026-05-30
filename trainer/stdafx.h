#pragma once

#ifndef VC_EXTRALEAN
#define VC_EXTRALEAN
#endif

// WinSock2 must precede windows.h (pulled in by MFC) to avoid winsock v1.
#include <winsock2.h>
#include <ws2tcpip.h>

#include <afxwin.h>
#include <afxext.h>
#include <afxcmn.h>
#include <afxdlgs.h>

#include <vector>
#include <string>

// OpenCV + Tesseract (paths come from the project's IncludePath: ..\ocr*)
#include <allheaders.h>
#include <baseapi.h>
#include <opencv.hpp>
#include <imgproc/imgproc.hpp>
