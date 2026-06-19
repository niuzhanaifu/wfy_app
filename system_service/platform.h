#ifndef SYSTEM_SERVICE_PLATFORM_H
#define SYSTEM_SERVICE_PLATFORM_H

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(SYSTEM_SERVICE_PLATFORM_A733) && SYSTEM_SERVICE_PLATFORM_A733
#define SYSTEM_SERVICE_IS_A733 1
#else
#define SYSTEM_SERVICE_IS_A733 0
#endif

#if !defined(SYSTEM_SERVICE_PLATFORM_RK3566)
#define SYSTEM_SERVICE_PLATFORM_RK3566 (!SYSTEM_SERVICE_IS_A733)
#endif

#ifndef SYSTEM_SERVICE_LOG_ROOT
#if SYSTEM_SERVICE_IS_A733
#define SYSTEM_SERVICE_LOG_ROOT "/home/radxa/blackbox"
#else
#define SYSTEM_SERVICE_LOG_ROOT "/blackbox"
#endif
#endif

#ifndef SYSTEM_SERVICE_WIFI_IFACE
#define SYSTEM_SERVICE_WIFI_IFACE "wlan0"
#endif

#ifndef SYSTEM_SERVICE_HOME_WIFI_CONF
#define SYSTEM_SERVICE_HOME_WIFI_CONF "/etc/taishan_home_wifi.conf"
#endif

#ifndef SYSTEM_SERVICE_HOSTAPD_CONF
#define SYSTEM_SERVICE_HOSTAPD_CONF "/etc/hostapd.conf"
#endif

#ifndef SYSTEM_SERVICE_BLE_NAME
#define SYSTEM_SERVICE_BLE_NAME "WFY"
#endif

#ifndef SYSTEM_SERVICE_AP_SSID
#define SYSTEM_SERVICE_AP_SSID "WFY_TEST"
#endif

#ifndef SYSTEM_SERVICE_AP_PASSWD
#define SYSTEM_SERVICE_AP_PASSWD "12345678"
#endif

#ifndef SYSTEM_SERVICE_NM_STA_CONN
#define SYSTEM_SERVICE_NM_STA_CONN "system_service_sta"
#endif

#ifndef SYSTEM_SERVICE_NM_AP_CONN
#define SYSTEM_SERVICE_NM_AP_CONN "system_service_ap"
#endif

#ifndef SYSTEM_SERVICE_NUM_CPUS
#if SYSTEM_SERVICE_IS_A733
#define SYSTEM_SERVICE_NUM_CPUS 8
#else
#define SYSTEM_SERVICE_NUM_CPUS 4
#endif
#endif

static inline int system_service_mkdir_p(const char *path, mode_t mode)
{
	char tmp[512];
	size_t len, i;

	if (!path || !path[0])
		return -1;

	len = strlen(path);
	if (len >= sizeof(tmp))
		return -1;

	memcpy(tmp, path, len + 1);

	for (i = 1; i < len; i++) {
		if (tmp[i] != '/')
			continue;
		tmp[i] = '\0';
		if (tmp[0] && mkdir(tmp, mode) != 0 && errno != EEXIST)
			return -1;
		tmp[i] = '/';
	}

	if (mkdir(tmp, mode) != 0 && errno != EEXIST)
		return -1;
	return 0;
}

#endif
