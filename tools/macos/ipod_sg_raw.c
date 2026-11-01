/* ipod_sg_raw — sg_raw-compatible raw SCSI for iPods on macOS.
 * Adapted from Raphmatt/nano7-a1717-dock, tools/macos/ipod_sg_raw.c.
 * MIT license and original copyright are retained in LICENSE alongside this file.
 *
 * Uses Apple's SCSITaskDeviceInterface (QA1179), which macOS exposes for iPods
 * via com_apple_driver_iPodSBCNub (SCSITaskDeviceCategory = iPodUserClientDevice).
 *
 * Usage (subset of sg_raw):
 *   ipod_sg_raw [-s N -i INFILE] [-o OUTFILE -r N] [-t TIMEOUT_SECONDS] [-v] DEV CDB_HEX...
 * DEV selects a BSD disk; the matching iPod SCSI nub is selected by USB ancestry.
 * DEV=auto permits START STOP UNIT (load/eject) and read-only C6/98 peek on one iPod. This
 * reactivates ejected media when no BSD disk exists to select by ancestry.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/scsi/SCSITaskLib.h>
#include <IOKit/scsi/SCSICommandOperationCodes.h>
#include <mach/mach_vm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>

static int verbose = 0;

static unsigned long number(const char *s, int base, unsigned long max) {
    char *end; errno = 0;
    unsigned long n = strtoul(s, &end, base);
    if (!*s || *s == '-' || *end || errno || n > max) {
        fprintf(stderr, "invalid numeric argument: %s\n", s); exit(1);
    }
    return n;
}

/* Match the SCSI nub and BSD disk through their common USB device, not order. */
static uint64_t usb_parent(io_registry_entry_t entry) {
    io_registry_entry_t current = entry, parent;
    IOObjectRetain(current);
    while (current) {
        if (IOObjectConformsTo(current, "IOUSBHostDevice") ||
            IOObjectConformsTo(current, "IOUSBDevice")) {
            uint64_t id = 0; IORegistryEntryGetRegistryEntryID(current, &id);
            IOObjectRelease(current); return id;
        }
        IOReturn rc = IORegistryEntryGetParentEntry(current, kIOServicePlane, &parent);
        IOObjectRelease(current);
        if (rc) break;
        current = parent;
    }
    return 0;
}

static void die(const char *m, IOReturn r) {
    fprintf(stderr, "ipod_sg_raw: %s (0x%08x)\n", m, r);
    exit(2);
}

