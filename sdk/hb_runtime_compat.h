#ifndef HB_RUNTIME_COMPAT_H
#define HB_RUNTIME_COMPAT_H

/* One app/resident compatibility contract, shared with the packaging tools.
 * Bump ABI only for incompatible changes, not for ordinary fixes/additions.
 * This encompasses the app API and shared-state layouts, whose own version
 * checks remain in place. Supporting a previous ABI requires explicit code;
 * never assume a numeric range of ABIs is compatible. Unsupported ABIs must be
 * rejected before app code executes. A newer resident accepts older apps using
 * a supported ABI; an older resident skips apps needing a newer resident version.
 *
 * Manifest format versions describe parsing separately from runtime ABI.
 * HBMinimumResidentVersion (Info.plist, default 0) gates compatible features or
 * fixes without changing ABI. Its value is a release's build.json resident_version.
 * tools/runtime_compat.py reads these constants for the Python packaging tools.
 */
#define HB_RUNTIME_ABI 2u
#define HB_RUNTIME_FIRMWARE 0x00010102u

#endif
