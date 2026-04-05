#include <shlobj.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <winioctl.h>

#define SECTOR_SIZE 512
#define CLUSTER_SIZE_32KB (32 * 1024)
#define RESERVED_SECTORS 32
#define PARTITION_START_LBA 2048

// MinGW環境等で不足する定義の補完
#ifndef IOCTL_DISK_SET_DISK_ATTRIBUTES
#define IOCTL_DISK_SET_DISK_ATTRIBUTES 0x0007c0f0
#endif

typedef struct {
    DWORD Version;
    BOOLEAN Persist;
    BYTE Reserved[3];
    DWORD64 Attributes;
    DWORD64 AttributesMask;
    DWORD Reserved2[4];
} SET_DISK_ATTRIBUTES;

#pragma pack(push, 1)
typedef struct {
    BYTE jmp[3];
    char oem_name[8];
    WORD bytes_per_sector;
    BYTE sectors_per_cluster;
    WORD reserved_sectors;
    BYTE fats;
    WORD root_entries;
    WORD sectors_16;
    BYTE media_type;
    WORD sectors_per_fat_16;
    WORD sectors_per_track;
    WORD heads;
    DWORD hidden_sectors;
    DWORD sectors_32;
    DWORD sectors_per_fat_32;
    WORD ext_flags;
    WORD fs_version;
    DWORD root_cluster;
    WORD fs_info_sector;
    WORD backup_boot_sector;
    BYTE reserved[12];
    BYTE drive_number;
    BYTE reserved1;
    BYTE boot_signature;
    DWORD volume_id;
    char volume_label[11];
    char fs_type[8];
    BYTE boot_code[420];
    WORD signature;
} FAT32_VBR;

typedef struct {
    DWORD lead_sig;
    BYTE reserved1[480];
    DWORD struc_sig;
    DWORD free_count;
    DWORD next_free;
    BYTE reserved2[12];
    DWORD trail_sig;
} FAT32_FSINFO;
#pragma pack(pop)

void ShowHelp() {
    printf("\nfat32fmt v1.1.0 - Professional Physical Formatter\n");
    printf("\n\n");
    printf("Usage:\n");
    printf("  fat32fmt.exe [options]\n\n");
    printf("Options:\n");
    printf("  -l, --list             List all physical drives\n");
    printf("  -f, --format <index>   Format specified drive to FAT32\n");
    printf("  -h, --help             Show this help message\n\n");
    printf("Safety Feature:\n");
    printf("  System Drive (Disk 0) is strictly protected.\n\n");
}

void ListDrives() {
    printf("Index\tSize (GB)\tStatus\n");
    printf("----------------------------------------------------\n");
    for(int i = 0; i < 16; i++) {
        char path[64];
        sprintf(path, "\\\\.\\PhysicalDrive%d", i);
        HANDLE h = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
        if(h != INVALID_HANDLE_VALUE) {
            DISK_GEOMETRY_EX geo;
            DWORD br;
            if(DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0,
                               &geo, sizeof(geo), &br, NULL)) {
                double gb =
                    (double)geo.DiskSize.QuadPart / (1024.0 * 1024.0 * 1024.0);
                printf(
                    "Disk %d\t%8.2f GB\t%s\n", i, gb,
                    (i == 0 ? "[SYSTEM - PROTECTED]" : "Removable/Secondary"));
            }
            CloseHandle(h);
        }
    }
}

bool SetDiskOffline(HANDLE h, bool offline) {
    SET_DISK_ATTRIBUTES attr = {0};
    attr.Version = sizeof(SET_DISK_ATTRIBUTES);
    attr.Persist = FALSE;
    attr.Attributes = offline ? 0x0000000000000001 : 0;
    attr.AttributesMask = 0x0000000000000001;
    DWORD br;
    return DeviceIoControl(h, IOCTL_DISK_SET_DISK_ATTRIBUTES, &attr,
                           sizeof(attr), NULL, 0, &br, NULL);
}

