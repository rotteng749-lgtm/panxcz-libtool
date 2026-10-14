LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := panxcz_libtool

# Debug: tanpa NDEBUG (log verbose), tanpa strip, tanpa optimize berat.
_CF := -O1 -g -march=armv8-a+simd \
       -fomit-frame-pointer -ffunction-sections -fdata-sections \
       -DUSE_OPENGL -w \
       -fstack-protector-strong \
       -fPIE \
       -fno-unwind-tables \
       -fno-asynchronous-unwind-tables \
       -fno-ident \
       -fno-jump-tables \
       -fstrict-aliasing \
       -fno-delete-null-pointer-checks \
       -fno-strict-overflow \
       -fno-builtin-memcpy \
       -fno-builtin-memset \
       -fno-builtin-strlen \
       -fno-builtin-strcmp

LOCAL_CPPFLAGS := $(_CF) -std=c++17 -fexceptions -fno-rtti -fpermissive \
                  -Wno-error=format-security -Wno-error=c++11-narrowing
LOCAL_CFLAGS   := $(_CF) -std=c99

LOCAL_LDFLAGS := -Wl,--gc-sections -Wl,--build-id=none -pie -fPIE

LOCAL_C_INCLUDES += $(LOCAL_PATH)/include \
                    $(LOCAL_PATH)/include/ImGui \
                    $(LOCAL_PATH)/include/ImGui/backends \
                    $(LOCAL_PATH)/src

LOCAL_SRC_FILES := \
    src/main.cpp \
    src/core/mem.cpp \
    src/core/log.cpp \
    src/core/il2cpp.cpp \
    src/core/target.cpp \
    src/core/explore.cpp \
    src/core/scanner.cpp \
    src/core/safe.cpp \
    src/core/keys.cpp \
    src/ui/ui.cpp \
    src/Android_draw/draw.cpp \
    src/Android_touch/TouchHelperA.cpp \
    src/ImGui/imgui.cpp \
    src/ImGui/imgui_draw.cpp \
    src/ImGui/imgui_tables.cpp \
    src/ImGui/imgui_widgets.cpp \
    src/ImGui/backends/imgui_impl_android.cpp \
    src/ImGui/backends/imgui_impl_opengl3.cpp

LOCAL_LDLIBS := -llog -landroid -lEGL -lGLESv3 -lz -ldl

include $(BUILD_EXECUTABLE)
