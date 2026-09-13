/* ZR10 SD package validation. Included only by the ZR10 web build. */
#ifndef MT11_WEB_TEST
static bool zr10_sd_mounted(void)
{
    FILE *file = fopen("/proc/mounts", "r");
    if (file == NULL) return false;
    char device[256], path[256], type[32], options[512];
    bool found = false;
    while (fscanf(file, "%255s %255s %31s %511s %*d %*d\n",
                  device, path, type, options) == 4) {
        if (strcmp(device, "/dev/mmcblk0p1") == 0 &&
            strcmp(path, MEDIA_ROOT) == 0 &&
            strcmp(type, "vfat") == 0) {
            found = true;
            break;
        }
    }
    fclose(file);
    return found;
}

#endif

static bool zr10_valid_firmware(int fd)
{
    unsigned char buffer[16384];
    unsigned char trailer[36];
    const size_t body_size = ZR10_SCRIPT_SIZE + ZR10_CUSTOMER_SIZE;
    struct stat st;
    uint32_t crc = 0xffffffffU;
    if (fstat(fd, &st) != 0 || st.st_size != ZR10_FIRMWARE_SIZE) return false;
    for (size_t offset = 0; offset < body_size;) {
        size_t size = body_size - offset;
        if (size > sizeof(buffer)) size = sizeof(buffer);
        ssize_t got = pread(fd, buffer, size, (off_t)offset);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        for (size_t i = 0; i < (size_t)got; i++) {
            size_t position = offset + i;
            if (position < ZR10_SCRIPT_SIZE) {
                unsigned char expected = position < sizeof(zr10_upgrade_script)-1 ?
                    zr10_upgrade_script[position] : 0xff;
                if (buffer[i] != expected) return false;
            }
            if (position == ZR10_SCRIPT_SIZE && buffer[i] != 0x85) return false;
            if (position == ZR10_SCRIPT_SIZE+1 && buffer[i] != 0x19) return false;
            crc ^= buffer[i];
            for (unsigned bit = 0; bit < 8; bit++)
                crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
        offset += (size_t)got;
    }
    if (pread(fd, trailer, sizeof(trailer), (off_t)body_size) != sizeof(trailer)) return false;
    uint32_t expected = (uint32_t)trailer[8] | (uint32_t)trailer[9] << 8 |
                        (uint32_t)trailer[10] << 16 | (uint32_t)trailer[11] << 24;
    return expected == (crc ^ 0xffffffffU) && memcmp(trailer, "ZR10AP01", 8) == 0 &&
           memcmp(trailer+12, "12345678", 8) == 0 &&
           memcmp(trailer+20, zr10_upgrade_script, 16) == 0;
}
