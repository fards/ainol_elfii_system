/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/*
 * [WIFI] Enable Wi-Fi Support, Please reference [PROJECT]/device/$(OEM)/$(TARGET_DEVICE)/BoardConfig.mk
 */

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <linux/wireless.h>

#include <openssl/evp.h>
#include <openssl/sha.h>

#define LOG_TAG "SoftapController"
#include <cutils/log.h>

#include "SoftapController.h"


//#define CONFIG_DAEMON_CMD_WITH_PARA
#define CONFIG_WLAN_RTK_WIFI_HOSTAPD
#ifdef CONFIG_WLAN_RTK_WIFI_HOSTAPD     /* [WIFI] Wi-Fi Support ++ */

extern void apNotifyInterfaceChanged(const char *name, bool isUp);

#include <ctype.h>
#include "private/android_filesystem_config.h"
#include "cutils/properties.h"
#ifdef HAVE_LIBC_SYSTEM_PROPERTIES
#define _REALLY_INCLUDE_SYS__SYSTEM_PROPERTIES_H_
#endif

#include <sys/_system_properties.h>
#include <libhostapd_client/wpa_ctrl.h>

#define WIFI_DRIVER_IFNAME_AP		"wlap%d"

extern "C" int wifi_unload_driver();
int ensure_config_file_exists(const char *conf_file, const char *tmplate);
char *getWifiIfname();
//extern "C" char ifname[PROPERTY_VALUE_MAX];

static const char HOSTAPD_NAME[]				= "hostapd";
static const char HOSTAPD_PROP_NAME[]		= "init.svc.hostapd";
static const char HOSTAPD_CTRL_DIR[]			= "/data/misc/wifi/hostapd";
static const char HOSTAPD_CONFIG_TEMPLATE[]	= "/system/etc/wifi/hostapd.conf";
static const char HOSTAPD_CONFIG_FILE[]    		= "/data/misc/wifi/hostapd.conf";

static const char MODULE_FILE[]	= "/proc/modules";
static char g_ifname[PROPERTY_VALUE_MAX];

#define WIFI_TEST_INTERFACE     "sta"
#define WIFI_DEFAULT_BI         100         /* in TU */
#define WIFI_DEFAULT_DTIM       1           /* in beacon */
#define WIFI_DEFAULT_CHANNEL    6
#define WIFI_DEFAULT_MAX_STA    8
#define WIFI_DEFAULT_PREAMBLE   0

static struct wpa_ctrl *ctrl_conn;

int mProfileValid;

int get_priv_func_num(int sockfd, const char *ifname, const char *fname) {
	struct iwreq wrq;
	struct iw_priv_args *priv_ptr;
	int i, ret;
	char *buf;

	if( NULL == (buf=(char *)malloc(4096)) ) {
		ret = -ENOMEM;
		goto exit;
	}

	strncpy(wrq.ifr_name, ifname, sizeof(wrq.ifr_name));
	wrq.u.data.pointer = buf;
	wrq.u.data.length = 4096 / sizeof(struct iw_priv_args);
	wrq.u.data.flags = 0;
	if ((ret = ioctl(sockfd, SIOCGIWPRIV, &wrq)) < 0) {
		LOGE("SIOCGIPRIV failed: %d %s", ret, strerror(errno));
		goto exit;
	}

	ret =-1;
	priv_ptr = (struct iw_priv_args *)wrq.u.data.pointer;
	for(i=0;(i < wrq.u.data.length);i++) {
		if (strcmp(priv_ptr[i].name, fname) == 0) {
			ret = priv_ptr[i].cmd;
			break;
		}
	}

exit:
	if(buf)
		free(buf);

	return ret;
}