int main(int argc, char **argv) {
    const char *infile = NULL, *outfile = NULL;
    long slen = 0, rlen = 0; UInt32 timeout = 10000;
    int opt;
    while ((opt = getopt(argc, argv, "s:i:o:r:t:v")) != -1) {
        switch (opt) {
        case 's': slen = number(optarg, 0, 16 * 1024 * 1024); break;
        case 'i': infile = optarg; break;
        case 'o': outfile = optarg; break;
        case 'r': rlen = number(optarg, 0, 16 * 1024 * 1024); break;
        case 't': timeout = (UInt32)number(optarg, 0, UINT_MAX / 1000) * 1000; break;
        case 'v': verbose++; break;
        default: fprintf(stderr, "usage: %s [-s N -i in] [-o out -r N] [-t seconds] DEV CDB...\n", argv[0]); return 1;
        }
    }
    if (argc - optind < 2) { fprintf(stderr, "need DEV and CDB bytes\n"); return 1; }
    const char *disk = argv[optind++];
    if ((slen && rlen) || (slen && !infile) || (rlen && !outfile) ||
        (!slen && infile) || (!rlen && outfile) || argc - optind > 16 || !timeout) {
        fprintf(stderr, "invalid transfer options or CDB length\n"); return 1;
    }
    UInt8 cdb[16] = {0}; UInt8 cdblen = 0;
    for (; optind < argc; optind++) cdb[cdblen++] = (UInt8)number(argv[optind], 16, 255);
    int auto_load = strcmp(disk, "auto") == 0;
    const UInt8 load_cdb[6] = {0x1b, 0, 0, 0, 3, 0};
    const UInt8 eject_cdb[6] = {0x1b, 0, 0, 0, 2, 0};
    int is_media = cdblen == 6 && (!memcmp(cdb, load_cdb, 6) ||
                                  !memcmp(cdb, eject_cdb, 6)) && !slen && !rlen;
    int is_peek = cdblen == 7 && cdb[0] == 0xc6 && cdb[1] == 0x98 &&
                  cdb[2] == 2 && !slen && rlen == 512;
    if (auto_load && !is_media && !is_peek) {
        fprintf(stderr, "auto selection supports only media load/eject or read-only C6/98 peek\n");
        return 1;
    }

    if (cdb[0] == 0xc6 && (cdblen < 2 || cdb[1] != 0x98)) {
        fprintf(stderr, "only the iTunes/Finder-compatible C6/98 debug channel is supported\n");
        return 1;
    }

    /* data buffer */
    long len = slen ? slen : rlen;
    UInt8 dir = slen ? kSCSIDataTransfer_FromInitiatorToTarget
              : rlen ? kSCSIDataTransfer_FromTargetToInitiator
              : kSCSIDataTransfer_NoDataTransfer;
    UInt8 *buf = NULL;
    if (len) {
        buf = valloc(len); if (!buf) { perror("valloc"); return 1; } memset(buf, 0, len);
        if (slen) {
            int fd = open(infile, O_RDONLY); if (fd < 0) { perror(infile); return 1; }
            size_t done = 0;
            while (done < (size_t)len) {
                ssize_t n = read(fd, buf + done, len - done);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) { fprintf(stderr, "short or failed input read\n"); close(fd); free(buf); return 1; }
                done += (size_t)n;
            }
            close(fd);
        }
    }

    /* find the iPod nub that carries the SCSITaskUserClient */
    io_iterator_t it; IOReturn kr;
    kr = IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("com_apple_driver_iPodSBCNub"), &it);
    if (kr) die("IOServiceGetMatchingServices", kr);
    const char *bsd = strrchr(disk, '/'); bsd = bsd ? bsd + 1 : disk;
    io_service_t media = auto_load ? 0 : IOServiceGetMatchingService(kIOMainPortDefault,
        IOBSDNameMatching(kIOMainPortDefault, 0, bsd));
    uint64_t usb = media ? usb_parent(media) : 0;
    if (media) IOObjectRelease(media);
    io_service_t candidate, svc = 0;
    while ((candidate = IOIteratorNext(it))) {
        if (auto_load || (usb && usb_parent(candidate) == usb)) {
            if (svc) { IOObjectRelease(candidate); IOObjectRelease(svc); svc = 0; break; }
            svc = candidate;
        } else IOObjectRelease(candidate);
    }
    IOObjectRelease(it);
    if (!svc) { fprintf(stderr, "ipod_sg_raw: no iPod (com_apple_driver_iPodSBCNub) found\n"); return 2; }

    IOCFPlugInInterface **plug = NULL; SInt32 score;
    kr = IOCreatePlugInInterfaceForService(svc, kIOSCSITaskDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    IOObjectRelease(svc);
    if (kr || !plug) die("IOCreatePlugInInterfaceForService", kr);

    SCSITaskDeviceInterface **dev = NULL;
    HRESULT hr = (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOSCSITaskDeviceInterfaceID), (LPVOID *)&dev);
    if (hr || !dev) die("QueryInterface SCSITaskDeviceInterface", (IOReturn)hr);

    if (!(*dev)->IsExclusiveAccessAvailable(dev) && verbose)
        fprintf(stderr, "ipod_sg_raw: exclusive access not available (volume mounted?)\n");
    kr = (*dev)->ObtainExclusiveAccess(dev);
    if (kr) die("ObtainExclusiveAccess (unmount the iPod volume first: diskutil unmountDisk <disk>)", kr);

    SCSITaskInterface **task = (*dev)->CreateSCSITask(dev);
    if (!task) die("CreateSCSITask", 0);

    /* SCSITaskLib only accepts 6/10/12/16-byte CDBs; sg_raw sends any length (the
     * iPod's vendor c6 98 .. commands are 7 bytes). Pad with zeros to the next valid
     * size -- vendor-group opcodes carry no fixed length, the device ignores the tail. */
    { UInt8 valid[] = {6, 10, 12, 16}; for (int i = 0; i < 4; i++) if (cdblen <= valid[i]) { cdblen = valid[i]; break; } }
    kr = (*task)->SetCommandDescriptorBlock(task, cdb, cdblen);
    if (kr) die("SetCommandDescriptorBlock", kr);
    SCSITaskSGElement sg = { (mach_vm_address_t)buf, (mach_vm_size_t)len };
    kr = (*task)->SetScatterGatherEntries(task, len ? &sg : NULL, len ? 1 : 0, (UInt64)len, dir);
    if (kr) die("SetScatterGatherEntries", kr);
    (*task)->SetTimeoutDuration(task, timeout);

    SCSI_Sense_Data sense; memset(&sense, 0, sizeof sense);
    SCSITaskStatus status = 0xff; UInt64 realized = 0;
    kr = (*task)->ExecuteTaskSync(task, &sense, &status, &realized);

    if (verbose || kr || status != kSCSITaskStatus_GOOD || realized != (UInt64)len)
        fprintf(stderr, "ipod_sg_raw: cdb=%02x%s status=0x%02x kr=0x%08x realized=%llu sense=%02x/%02x/%02x\n",
                cdb[0], cdblen > 1 ? "…" : "", status, kr, realized,
                sense.SENSE_KEY & 0x0f, sense.ADDITIONAL_SENSE_CODE, sense.ADDITIONAL_SENSE_CODE_QUALIFIER);

    int rc = (kr == kIOReturnSuccess && status == kSCSITaskStatus_GOOD && realized == (UInt64)len) ? 0 : 3;
    if (rc == 0 && rlen && outfile) {
        int fd = strcmp(outfile, "-") ? open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644) : 1;
        if (fd < 0) { perror(outfile); rc = 3; }
        else {
            size_t done = 0;
            while (done < (size_t)rlen) {
                ssize_t n = write(fd, buf + done, rlen - done);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) { perror("write"); rc = 3; break; }
                done += (size_t)n;
            }
            if (fd != 1 && close(fd) != 0) rc = 3;
        }
    }
    (*task)->Release(task);
    (*dev)->ReleaseExclusiveAccess(dev);
    (*dev)->Release(dev);
    IODestroyPlugInInterface(plug);
    free(buf);
    return rc;
}
