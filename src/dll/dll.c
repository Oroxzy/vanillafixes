#include <windows.h>
#include <shlwapi.h>
#include <stdlib.h>
#include <timeapi.h>
#include <intrin.h>

#include <MinHook.h>

#include "macros.h"

#include "loader.h"
#include "memory.h"
#include "os.h"
#include "tsc.h"

// Addresses found by SearchMemory procedure
static VF_ADDRESSES g_addresses;

// Handle to this module, for unloading on exit
static HMODULE g_hSelf = NULL;

// Flag to indicate whether the main thread has finished execution
static volatile BOOL g_mainThreadFinished = FALSE;

// Hooked OsTimeManager::TimeKeeper to apply our changes and prevent it from running in the background during gameplay
DWORD WINAPI VfTimeKeeperThreadProc(LPVOID lpParameter) {
	// Increase the process timer resolution up to a cap of 0.5 ms
	IncreaseTimerResolution(5000);

	// Disable Windows 11 power throttling for the process
	SetPowerThrottlingState(PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, FALSE);
	SetPowerThrottlingState(PROCESS_POWER_THROTTLING_EXECUTION_SPEED, FALSE);

	// Calibrate the game timer using QueryPerformanceFrequency
	*g_addresses.pTimerTicksPerSecond = CalibrateTSC();
	DebugOutputF(L"timerTicksPerSecond=%lld", *g_addresses.pTimerTicksPerSecond);
	*g_addresses.pTimerToMilliseconds = 1000.0 / *g_addresses.pTimerTicksPerSecond;
	*g_addresses.pUseTSC = TRUE;

	// Align TSC with GTC
	*g_addresses.pTimerOffset = GetTickCount() - (__rdtsc() * *g_addresses.pTimerToMilliseconds);
	DebugOutputF(L"timerOffset=%.0lf", *g_addresses.pTimerOffset);

	while(!g_mainThreadFinished) {
		Sleep(1);
	}

	DebugOutputF(L"Done! Unloading from process %lu...", GetCurrentProcessId());
	FreeLibraryAndExitThread(g_hSelf, 0);
}

typedef DWORD (*PLOAD)();

// Function to call load functions in DLLs loaded by the included launcher
void InitAdditionalDLLs() {
	LPWSTR pWowDirectory = malloc(MAX_PATH * sizeof(WCHAR));
	GetModuleFileName(NULL, pWowDirectory, MAX_PATH);
	// Remove the file name from the directory path
	PathRemoveFileSpec(pWowDirectory);

	LPWSTR pConfigPath = malloc(MAX_PATH * sizeof(WCHAR));
	PathCombine(pConfigPath, pWowDirectory, L"dlls.txt");

	// Obtain a list of any additional DLLs that should have been loaded by the launcher
	VF_DLL_LIST_PARSE_DATA dllListData = {0};
	LoaderParseConfig(pWowDirectory, pConfigPath, &dllListData);

	if(dllListData.pAdditionalDLLs) {
		for(int i = 0; i < dllListData.nAdditionalDLLs; i++) {
			// Is the DLL loaded into the process?
			HMODULE moduleHandle = GetModuleHandle(dllListData.pAdditionalDLLs[i]);

			if(moduleHandle) {
				DebugOutputF(L"Handle was found for %ls", dllListData.pAdditionalDLLs[i]);
				PLOAD pLoad = (PLOAD)GetProcAddress(moduleHandle, "Load");
				// Does this DLL export a load function?
				if(pLoad) {
					DWORD result = pLoad();
					DebugOutputF(L"Load function was found for %ls, result=%d", dllListData.pAdditionalDLLs[i], result);

					// Did the load function return an error?
					if(result) {
						MessageBoxF(L"DLL %ls was not loaded properly (load function failed)",
							dllListData.pAdditionalDLLs[i]);
					}
				}
			}
			else {
				MessageBoxF(L"DLL %ls was not loaded properly (no module handle)",
					dllListData.pAdditionalDLLs[i]);
			}

			free(dllListData.pAdditionalDLLs[i]);
		}

		free(dllListData.pAdditionalDLLs);
	}

	free(pConfigPath);
	free(pWowDirectory);
}

// Hooked function on main thread to perform initialization and prevent the game from hanging
DWORD64 VfGetCPUFrequency() {
	// The game has built-in UI scaling which means Windows DPI scaling is unnecessary
	// DXVK already enabled this by default (d3d9.dpiAware)
	EnableDPIAwareness();
	// Check for and init additional DLLs loaded by the launcher
	InitAdditionalDLLs();

	// Wait until all threads have finished
	while(!*g_addresses.pUseTSC) {
		Sleep(1);
	}

	g_mainThreadFinished = TRUE;

	// The game assumes the TSC frequency is equal to the CPU frequency
	return *g_addresses.pTimerTicksPerSecond;
}

// Initialization function for VanillaFixes
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
	g_hSelf = hinstDLL;

	switch(fdwReason) {
		case DLL_PROCESS_ATTACH: {
			// Search for addresses before the game has a chance to write to memory
			DWORD startTime = timeGetTime();
			if(!ScanMemory(&g_addresses)) {
				return FALSE;
			}

			DWORD scanTime = timeGetTime() - startTime;
			DebugOutputF(L"Searching for addresses took %d ms", scanTime);
			DebugOutputF(L"pTimeKeeperThreadProc=0x%p", g_addresses.pTimeKeeperThreadProc);
			DebugOutputF(L"pGetCPUFrequency=0x%p", g_addresses.pGetCPUFrequency);
			DebugOutputF(L"pUseTSC=0x%p", g_addresses.pUseTSC);
			DebugOutputF(L"pTimerTicksPerSecond=0x%p", g_addresses.pTimerTicksPerSecond);
			DebugOutputF(L"pTimerToMilliseconds=0x%p", g_addresses.pTimerToMilliseconds);
			DebugOutputF(L"pTimerOffset=0x%p", g_addresses.pTimerOffset);

			if(MH_Initialize() != MH_OK) {
				return FALSE;
			}

			// Hook OsTimeManager::TimeKeeper before the thread is created
			if(MH_CreateHook(g_addresses.pTimeKeeperThreadProc, &VfTimeKeeperThreadProc, NULL) != MH_OK) {
				return FALSE;
			}
			// Hook function on the main thread to perform extra tasks
			if(MH_CreateHook(g_addresses.pGetCPUFrequency, &VfGetCPUFrequency, NULL) != MH_OK) {
				return FALSE;
			}

			if(MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
				return FALSE;
			}

			break;
		}
		case DLL_PROCESS_DETACH: {
			MH_DisableHook(MH_ALL_HOOKS);
			MH_Uninitialize();
			break;
		}
	}

	return TRUE;
}
