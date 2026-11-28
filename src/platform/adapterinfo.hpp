#pragma once

#include <QHash>
#include <QString>

// What Windows itself says about a network adapter, so the server can tell a wired port from a phone's USB
// tethering, a VPN or a virtual switch without guessing from names.
struct AdapterInfo
{
	bool known = false;
	QString name;     // the name shown in network settings
	int ifType = 0;   // IANA interface type: 6 Ethernet, 71 Wi-Fi, 53 virtual, 131 tunnel
	bool hardware = false; // a physical network card, as opposed to software
	QString bus;      // where the device sits: PCI, USB, BTH (Bluetooth), SWD or ROOT (software), or empty
};

// Keyed by IPv4 address. Empty on platforms where the information is not available.
QHash<QString, AdapterInfo> adapterInfoByAddress();
