#include "SwitchStorageBridge.h"

#include "SwitchStorage.h"

#include <switch.h>

#include <cstring>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <utility>

namespace
{
bool s_initialized = false;
bool s_socketInitialized = false;

bool regularFileExists(const char* path)
{
	struct stat info{};
	return path && stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

bool waitForUsbPath(char* gamePath, size_t gamePathSize)
{
	const char* colon = std::strchr(gamePath, ':');
	if (!colon)
		return false;
	std::string relative = colon + 1;
	while (!relative.empty() && relative.front() == '/')
		relative.erase(relative.begin());

	for (int attempt = 0; attempt < 100; ++attempt)
	{
		if (regularFileExists(gamePath))
			return true;
		std::string match;
		for (const auto& location : SwitchStorage::ListUsbLocations())
		{
			std::string candidate = location.path;
			if (!candidate.empty() && candidate.back() != '/')
				candidate += '/';
			candidate += relative;
			if (!regularFileExists(candidate.c_str()))
				continue;
			if (!match.empty())
			{
				match.clear();
				break;
			}
			match = std::move(candidate);
		}
		if (!match.empty())
		{
			const int length = std::snprintf(gamePath, gamePathSize, "%s", match.c_str());
			return length >= 0 && static_cast<size_t>(length) < gamePathSize;
		}
		svcSleepThread(50'000'000);
	}
	return false;
}
}

extern "C" bool switchStorageInitializeForPath(const char* iniPath, char* gamePath,
                                                size_t gamePathSize, char* outputError,
                                                size_t outputErrorSize)
{
	if (outputError && outputErrorSize)
		outputError[0] = '\0';
	auto fail = [&](const std::string& message) {
		if (outputError && outputErrorSize)
			std::snprintf(outputError, outputErrorSize, "%s", message.c_str());
		return false;
	};
	if (s_initialized)
		return true;
	if (!gamePath || !*gamePath)
		return true;
	if (!gamePathSize)
		return fail("The game path buffer is invalid");

	const bool usbPath = std::strncmp(gamePath, "ums", 3) == 0;
	const bool smbPath = std::strncmp(gamePath, "nxsmb_", 6) == 0;
	if (!usbPath && !smbPath)
		return true;

	std::string error;
	if (usbPath && !SwitchStorage::InitializeUsb(&error))
		return fail(error.empty() ? "USB storage initialization failed" : error);
	if (usbPath && !waitForUsbPath(gamePath, gamePathSize))
	{
		SwitchStorage::Shutdown();
		return fail("The USB game path is unavailable");
	}

	if (smbPath)
	{
		s_socketInitialized = R_SUCCEEDED(socketInitializeDefault());
		if (!s_socketInitialized)
			return fail("Network initialization failed");
		bool mounted = false;
		for (const auto& share : SwitchStorage::LoadSmbShares(iniPath ? iniPath : ""))
		{
			const std::string root = SwitchStorage::SmbRootPath(share.id);
			if (root.empty() || std::strncmp(gamePath, root.c_str(), root.size()) != 0)
				continue;
			mounted = SwitchStorage::MountSmb(share, &error);
			break;
		}
		if (!mounted)
		{
			SwitchStorage::Shutdown();
			socketExit();
			s_socketInitialized = false;
			return fail(error.empty() ? "The configured SMB share was not found" : error);
		}
	}

	s_initialized = true;
	return true;
}

extern "C" bool switchStorageSocketReady(void)
{
	return s_socketInitialized;
}

extern "C" void switchStorageShutdown(void)
{
	if (!s_initialized && !s_socketInitialized)
		return;
	SwitchStorage::Shutdown();
	if (s_socketInitialized)
		socketExit();
	s_socketInitialized = false;
	s_initialized = false;
}
