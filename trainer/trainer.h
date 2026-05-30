#pragma once

#include "resource.h"

class CTrainerApp : public CWinApp {
public:
	CTrainerApp();
	virtual BOOL InitInstance();
};

extern CTrainerApp theApp;