int rtl871x_drv_rereg_nd_name_fd(int sockfd, const char *ifname, const int fnum, const char * new_ifname)
{
	struct iwreq wrq;
	int ret;
	char ifname_buf[IFNAMSIZ];
	strncpy(wrq.ifr_name, ifname, sizeof(wrq.ifr_name));
	strncpy(ifname_buf, new_ifname, IFNAMSIZ);
	ifname_buf[IFNAMSIZ-1] = 0;
	wrq.u.data.pointer = ifname_buf;
	wrq.u.data.length = strlen(ifname_buf)+1;
	wrq.u.data.flags = 0;

	ret = ioctl(sockfd, fnum, &wrq);

	if (ret) {
		LOGE("ioctl - failed: %d %s", ret, strerror(errno));
	}
	return ret;
}

int rtl871x_drv_rereg_nd_name(const char *ifname, const char *new_ifname)
{
	int sockfd;
	int ret;

#if 0
	if (ifc_init() < 0)
		return -1;	
	if (ifc_up(ifname)) {
		LOGD("failed to bring up interface %s: %s\n", ifname, strerror(errno));
		return -1;
	}
#endif
	
	sockfd = socket(PF_INET, SOCK_DGRAM, 0);
	if (sockfd< 0) {
		perror("socket[PF_INET,SOCK_DGRAM]");
		ret = -1;
		goto bad;
	}

	ret = rtl871x_drv_rereg_nd_name_fd(
		sockfd
		, ifname
		, get_priv_func_num(sockfd, ifname, "rereg_nd_name")
		, new_ifname
	);
	
	close(sockfd);
bad:
	return ret;
}

int ensure_config_file_exists(const char *conf_file, const char *tmplate)
{
    char buf[2048];
    int srcfd, destfd;
    int nread;

    if (access(conf_file, R_OK|W_OK) == 0) {
        return 0;
    } else if (errno != ENOENT) {
        LOGE("Cannot access \"%s\": %s", conf_file, strerror(errno));
        return -1;
    }

    srcfd = open(tmplate, O_RDONLY);
    if (srcfd < 0) {
        LOGE("Cannot open \"%s\": %s", tmplate, strerror(errno));
        return -1;
    }

    destfd = open(conf_file, O_CREAT|O_WRONLY, 0660);
    if (destfd < 0) {
        close(srcfd);
        LOGE("Cannot create \"%s\": %s", conf_file, strerror(errno));
        return -1;
    }

    while ((nread = read(srcfd, buf, sizeof(buf))) != 0) {
        if (nread < 0) {
            LOGE("Error reading \"%s\": %s", tmplate, strerror(errno));
            close(srcfd);
            close(destfd);
            unlink(conf_file);
            return -1;
        }
        write(destfd, buf, nread);
    }

    close(destfd);
    close(srcfd);

    if (chown(conf_file, AID_SYSTEM, AID_WIFI) < 0) {
        LOGE("Error changing group ownership of %s to %d: %s",
             conf_file, AID_WIFI, strerror(errno));
        unlink(conf_file);
        return -1;
    }
    return 0;
}

int DetectWifiIfNameFromProc()
{
	char linebuf[1024];
	FILE *f = fopen("/proc/net/wireless", "r");
	
	g_ifname[0] = '\0';
	if (f) {
		while(fgets(linebuf, sizeof(linebuf)-1, f)) {
			
			if (strchr(linebuf, ':')) {
				char *dest = g_ifname;
				char *p = linebuf;
				
				while(*p && isspace(*p))
					++p;
				while (*p && *p != ':') {
					*dest++ = *p++;
				}
				*dest = '\0';
				LOGD("DetectWifiIfNameFromProc: %s\n", g_ifname);
				fclose(f);
				return 0;
			}
		}
		fclose(f);
	} 
	return -1;
}

char *getWifiIfname()
{
	DetectWifiIfNameFromProc();
	return g_ifname;
}

int SoftapController::setCommand(char *iface, const char *fname, unsigned buflen)
{
		return 0;
}

void SoftapController::generatePsk(char *ssid, char *passphrase, char *psk_str)
{
}

int SoftapController::clientsSoftap(char **retbuf)
{
		return 0;
}

