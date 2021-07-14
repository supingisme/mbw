LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE:= mbw
LOCAL_MODULE_TAGS:= optional
LOCAL_SRC_FILES:= \
		mbw.c

include $(BUILD_EXECUTABLE)

