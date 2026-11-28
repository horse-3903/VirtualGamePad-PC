#include "adapterinfo.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <windows.h>

#include <vector>

// The device a network connection belongs to is stored under its adapter GUID, for example PCI\VEN_8086... for a
// built-in card and USB\VID_04E8... for a phone's USB tethering.
static QString deviceBus(const wchar_t *adapterGuid)
{
	wchar_t path[256];
	swprintf(path, 256, L"SYSTEM\\CurrentControlSet\\Control\\Network\\{4D36E972-E325-11CE-BFC1-08002BE10318}\\%ls\\Connection", adapterGuid);
	wchar_t value[512] = {};
	DWORD size = sizeof(value);
	if (RegGetValueW(HKEY_LOCAL_MACHINE, path, L"PnpInstanceID", RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS)
		return QString();
	const QString id = QString::fromWCharArray(value);
	const int slash = id.indexOf(QLatin1Char('\\'));
	return (slash > 0 ? id.left(slash) : id).toUpper();
}

QHash<QString, AdapterInfo> adapterInfoByAddress()
{
	QHash<QString, AdapterInfo> result;
	ULONG size = 0;
	GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, nullptr, &size);
	if (size == 0)
		return result;
	std::vector<unsigned char> buffer(size);
	auto *first = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
	if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, first, &size) != NO_ERROR)
		return result;
	for (IP_ADAPTER_ADDRESSES *adapter = first; adapter != nullptr; adapter = adapter->Next)
	{
		AdapterInfo info;
		info.known = true;
		info.name = QString::fromWCharArray(adapter->FriendlyName);
		info.ifType = static_cast<int>(adapter->IfType);
		MIB_IF_ROW2 row{};
		row.InterfaceIndex = adapter->IfIndex;
		if (GetIfEntry2(&row) == NO_ERROR)
			info.hardware = row.InterfaceAndOperStatusFlags.HardwareInterface != 0;
		info.bus = deviceBus(QString::fromLatin1(adapter->AdapterName).toStdWString().c_str());
		for (IP_ADAPTER_UNICAST_ADDRESS *unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next)
		{
			if (unicast->Address.lpSockaddr == nullptr || unicast->Address.lpSockaddr->sa_family != AF_INET)
				continue;
			char text[INET_ADDRSTRLEN] = {};
			const auto *address = reinterpret_cast<const sockaddr_in *>(unicast->Address.lpSockaddr);
			if (inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text)) != nullptr)
				result.insert(QString::fromLatin1(text), info);
		}
	}
	return result;
}
#else
QHash<QString, AdapterInfo> adapterInfoByAddress()
{
	return {};
}
#endif