static int set_hostapd_config_file(const char *conf_file, int argc, char *argv[])
{
	int fd;
	char buf[80];
	int len;

	fd = open(conf_file, O_CREAT|O_WRONLY|O_TRUNC, 0660);
	if (fd < 0) {
		LOGE("Cannot create \"%s\": %s", conf_file, strerror(errno));
		return -1;
	}

	#if 0
	#ifdef USE_DRIVER_PROP_IF_NAME
	getWifiIfname(defIfname);
	property_get(DRIVER_PROP_IF_NAME, ifname, defIfname);
	#else
	getWifiIfname(ifname);
	#endif
	#endif


	if(*getWifiIfname() == '\0') {
		LOGD("set_hostapd_config_file: getWifiIfname fail");
		return -1;
	}
	
	//len = snprintf(buf, sizeof(buf), "interface=%s\n",argv[3]);
	len = snprintf(buf, sizeof(buf), "interface=%s\n", getWifiIfname());
	write(fd, buf, len);
	len = snprintf(buf, sizeof(buf), "ctrl_interface=%s\n", HOSTAPD_CTRL_DIR);
	write(fd, buf, len);

	/* for CU-series flag */
	len = snprintf(buf, sizeof(buf), "driver=rtl871xdrv\n");
	write(fd, buf, len);
	len = snprintf(buf, sizeof(buf), "wme_enabled=1\n");
	write(fd, buf, len);
	len = snprintf(buf, sizeof(buf), "hw_mode=g\n");
	write(fd, buf, len);
	len = snprintf(buf, sizeof(buf), "ieee80211n=1\n");
	write(fd, buf, len);
	len = snprintf(buf, sizeof(buf), "ht_capab=[SHORT-GI-20][SHORT-GI-40]\n");
	write(fd, buf, len);
    
	if (argc > 4) {
		len = snprintf(buf, sizeof(buf), "ssid=%s\n",argv[4]);
	} else {
		len = snprintf(buf, sizeof(buf), "ssid=AndroidAP\n");
	}
	
	/* set open auth */
	write(fd, buf, len);
	len = snprintf(buf, sizeof(buf), "auth_algs=1\n");
	write(fd, buf, len);
	len = snprintf(buf, sizeof(buf), "max_num_sta=%d\n", WIFI_DEFAULT_MAX_STA);
	write(fd, buf, len);
	len = snprintf(buf, sizeof(buf), "beacon_int=%d\n", WIFI_DEFAULT_BI);
	write(fd, buf, len);
	len = snprintf(buf, sizeof(buf), "dtim_period=%d\n", WIFI_DEFAULT_DTIM);
	write(fd, buf, len);
	
	if (argc > 5) {
		if (strncmp(argv[5], "wpa-psk", 8) == 0) {
			len = snprintf(buf, sizeof(buf), "wpa=1\n");
			write(fd, buf, len);
			len = snprintf(buf, sizeof(buf), "wpa_key_mgmt=WPA-PSK\n");
			write(fd, buf, len);
			len = snprintf(buf, sizeof(buf), "wpa_pairwise=CCMP\n");
			write(fd, buf, len);
			if (argc > 6) {
				len = snprintf(buf, sizeof(buf), "wpa_passphrase=%s\n",argv[6]);
				write(fd, buf, len);
			} else {
				len = snprintf(buf, sizeof(buf), "wpa_passphrase=12345678\n");
				write(fd, buf, len);
			}
		}
		else if (strncmp(argv[5], "wpa2-psk", 8) == 0) {
			len = snprintf(buf, sizeof(buf), "wpa=2\n");
			write(fd, buf, len);
			len = snprintf(buf, sizeof(buf), "wpa_key_mgmt=WPA-PSK\n");
			write(fd, buf, len);
			len = snprintf(buf, sizeof(buf), "wpa_pairwise=CCMP\n");
			write(fd, buf, len);
			
			if (argc > 6) {
				len = snprintf(buf, sizeof(buf), "wpa_passphrase=%s\n",argv[6]);
				write(fd, buf, len);
			} else {
				len = snprintf(buf, sizeof(buf), "wpa_passphrase=12345678\n");
				write(fd, buf, len);
			}
		}
	}
	
	if (argc > 7) {
		len = snprintf(buf, sizeof(buf), "channel=%s\n",argv[7]);
		write(fd, buf, len);
	} else {
		len = snprintf(buf, sizeof(buf), "channel=%d\n",WIFI_DEFAULT_CHANNEL);
		write(fd, buf, len);
	}
	
	if (argc > 8) {
		len = snprintf(buf, sizeof(buf), "preamble=%s\n",argv[8]);
		write(fd, buf, len);
	} else {
		len = snprintf(buf, sizeof(buf), "preamble=%d\n",WIFI_DEFAULT_PREAMBLE);
		write(fd, buf, len);
	}
	
	mProfileValid = 1;
	close(fd);

	return 0;
}

