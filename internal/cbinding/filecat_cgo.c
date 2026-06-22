/* Umbrella translation unit: cgo auto-compiles every .c file in the package
 * directory, but the upstream Filecat sources are split per OS and we want
 * only one of them in the build. Keeping the platform sources in csrc/
 * (a subdirectory cgo ignores) and pulling the right one in here means the
 * upstream files stay byte-for-byte identical to the C project. */

#if defined(_WIN32)
#  include "csrc/platform/windows/filecat_win.c"
#elif defined(__APPLE__)
#  include "csrc/platform/macos/filecat_mac.c"
#elif defined(__linux__)
#  include "csrc/platform/linux/filecat_linux.c"
#else
#  error "filecat-go: unsupported platform"
#endif
