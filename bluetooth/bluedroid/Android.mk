#
# libbluedroid
#

LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)
    
ifdef BT_DRIVER_MODULE_PATH
LOCAL_CFLAGS += -DBT_DRIVER_MODULE_PATH=\"$(BT_DRIVER_MODULE_PATH)\"
endif
ifdef BT_DRIVER_MODULE_ARG
LOCAL_CFLAGS += -DBT_DRIVER_MODULE_ARG=\"$(BT_DRIVER_MODULE_ARG)\"
endif
ifdef BT_DRIVER_MODULE_NAME
LOCAL_CFLAGS += -DBT_DRIVER_MODULE_NAME=\"$(BT_DRIVER_MODULE_NAME)\"
endif

LOCAL_SRC_FILES := \
	bluetooth.c

LOCAL_C_INCLUDES := \
	$(LOCAL_PATH)/include \
	system/bluetooth/bluez-clean-headers

LOCAL_SHARED_LIBRARIES := \
	libcutils

LOCAL_MODULE := libbluedroid

include $(BUILD_SHARED_LIBRARY)