#ifdef START_HOSTAPD_INSIDE
static int wifi_start_hostapd()
{
	static int hostapdPid=0;
	
	LOGD("SoftapController::wifi_start_hostapd");


	/* Check whether already running */
	if (hostapdPid != 0) {
		LOGE("%s already started", HOSTAPD_NAME);
		return 0;
	}

	/* Clear out any stale socket files that might be left over. */
	wpa_ctrl_cleanup();
	
	LOGD("Starting %s services", HOSTAPD_NAME);


	if ((pid = fork()) < 0) {
		LOGE("fork failed (%s)", strerror(errno));
		return -1;
	}

	if (!pid) {
		
		char *args[] = {
			(char *)"/system/bin/hostapd"
			(char *)HOSTAPD_CONFIG_FILE	
			,(char *) 0 
		};

		if (execv(args[0], args)) {
			LOGE("execl failed (%s)", strerror(errno));
		}

		LOGE("Should never get here!");
		return 0;
	} else {
		hostapdPid = pid;
		LOGD("%s services running", HOSTAPD_NAME);
	}

    return 0;

}

static int wifi_stop_hostapd()
{
	int count = 50; /* wait at most 5 seconds for completion */
	int wait_ret;

	/* Check whether hostapd already stopped */
	if (hostapdPid == 0) {
		LOGE("%s already stopped", HOSTAPD_NAME);
		return 0;
	}
	

	LOGD("Stopping %s services", HOSTAPD_NAME);
	kill(hostapdPid, SIGTERM);

	while (count-- > 0) {	
		if(hostapdPid == (wait_ret=waitpid(hostapdPid, NULL, WNOHANG)) ) {
			LOGD("%s services stopped", HOSTAPD_NAME);
			hostapdPid = 0;
			return 0
		} else if(wait_ret == 0) {
			usleep(100000);
		} else {
			LOGD("Stopping %s services failed(%s)", HOSTAPD_NAME, strerror(errno));
			return -1;
		}
	}
	return -1;
		
}
#else
static int wifi_start_hostapd()
{
    char daemon_cmd[PROPERTY_VALUE_MAX];
    char supp_status[PROPERTY_VALUE_MAX] = {'\0'};
    int count = 200; /* wait at most 20 seconds for completion */
    char mac_buff[15] = {'\0'};
#ifdef HAVE_LIBC_SYSTEM_PROPERTIES
    const prop_info *pi;
    unsigned serial = 0;
#endif

	LOGD("SoftapController::wifi_start_hostapd");

	/* Check whether already running */
	if (property_get(HOSTAPD_PROP_NAME, supp_status, NULL)
		&& strcmp(supp_status, "running") == 0) {
		return 0;
	}

	/* Before starting the daemon, make sure its config file exists */
	if(ensure_config_file_exists(HOSTAPD_CONFIG_FILE, HOSTAPD_CONFIG_TEMPLATE) < 0) {
		LOGE("configuration file missing");
		return -1;
	}

	/* Clear out any stale socket files that might be left over. */
	wpa_ctrl_cleanup();

#ifdef HAVE_LIBC_SYSTEM_PROPERTIES
	/*
	 * Get a reference to the status property, so we can distinguish
	 * the case where it goes stopped => running => stopped (i.e.,
	 * it start up, but fails right away) from the case in which
	 * it starts in the stopped state and never manages to start
	 * running at all.
	 */
	pi = __system_property_find(HOSTAPD_PROP_NAME);
	if (pi != NULL) {
		serial = pi->serial;
	}
#endif

	#ifdef CONFIG_DAEMON_CMD_WITH_PARA
	snprintf(daemon_cmd, PROPERTY_VALUE_MAX, "%s:%s", HOSTAPD_NAME, HOSTAPD_CONFIG_FILE);
	#else
	snprintf(daemon_cmd, PROPERTY_VALUE_MAX, "%s", HOSTAPD_NAME);
	#endif
	
	property_set("ctl.start", daemon_cmd);
	LOGD("hostapd daemon_cmd = %s\n", daemon_cmd);   
	sched_yield();

	while (count-- > 0) {
#ifdef HAVE_LIBC_SYSTEM_PROPERTIES
		if (pi == NULL) {
			pi = __system_property_find(HOSTAPD_PROP_NAME);
		}
		if (pi != NULL) {
			__system_property_read(pi, NULL, supp_status);
			if (strcmp(supp_status, "running") == 0) {
				LOGD("hostapd running 1");
				return 0;
			} else if (pi->serial != serial && strcmp(supp_status, "stopped") == 0) {
				LOGI("wifi_start_supplicant stopped pi->serial %u serial %u leave", pi->serial, serial);
				if (serial==0) { /* no initialized, skip it */
					serial = pi->serial;
				} else {
					LOGE("HAVE_LIBC_SYSTEM_PROPERTIES: return -1");
					return -1;
				}
			}
		}
#else
		if (property_get(HOSTAPD_PROP_NAME, supp_status, NULL)) {
			if (strcmp(supp_status, "running") == 0) {
				LOGD("hostapd running 2");
				return 0;
			}
		}
#endif
		usleep(100000);
	}
	LOGI("wifi_start_hostapd is NOT running. timeout. leave" );
	return -1;
}

