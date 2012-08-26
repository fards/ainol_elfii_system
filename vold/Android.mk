LOCAL_PATH:= $(call my-dir)

common_src_files := \
	VolumeManager.cpp \
	CommandListener.cpp \
	VoldCommand.cpp \
	NetlinkManager.cpp \
	NetlinkHandler.cpp \
	Volume.cpp \
	DirectVolume.cpp \
	logwrapper.c \
	Process.cpp \
	Fat.cpp \
	Ntfs.cpp \
	Exfat.cpp \
	Hfsplus.cpp \
	Iso9660.cpp \
	Loop.cpp \
	Devmapper.cpp \
	ResponseCode.cpp \
	Xwarp.cpp \
	cryptfs.c \
	Ums.cpp \
	UsbMdmMgr.cpp \
	UsbModem.cpp

common_c_includes := \
	$(KERNEL_HEADERS) \
	system/extras/ext4_utils \
	external/openssl/include

common_shared_libraries := \
	libsysutils \
	libcutils \
	libdiskconfig \
	libhardware_legacy \
	libcrypto

include $(CLEAR_VARS)

LOCAL_MODULE := libvold

LOCAL_SRC_FILES := $(common_src_files)

LOCAL_C_INCLUDES := $(common_c_includes)

LOCAL_SHARED_LIBRARIES := $(common_shared_libraries)

LOCAL_MODULE_TAGS := eng tests

include $(BUILD_STATIC_LIBRARY)

include $(CLEAR_VARS)

LOCAL_MODULE:= vold

LOCAL_SRC_FILES := \
	main.cpp \
	$(common_src_files)

LOCAL_C_INCLUDES := $(common_c_includes)

LOCAL_CFLAGS := 
#LOCAL_CFLAGS += -DHAS_UMS_SWITCH
#LOCAL_CFLAGS += -DPARTITION_DEBUG -DNETLINK_DEBUG
LOCAL_CFLAGS += -DHAS_NTFS_3G
LOCAL_CFLAGS += -DHAS_EXFAT
LOCAL_CFLAGS += -DHAS_EXFAT_FUSE
LOCAL_CFLAGS += -DHAS_ISO9660
LOCAL_CFLAGS += -DHAS_VIRTUAL_CDROM

ifneq ($(strip $(TARGET_RECOVERY_MEDIA_LABEL)),)
LOCAL_CFLAGS += -DRECOVERY_MEDIA_LABEL=$(TARGET_RECOVERY_MEDIA_LABEL)
endif

LOCAL_SHARED_LIBRARIES := $(common_shared_libraries)

include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= vdc.c

LOCAL_MODULE:= vdc

LOCAL_C_INCLUDES := $(KERNEL_HEADERS)

LOCAL_CFLAGS := 

LOCAL_SHARED_LIBRARIES := libcutils

include $(BUILD_EXECUTABLE)
