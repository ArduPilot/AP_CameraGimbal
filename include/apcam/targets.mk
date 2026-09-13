# Include from camera_app/Makefile or web/Makefile. These are compiler target
# identifiers, not a second table of camera properties.
APCAM_TARGET_mt11 := APCAM_TARGET_MT11
APCAM_TARGET_a8 := APCAM_TARGET_A8
APCAM_TARGET_zr10 := APCAM_TARGET_ZR10
APCAM_TARGET_z1mini := APCAM_TARGET_Z1_MINI
APCAM_HEADERS := $(wildcard ../include/apcam/*.h)
APCAM_CPPFLAGS = -I../include -DAPCAM_TARGET=$(APCAM_TARGET_$(CAMERA_BACKEND))