static int wifi_stop_hostapd()
{
	char supp_status[PROPERTY_VALUE_MAX] = {'\0'};
	int count = 50; /* wait at most 5 seconds for completion */

	/* Check whether hostapd already stopped */
	if (property_get(HOSTAPD_PROP_NAME, supp_status, NULL)
		&& strcmp(supp_status, "stopped") == 0) {
		return 0;
	}

	property_set("ctl.stop", HOSTAPD_NAME);
	sched_yield();

	while (count-- > 0) {
		if (property_get(HOSTAPD_PROP_NAME, supp_status, NULL)) {
			if (strcmp(supp_status, "stopped") == 0)
				return 0;
		}
		usleep(100000);
	}
	return -1;
}
#endif

static int wifi_connect_to_hostapd()
{
	char defIfname[256];
	char ctrl_conn_path[256];
	char supp_status[PROPERTY_VALUE_MAX] = {'\0'};
	int retry_times = 20;

	/* Make sure hostapd is running */
	if (!property_get(HOSTAPD_PROP_NAME, supp_status, NULL)
		|| strcmp(supp_status, "running") != 0) {
		LOGE("hostapd not running, cannot connect");
		return -1;
	}

	#if 0
	#ifdef USE_DRIVER_PROP_IF_NAME
	getWifiIfname(defIfname);
	property_get(DRIVER_PROP_IF_NAME, ifname, defIfname);
	#else
	getWifiIfname(ifname);
	#endif
	#endif

	if(*getWifiIfname() == '\0') {
		LOGD("wifi_connect_to_hostapd: getWifiIfname fail");
		return -1;
	}
	
	snprintf(ctrl_conn_path, sizeof(ctrl_conn_path), "%s/%s", HOSTAPD_CTRL_DIR, getWifiIfname());
	LOGD("ctrl_conn_path = %s\n", ctrl_conn_path);


	{ /* check iface file is ready */
		int cnt = 160; /* 8 seconds (160*50)*/
		sched_yield();
		while ( access(ctrl_conn_path, F_OK|W_OK)!=0 && cnt-- > 0) {
			usleep(50000);
		}
		if (access(ctrl_conn_path, F_OK|W_OK)==0) {
			LOGD("ctrl_conn_path %s is ready to read/write cnt=%d\n", ctrl_conn_path, cnt);
		} else {
			LOGD("ctrl_conn_path %s is not ready, cnt=%d\n", ctrl_conn_path, cnt);
		}
	}

	while (retry_times--){
		ctrl_conn = wpa_ctrl_open(ctrl_conn_path);
		if (NULL == ctrl_conn) {
			usleep(1000 * 500);
			LOGD("Retry to wpa_ctrl_open \n");
		} else {
			break;
		}
	}
	
	if (NULL == ctrl_conn) {
		LOGE("Unable to open connection to supplicant on \"%s\": %s",
		ctrl_conn_path, strerror(errno));
		return -1;
	}

	if (wpa_ctrl_attach(ctrl_conn) != 0) {
		wpa_ctrl_close(ctrl_conn);
		ctrl_conn = NULL;
		return -1;
	}
	return 0;
}