bool WriteSectorStrict(HANDLE h, LONGLONG lba, void *data, DWORD size) {
    LARGE_INTEGER li;
    li.QuadPart = lba * SECTOR_SIZE;
    DWORD bw;
    if(!SetFilePointerEx(h, li, NULL, FILE_BEGIN))
        return false;
    if(!WriteFile(h, data, size, &bw, NULL))
        return false;
    return FlushFileBuffers(h);
}

bool DoFormat(int index) {
    if(index == 0) {
        printf("\n[CRITICAL ERROR] Access to System Drive (Disk 0) is DENIED "
               "for safety.\n");
        return false;
    }

    char path[64];
    sprintf(path, "\\\\.\\PhysicalDrive%d", index);

    HANDLE hDisk = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                               OPEN_EXISTING, 0, NULL);
    if(hDisk == INVALID_HANDLE_VALUE) {
        printf("\n[ERROR] Could not open Disk %d. Please check Administrator "
               "privileges.\n",
               index);
        return false;
    }

    DWORD br;
    printf("\n>>> Initiating formatting sequence for Disk %d <<<\n\n", index);

    printf("[1/5] Detaching drive from OS (Dismount & Offline)...\n");
    DeviceIoControl(hDisk, FSCTL_ALLOW_EXTENDED_DASD_IO, NULL, 0, NULL, 0, &br,
                    NULL);
    DeviceIoControl(hDisk, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &br, NULL);
    DeviceIoControl(hDisk, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &br, NULL);
    if(!SetDiskOffline(hDisk, true)) {
        printf("      Warning: Could not set offline. OS might cache data.\n");
    }

    printf("[2/5] Initializing GPT partition table...\n");
    CREATE_DISK cd = {PARTITION_STYLE_GPT};
    DeviceIoControl(hDisk, IOCTL_DISK_CREATE_DISK, &cd, sizeof(cd), NULL, 0,
                    &br, NULL);

    DISK_GEOMETRY_EX geo;
    DeviceIoControl(hDisk, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, &geo,
                    sizeof(geo), &br, NULL);

    DWORD loSize =
        sizeof(DRIVE_LAYOUT_INFORMATION_EX) + sizeof(PARTITION_INFORMATION_EX);
    DRIVE_LAYOUT_INFORMATION_EX *lo =
        (DRIVE_LAYOUT_INFORMATION_EX *)calloc(1, loSize);
    lo->PartitionStyle = PARTITION_STYLE_GPT;
    lo->PartitionCount = 1;
    lo->PartitionEntry[0].StartingOffset.QuadPart =
        (LONGLONG)PARTITION_START_LBA * SECTOR_SIZE;
    LONGLONG partLen = (geo.DiskSize.QuadPart -
                        lo->PartitionEntry[0].StartingOffset.QuadPart) -
                       (2048 * SECTOR_SIZE);
    lo->PartitionEntry[0].PartitionLength.QuadPart = partLen;
    lo->PartitionEntry[0].PartitionNumber = 1;
    lo->PartitionEntry[0].RewritePartition = TRUE;
    // Microsoft Basic Data Partition GUID
    static const GUID DATA_GUID = {
        0xEBD0A0A2,
        0xB9E5,
        0x4433,
        {0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7}};
    lo->PartitionEntry[0].Gpt.PartitionType = DATA_GUID;
    DeviceIoControl(hDisk, IOCTL_DISK_SET_DRIVE_LAYOUT_EX, lo, loSize, NULL, 0,
                    &br, NULL);
    DeviceIoControl(hDisk, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &br,
                    NULL);

    printf("[3/5] Calculating FAT32 parameters...\n");
    DWORD totalSectors = (DWORD)(partLen / SECTOR_SIZE);
    DWORD sectorsPerFat = (DWORD)((((unsigned __int64)totalSectors /
                                    (CLUSTER_SIZE_32KB / SECTOR_SIZE)) *
                                       4 +
                                   SECTOR_SIZE - 1) /
                                  SECTOR_SIZE);

    FAT32_VBR vbr = {0};
    vbr.jmp[0] = 0xEB;
    vbr.jmp[1] = 0x58;
    vbr.jmp[2] = 0x90;
    memcpy(vbr.oem_name, "MSWIN4.1", 8);
    vbr.bytes_per_sector = SECTOR_SIZE;
    vbr.sectors_per_cluster = CLUSTER_SIZE_32KB / SECTOR_SIZE;
    vbr.reserved_sectors = RESERVED_SECTORS;
    vbr.fats = 2;
    vbr.media_type = 0xF8;
    vbr.hidden_sectors = PARTITION_START_LBA;
    vbr.sectors_32 = totalSectors;
    vbr.sectors_per_fat_32 = sectorsPerFat;
    vbr.root_cluster = 2;
    vbr.fs_info_sector = 1;
    vbr.backup_boot_sector = 6;
    vbr.boot_signature = 0x29;
    vbr.volume_id = GetTickCount();
    memcpy(vbr.volume_label, "FAT32_PRO  ", 11);
    memcpy(vbr.fs_type, "FAT32   ", 8);
    vbr.signature = 0xAA55;

    FAT32_FSINFO fsi = {0x41615252, {0}, 0x61417272, 0xFFFFFFFF,
                        0xFFFFFFFF, {0}, 0xAA550000};

    printf("[4/5] Writing FAT32 structures directly to physical disk...\n");
    WriteSectorStrict(hDisk, PARTITION_START_LBA, &vbr, 512);
    WriteSectorStrict(hDisk, PARTITION_START_LBA + 1, &fsi, 512);
    WriteSectorStrict(hDisk, PARTITION_START_LBA + 6, &vbr, 512);

    DWORD fatByteSize = sectorsPerFat * SECTOR_SIZE;
    BYTE *fatBuffer = (BYTE *)calloc(1, fatByteSize);

    DWORD fatStart[3] = {0x0FFFFFF8, 0xFFFFFFFF, 0x0FFFFFFF};
    memcpy(fatBuffer, fatStart, sizeof(fatStart));

    printf("      Writing FAT1 (%u sectors)...\n", (unsigned int)sectorsPerFat);
    WriteSectorStrict(hDisk, PARTITION_START_LBA + RESERVED_SECTORS, fatBuffer,
                      fatByteSize);

    printf("      Writing FAT2 (%u sectors)...\n", (unsigned int)sectorsPerFat);
    WriteSectorStrict(hDisk,
                      PARTITION_START_LBA + RESERVED_SECTORS + sectorsPerFat,
                      fatBuffer, fatByteSize);

    free(fatBuffer);

    BYTE *zero = calloc(1, CLUSTER_SIZE_32KB);
    WriteSectorStrict(
        hDisk, PARTITION_START_LBA + RESERVED_SECTORS + (2 * sectorsPerFat),
        zero, CLUSTER_SIZE_32KB);
    free(zero);

    printf("[5/5] Re-mounting volume and restoring OS access...\n");
    SetDiskOffline(hDisk, false);

    DeviceIoControl(hDisk, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &br,
                    NULL);

    DeviceIoControl(hDisk, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &br, NULL);

    printf("\n[SUCCESS] Disk %d has been successfully formatted to FAT32!\n",
           index);
    CloseHandle(hDisk);
    free(lo);
    return true;
}

int main(int argc, char *argv[]) {
    if(IsUserAnAdmin() == 0) {
        printf("[ERROR] This program requires Administrator privileges.\n");
        return 1;
    }

    if(argc < 2) {
        ShowHelp();
        return 0;
    }

    for(int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            ShowHelp();
            return 0;
        } else if(strcmp(argv[i], "-l") == 0 ||
                  strcmp(argv[i], "--list") == 0) {
            ListDrives();
            return 0;
        } else if(strcmp(argv[i], "-f") == 0 ||
                  strcmp(argv[i], "--format") == 0) {
            if(i + 1 < argc) {
                int idx = atoi(argv[i + 1]);
                return DoFormat(idx) ? 0 : 1;
            } else {
                printf("[ERROR] Missing drive index. Usage: fat32fmt -f "
                       "<index>\n");
                return 1;
            }
        }
    }

    ShowHelp();
    return 0;
}