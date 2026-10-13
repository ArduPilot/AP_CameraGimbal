# Explicit dynamically linked, bounded RAM full-app integration only.
# No default native build or package activation; no SIYI UART driver linked.
ZT30_APP_DIR := build/zt30-ram
ZT30_APP_SRC := $(filter-out $(CONTROL_DIR)/control.cpp,$(COMMON_SOURCES)) \
 src/backends/zt30/ram_control.cpp src/backends/zt30/ram_factory.cpp \
 src/backends/zt30/media_backend.cpp src/backends/zt30/rgb_owner.cpp src/backends/zt30/encoder_owner.cpp \
 src/streaming/live_video_server.cpp src/recording/mp4_minimp4.cpp src/streaming/rtsp_xop.cpp
ZT30_APP_CPPFLAGS := $(CPPFLAGS) -DCAMERA_APP_ZT30_RAM -D_FILE_OFFSET_BITS=64 \
 -I$(RTSP_ROOT) -I$(RTSP_ROOT)/net -I$(RTSP_ROOT)/xop -I$(RTSP_ROOT)/3rdpart
ZT30_APP_XOP := $(filter-out $(RTSP_ROOT)/net/TaskScheduler.cpp $(RTSP_ROOT)/xop/RtspConnection.cpp $(RTSP_ROOT)/xop/RtpConnection.cpp,$(wildcard $(RTSP_ROOT)/net/*.cpp $(RTSP_ROOT)/xop/*.cpp))
ZT30_APP_OBJECTS := $(patsubst %.cpp,$(ZT30_APP_DIR)/c/%.o,$(ZT30_APP_SRC)) \
 $(patsubst $(RTSP_ROOT)/%.cpp,$(ZT30_APP_DIR)/xop/%.o,$(ZT30_APP_XOP)) \
 $(ZT30_APP_DIR)/patched-rtsp.o $(ZT30_APP_DIR)/patched-rtp.o $(ZT30_APP_DIR)/patched-scheduler.o $(ZT30_APP_DIR)/tracking_dlib.o
OBJECTS := $(ZT30_APP_OBJECTS)
$(TARGET): $(OBJECTS) Makefile ../tools/zt30_app.mk
	mkdir -p $(@D)
	$(CXX) $(APP_CXXFLAGS) -o $@ $(OBJECTS) -lpthread -ldl -lm
	python3 ../tools/check_zt30_elf.py $@ --readelf $(CROSS_COMPILE)readelf
$(ZT30_APP_DIR)/c/%.o: %.cpp Makefile ../tools/zt30_app.mk | generated
	mkdir -p $(@D)
	$(CXX) $(DEPFLAGS) $(ZT30_APP_CPPFLAGS) $(APP_CXXFLAGS) -Wno-unused-function -Wno-unused-parameter -Wno-error=sign-compare -Wno-error=unused-variable -Wno-error=implicit-fallthrough -c $< -o $@
$(ZT30_APP_DIR)/xop/%.o: $(RTSP_ROOT)/%.cpp | $(XOP_PREPARE_STAMP)
	mkdir -p $(@D)
	$(CXX) $(DEPFLAGS) $(ZT30_APP_CPPFLAGS) -include cstdint -O2 -std=gnu++17 -c $< -o $@
$(ZT30_APP_DIR)/patched-rtsp.o: build/xop_rtsp_connection.cpp
	mkdir -p $(@D)
	$(CXX) $(DEPFLAGS) $(ZT30_APP_CPPFLAGS) -O2 -std=gnu++17 -c $< -o $@
$(ZT30_APP_DIR)/patched-rtp.o: build/xop_rtp_connection.cpp
	mkdir -p $(@D)
	$(CXX) $(DEPFLAGS) $(ZT30_APP_CPPFLAGS) -O2 -std=gnu++17 -c $< -o $@
$(ZT30_APP_DIR)/TaskScheduler.cpp: $(RTSP_ROOT)/net/TaskScheduler.cpp ../tools/zt30_xop_signal.patch ../tools/patch_source.py
	python3 ../tools/patch_source.py $< ../tools/zt30_xop_signal.patch $@
$(ZT30_APP_DIR)/patched-scheduler.o: $(ZT30_APP_DIR)/TaskScheduler.cpp
	$(CXX) $(DEPFLAGS) $(ZT30_APP_CPPFLAGS) -O2 -std=gnu++17 -c $< -o $@
$(ZT30_APP_DIR)/tracking_dlib.o: src/media/tracking_dlib.cpp include/camera_app/image_tracker.h Makefile ../tools/zt30_app.mk | generated
	mkdir -p $(@D)
	$(CXX) $(DEPFLAGS) $(DLIB_FLAGS) -c $< -o $@
-include $(ZT30_APP_OBJECTS:.o=.d)