static void wifi_close_hostapd_connection()
{
	if (ctrl_conn != NULL) {
		wpa_ctrl_close(ctrl_conn);
		ctrl_conn = NULL;
	}
}

static int wifi_load_profile(bool started)
{
	if ((started) && (mProfileValid)) {
		if (ctrl_conn == NULL) {
			LOGE("wifi_load_profile(): ctrl_conn == NULL");
			return -1;
		} else {
			LOGE("wpa_ctrl_reload(ctrl_conn)...");
			return wpa_ctrl_reload(ctrl_conn);
		}
	}
	return 0;
}

#endif  /* CONFIG_WLAN_RTK_WIFI_HOSTAPD [WIFI] Wi-Fi Support -- */


SoftapController::SoftapController() {
	mPid = 0;
	mSock = socket(AF_INET, SOCK_DGRAM, 0);
	if (mSock < 0)
		LOGE("Failed to open socket");
	
	memset(mIface, 0, sizeof(mIface));

#ifdef CONFIG_WLAN_RTK_WIFI_HOSTAPD     /* [WIFI] Wi-Fi Support ++ */
	mProfileValid = 0;
	ctrl_conn = NULL;
#endif  /* CONFIG_WLAN_RTK_WIFI_HOSTAPD [WIFI] Wi-Fi Support -- */
}

SoftapController::~SoftapController() {
	if (mSock >= 0)
		close(mSock);
}

int SoftapController::startDriver(char *iface) {
	int ret = 0;
	LOGD("SoftapController::startDriver");
	if (mSock < 0) {
		LOGE("Softap driver start - failed to open socket");
		return -1;
	}
	if (!iface || (iface[0] == '\0')) {
		LOGD("Softap driver start - wrong interface");
		iface = mIface;
	}

	ret = rtl871x_drv_rereg_nd_name(getWifiIfname(), WIFI_DRIVER_IFNAME_AP);
		
	/* Indicate interface down */
	//apNotifyInterfaceChanged(iface, false);

	LOGD("Softap driver start: %d", ret);
	
	return ret;
}

int SoftapController::stopDriver(char *iface) {
	int ret = 0;
	LOGE("SoftapController::stopDriver");

	if (mSock < 0) {
		LOGE("Softap driver stop - failed to open socket");
		return -1;
	}
	if (!iface || (iface[0] == '\0')) {
		LOGD("Softap driver stop - wrong interface");
		iface = mIface;
	}

	//Is this needed??
	//ret = wifi_unload_driver();

	LOGD("Softap driver stop: %d", ret);
	return ret;
}

int SoftapController::startSoftap() {
	struct iwreq wrq;
	pid_t pid = 1;
	int fnum, ret = 0;

	LOGD("SoftapController::startSoftap");

	if (mPid) {
		LOGE("Softap already started");
		return 0;
	}

	if (mSock < 0) {
		LOGE("Softap startap - failed to open socket");
		return -1;
	}

	if ((ret = wifi_start_hostapd()) < 0) {
		LOGE("Softap startap - starting hostapd fails");
		return -1;
	}
       
	if ((ret = wifi_connect_to_hostapd()) < 0) {
		LOGE("Softap startap - connect to hostapd fails");
		return -1;
	}

        /* Indicate interface up */
        //apNotifyInterfaceChanged(iface, true);

	if ((ret = wifi_load_profile(true)) < 0) {
		LOGE("Softap startap - load new configuration fails");
		return -1;
	}

	if (ret) {
		LOGE("Softap startap - failed: %d", ret);
	}
	else {
		mPid = pid;
		LOGD("Softap startap - Ok");
		usleep(AP_BSS_START_DELAY);
	}
	return ret;

}

int SoftapController::stopSoftap() {
	struct iwreq wrq;
	int fnum, ret;

	LOGD("softapcontroller->stopSoftap");

	if (mPid == 0) {
		LOGE("Softap already stopped");
		return 0;
	}
	if (mSock < 0) {
		LOGE("Softap stopap - failed to open socket");
		return -1;
	}
	
	wifi_close_hostapd_connection();
	ret = wifi_stop_hostapd();

	mPid = 0;
	LOGD("Softap service stopped: %d", ret);
	usleep(AP_BSS_STOP_DELAY);
	
	return ret;
}

bool SoftapController::isSoftapStarted() {
    return (mPid != 0 ? true : false);
}

int SoftapController::addParam(int pos, const char *cmd, const char *arg)
{
    if (pos < 0)
        return pos;
    if ((unsigned)(pos + strlen(cmd) + strlen(arg) + 1) >= sizeof(mBuf)) {
        LOGE("Command line is too big");
        return -1;
    }
    pos += sprintf(&mBuf[pos], "%s=%s,", cmd, arg);
    return pos;
}

/*
 * Arguments:
 *      argv[2] - wlan interface
 *      argv[3] - softap interface
 *      argv[4] - SSID
 *	argv[5] - Security
 *	argv[6] - Key
 *	argv[7] - Channel
 *	argv[8] - Preamble
 *	argv[9] - Max SCB
 */
int SoftapController::setSoftap(int argc, char *argv[]) {
	unsigned char psk[SHA256_DIGEST_LENGTH];
	char psk_str[2*SHA256_DIGEST_LENGTH+1];
	struct iwreq wrq;
	int fnum, ret, i = 0;
	char *ssid;

	if (mSock < 0) {
		LOGE("Softap set - failed to open socket");
		return -1;
	}
	if (argc < 4) {
		LOGE("Softap set - missing arguments");
		return -1;
	}
	
	if ((ret = set_hostapd_config_file(HOSTAPD_CONFIG_FILE, argc, argv)) < 0) {
		LOGE("Softap set - set_hostapd_config_file fails");
		return -1;
	}

	if ((ret = wifi_load_profile(isSoftapStarted())) < 0) {
		LOGE("Softap set - load new configuration fails");
		return -1;
	}    

	if (ret) {
		LOGE("Softap set - failed: %d", ret);
	} else {
		LOGD("Softap set - Ok");
		usleep(AP_SET_CFG_DELAY);
	}
	return ret;
}

/*
 * Arguments:
 *	argv[2] - interface name
 *	argv[3] - AP or STA
 */
int SoftapController::fwReloadSoftap(int argc, char *argv[])
{
	struct iwreq wrq;
	int fnum, ret, i = 0;
	char *iface;

	if (mSock < 0) {
		LOGE("Softap fwrealod - failed to open socket");
		return -1;
	}
	if (argc < 4) {
		LOGE("Softap fwreload - missing arguments");
		return -1;
	}
	
	ret = 0;

	if (ret) {
		LOGE("Softap fwReload - failed: %d", ret);
	}
	else {
		LOGD("Softap fwReload - Ok");
	}
	return ret;
}
