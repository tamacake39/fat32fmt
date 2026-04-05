// ============================================================================
//  fat32fmt - FAT32 Formatter
//  物理ドライブをFAT32形式でフォーマットするツール (CUI/GUI両対応)
// ============================================================================

// インクルード順番が重要１
#include <windows.h>
// インクルード順番が重要２
#include <commctrl.h>

#include <shlobj.h>
#include <shlwapi.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <winioctl.h>

// ----------------------------------------------------------------------------
// マクロ定義
// ----------------------------------------------------------------------------
#define SECTOR_SIZE 512               // 1セクタのバイト数
#define CLUSTER_SIZE_32KB (32 * 1024) // 1クラスタのバイト数 (32KB)
#define RESERVED_SECTORS 32           // FAT32の予約セクタ数
#define PARTITION_START_LBA 2048      // パーティションの開始LBA (アライメント考慮)

// MinGW環境等で不足する定義の補完
#ifndef IOCTL_DISK_SET_DISK_ATTRIBUTES
#define IOCTL_DISK_SET_DISK_ATTRIBUTES 0x0007c0f0
#endif

// ----------------------------------------------------------------------------
// グローバル変数
// ----------------------------------------------------------------------------
HWND hCombo;               // GUI: ドライブ選択コンボボックスのハンドル
HWND hStatus;              // GUI: ステータス表示ラベルのハンドル
int g_SystemDiskIndex = 0; // システムがインストールされているドライブ番号 (保護用)

// ----------------------------------------------------------------------------
// 構造体定義
// ----------------------------------------------------------------------------

// ディスク属性設定用構造体 (オフライン化等に使用)
typedef struct {
    DWORD Version;
    BOOLEAN Persist;
    BYTE Reserved[3];
    DWORD64 Attributes;
    DWORD64 AttributesMask;
    DWORD Reserved2[4];
} SET_DISK_ATTRIBUTES;

#pragma pack(push, 1) // 構造体のアライメントを1バイト単位に設定

// FAT32 ボリュームブートレコード (VBR) 構造体
typedef struct {
    BYTE jmp[3];              // ジャンプ命令
    char oem_name[8];         // OEM名
    WORD bytes_per_sector;    // セクタあたりのバイト数
    BYTE sectors_per_cluster; // クラスタあたりのセクタ数
    WORD reserved_sectors;    // 予約セクタ数
    BYTE fats;                // FATの数
    WORD root_entries;        // ルートエントリ数 (FAT32では常に0)
    WORD sectors_16;          // 16ビットセクタ数 (FAT32では常に0)
    BYTE media_type;          // メディアタイプ (0xF8: 固定メディア)
    WORD sectors_per_fat_16;  // 16ビットFATあたりのセクタ数 (FAT32では0)
    WORD sectors_per_track;   // トラックあたりのセクタ数
    WORD heads;               // ヘッド数
    DWORD hidden_sectors;     // 隠しセクタ数
    DWORD sectors_32;         // 32ビットセクタ数
    DWORD sectors_per_fat_32; // 32ビットFATあたりのセクタ数
    WORD ext_flags;           // 拡張フラグ
    WORD fs_version;          // ファイルシステムバージョン
    DWORD root_cluster;       // ルートディレクトリの開始クラスタ
    WORD fs_info_sector;      // FSINFOセクタの番号
    WORD backup_boot_sector;  // バックアップブートセクタの番号
    BYTE reserved[12];        // 予約領域
    BYTE drive_number;        // ドライブ番号
    BYTE reserved1;           // 予約領域
    BYTE boot_signature;      // 拡張ブートシグネチャ (0x29)
    DWORD volume_id;          // ボリュームシリアル番号
    char volume_label[11];    // ボリュームラベル
    char fs_type[8];          // ファイルシステムタイプ ("FAT32   ")
    BYTE boot_code[420];      // ブートストラップコード
    WORD signature;           // ブートシグネチャ (0xAA55)
} FAT32_VBR;

// FAT32 ファイルシステム情報 (FSINFO) 構造体
typedef struct {
    DWORD lead_sig;      // リードシグネチャ (0x41615252)
    BYTE reserved1[480]; // 予約領域
    DWORD struc_sig;     // 構造体シグネチャ (0x61417272)
    DWORD free_count;    // 空きクラスタ数
    DWORD next_free;     // 次に割り当てる空きクラスタ番号
    BYTE reserved2[12];  // 予約領域
    DWORD trail_sig;     // トレイルシグネチャ (0xAA550000)
} FAT32_FSINFO;

#pragma pack(pop) // アライメント設定を元に戻す

// ----------------------------------------------------------------------------
// 関数定義
// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
// 機能: ヘルプメッセージを表示する
// ----------------------------------------------------------------------------
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

// ----------------------------------------------------------------------------
// 機能: 接続されている物理ドライブの一覧と情報を表示する
// ----------------------------------------------------------------------------
void ListDrives() {
    printf("Index\tSize (GB)\tStatus\n");
    printf("--------------------------------------------------\n");

    // PhysicalDrive0 から 15 までを走査
    for(int i = 0; i < 16; i++) {
        char path[64];
        sprintf(path, "\\\\.\\PhysicalDrive%d", i);

        // ドライブのハンドルを取得
        HANDLE h = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if(h != INVALID_HANDLE_VALUE) {
            DISK_GEOMETRY_EX geo;
            DWORD br;

            // ドライブのジオメトリ情報（サイズ等）を取得
            if(DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, &geo, sizeof(geo), &br, NULL)) {
                // サイズをGB単位に変換して出力
                double gb = (double)geo.DiskSize.QuadPart / (1024.0 * 1024.0 * 1024.0);
                printf("Disk %d\t%8.2f GB\t%s\n", i, gb, (i == g_SystemDiskIndex ? "[SYSTEM - PROTECTED]" : "Removable/Secondary"));
            }
            CloseHandle(h);
        }
    }
}

// ----------------------------------------------------------------------------
// 機能: OSがインストールされているシステムディスクのインデックスを取得する
// 戻り値: システムディスクのインデックス番号 (失敗時は安全のため0)
// ----------------------------------------------------------------------------
int GetSystemDiskIndex() {
    char winDir[MAX_PATH];
    char volPath[10];

    // Windowsディレクトリのパスを取得 (例: "C:\Windows")
    GetWindowsDirectoryA(winDir, MAX_PATH);

    // ドライブレター部分を抽出してボリュームパスを作成 (例: "\\.\C:")
    snprintf(volPath, sizeof(volPath), "\\\\.\\%.2s", winDir);

    HANDLE hVol = CreateFileA(volPath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if(hVol == INVALID_HANDLE_VALUE)
        return 0; // 失敗時は安全のためDisk 0をシステムドライブとみなす

    STORAGE_DEVICE_NUMBER sdn;
    DWORD br;
    int res = 0;

    // ストレージデバイス番号を取得
    if(DeviceIoControl(hVol, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &sdn, sizeof(sdn), &br, NULL)) {
        res = sdn.DeviceNumber;
    }
    CloseHandle(hVol);
    return res;
}

// ----------------------------------------------------------------------------
// 機能: ディスクのオフライン/オンライン状態を設定する
// 引数: h       - ディスクのハンドル
//       offline - true: オフライン, false: オンライン
// 戻り値: 成功時 true
// ----------------------------------------------------------------------------
bool SetDiskOffline(HANDLE h, bool offline) {
    SET_DISK_ATTRIBUTES attr = {0};
    attr.Version = sizeof(SET_DISK_ATTRIBUTES);
    attr.Persist = FALSE;                               // 再起動後には状態を維持しない
    attr.Attributes = offline ? 0x0000000000000001 : 0; // 1 = OFFLINE
    attr.AttributesMask = 0x0000000000000001;

    DWORD br;
    return DeviceIoControl(h, IOCTL_DISK_SET_DISK_ATTRIBUTES, &attr, sizeof(attr), NULL, 0, &br, NULL);
}

// ----------------------------------------------------------------------------
// 機能: 指定したLBA(論理ブロックアドレス)にデータを直接書き込む
// 引数: h    - ディスクのハンドル
//       lba  - 書き込み開始のLBA
//       data - 書き込むデータへのポインタ
//       size - 書き込みサイズ(バイト)
// 戻り値: 成功時 true
// ----------------------------------------------------------------------------
bool WriteSectorStrict(HANDLE h, LONGLONG lba, void *data, DWORD size) {
    LARGE_INTEGER li;
    li.QuadPart = lba * SECTOR_SIZE; // バイトオフセットを計算
    DWORD bw;

    // 書き込み位置にシーク
    if(!SetFilePointerEx(h, li, NULL, FILE_BEGIN))
        return false;

    // データを書き込む
    if(!WriteFile(h, data, size, &bw, NULL))
        return false;

    // バッファをフラッシュしてディスクへ確実に書き込む
    return FlushFileBuffers(h);
}

// ----------------------------------------------------------------------------
// 機能: 物理ディスクをFAT32形式でフォーマットするコア処理
// 引数: index - フォーマット対象の物理ディスク番号
// 戻り値: 成功時 true
// ----------------------------------------------------------------------------
bool DoFormat(int index) {
    // 1. システムドライブ保護チェック
    if(index == g_SystemDiskIndex) {
        fprintf(stderr, "[FATAL] Drive %d is the SYSTEM DRIVE. Protection active.\n", index);
        return false;
    }

    char path[64];
    sprintf(path, "\\\\.\\PhysicalDrive%d", index);

    // 2. ディスクのオープン (排他制御)
    HANDLE hDisk = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if(hDisk == INVALID_HANDLE_VALUE) {
        printf("\n[ERROR] Could not open Disk %d. Please check Administrator "
               "privileges.\n",
               index);
        return false;
    }

    DWORD br;
    printf("\n>>> Initiating formatting sequence for Disk %d <<<\n\n", index);

    // 3. ボリュームのマウント解除とオフライン化 (OSからのアクセスを遮断)
    printf("[1/5] Detaching drive from OS (Dismount & Offline)...\n");
    DeviceIoControl(hDisk, FSCTL_ALLOW_EXTENDED_DASD_IO, NULL, 0, NULL, 0, &br, NULL);
    DeviceIoControl(hDisk, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &br, NULL);
    DeviceIoControl(hDisk, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &br, NULL);
    if(!SetDiskOffline(hDisk, true)) {
        printf("      Warning: Could not set offline. OS might cache data.\n");
    }

    // 4. GPTパーティションテーブルの初期化と作成
    printf("[2/5] Initializing GPT partition table...\n");
    CREATE_DISK cd = {PARTITION_STYLE_GPT};
    DeviceIoControl(hDisk, IOCTL_DISK_CREATE_DISK, &cd, sizeof(cd), NULL, 0, &br, NULL);

    DISK_GEOMETRY_EX geo;
    DeviceIoControl(hDisk, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, &geo, sizeof(geo), &br, NULL);

    // パーティションレイアウトの設定
    DWORD loSize = sizeof(DRIVE_LAYOUT_INFORMATION_EX) + sizeof(PARTITION_INFORMATION_EX);
    DRIVE_LAYOUT_INFORMATION_EX *lo = (DRIVE_LAYOUT_INFORMATION_EX *)calloc(1, loSize);

    lo->PartitionStyle = PARTITION_STYLE_GPT;
    lo->PartitionCount = 1; // 1つのパーティションを作成

    // パーティションの開始位置とサイズを計算
    lo->PartitionEntry[0].StartingOffset.QuadPart = (LONGLONG)PARTITION_START_LBA * SECTOR_SIZE;
    LONGLONG partLen = (geo.DiskSize.QuadPart - lo->PartitionEntry[0].StartingOffset.QuadPart) - (2048 * SECTOR_SIZE);
    lo->PartitionEntry[0].PartitionLength.QuadPart = partLen;
    lo->PartitionEntry[0].PartitionNumber = 1;
    lo->PartitionEntry[0].RewritePartition = TRUE;

    // Microsoft Basic Data Partition の GUID を設定
    static const GUID DATA_GUID = {0xEBD0A0A2, 0xB9E5, 0x4433, {0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7}};
    lo->PartitionEntry[0].Gpt.PartitionType = DATA_GUID;

    DeviceIoControl(hDisk, IOCTL_DISK_SET_DRIVE_LAYOUT_EX, lo, loSize, NULL, 0, &br, NULL);
    DeviceIoControl(hDisk, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &br, NULL);

    // 5. FAT32パラメータの計算
    printf("[3/5] Calculating FAT32 parameters...\n");
    DWORD totalSectors = (DWORD)(partLen / SECTOR_SIZE);

    // FATサイズ(セクタ数)の計算
    DWORD sectorsPerFat = (DWORD)((((unsigned __int64)totalSectors / (CLUSTER_SIZE_32KB / SECTOR_SIZE)) * 4 + SECTOR_SIZE - 1) / SECTOR_SIZE);

    // VBR (Volume Boot Record) の構築
    FAT32_VBR vbr = {0};
    vbr.jmp[0] = 0xEB;
    vbr.jmp[1] = 0x58;
    vbr.jmp[2] = 0x90;
    memcpy(vbr.oem_name, "MSWIN4.1", 8);
    vbr.bytes_per_sector = SECTOR_SIZE;
    vbr.sectors_per_cluster = CLUSTER_SIZE_32KB / SECTOR_SIZE;
    vbr.reserved_sectors = RESERVED_SECTORS;
    vbr.fats = 2; // FATの数は2つ
    vbr.media_type = 0xF8;
    vbr.hidden_sectors = PARTITION_START_LBA;
    vbr.sectors_32 = totalSectors;
    vbr.sectors_per_fat_32 = sectorsPerFat;
    vbr.root_cluster = 2;
    vbr.fs_info_sector = 1;
    vbr.backup_boot_sector = 6;
    vbr.boot_signature = 0x29;
    vbr.volume_id = GetTickCount(); // ランダムなボリュームID
    memcpy(vbr.volume_label, "FAT32_PRO  ", 11);
    memcpy(vbr.fs_type, "FAT32   ", 8);
    vbr.signature = 0xAA55;

    // FSINFO の構築
    FAT32_FSINFO fsi = {0x41615252, {0}, 0x61417272, 0xFFFFFFFF, 0xFFFFFFFF, {0}, 0xAA550000};

    // 6. FAT32の各構造体をディスクへ直接書き込む
    printf("[4/5] Writing FAT32 structures directly to physical disk...\n");
    WriteSectorStrict(hDisk, PARTITION_START_LBA, &vbr, 512);     // プライマリVBR
    WriteSectorStrict(hDisk, PARTITION_START_LBA + 1, &fsi, 512); // プライマリFSINFO
    WriteSectorStrict(hDisk, PARTITION_START_LBA + 6, &vbr, 512); // バックアップVBR

    // FAT領域の初期化 (最初の2エントリはシステム予約)
    DWORD fatByteSize = sectorsPerFat * SECTOR_SIZE;
    BYTE *fatBuffer = (BYTE *)calloc(1, fatByteSize);
    DWORD fatStart[3] = {0x0FFFFFF8, 0xFFFFFFFF, 0x0FFFFFFF};
    memcpy(fatBuffer, fatStart, sizeof(fatStart));

    printf("      Writing FAT1 (%u sectors)...\n", (unsigned int)sectorsPerFat);
    WriteSectorStrict(hDisk, PARTITION_START_LBA + RESERVED_SECTORS, fatBuffer, fatByteSize);

    printf("      Writing FAT2 (%u sectors)...\n", (unsigned int)sectorsPerFat);
    WriteSectorStrict(hDisk, PARTITION_START_LBA + RESERVED_SECTORS + sectorsPerFat, fatBuffer, fatByteSize);

    free(fatBuffer);

    // ルートディレクトリ領域 (クラスタ2) のクリア
    BYTE *zero = calloc(1, CLUSTER_SIZE_32KB);
    WriteSectorStrict(hDisk, PARTITION_START_LBA + RESERVED_SECTORS + (2 * sectorsPerFat), zero, CLUSTER_SIZE_32KB);
    free(zero);

    // 7. ボリュームをオンラインに戻し、OSのアクセスを復元
    printf("[5/5] Re-mounting volume and restoring OS access...\n");
    SetDiskOffline(hDisk, false);
    DeviceIoControl(hDisk, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &br, NULL);
    DeviceIoControl(hDisk, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &br, NULL);

    printf("\n[SUCCESS] Disk %d has been successfully formatted to FAT32!\n", index);

    // リソースの解放
    CloseHandle(hDisk);
    free(lo);
    return true;
}

// ----------------------------------------------------------------------------
// GUI: ウィンドウプロシージャ
// ----------------------------------------------------------------------------
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch(uMsg) {
    case WM_CREATE: {
        // 1. UIコントロールの作成 (ラベル・コンボボックス・ステータス)
        CreateWindowA("STATIC", "Target:", WS_VISIBLE | WS_CHILD, 10, 12, 50, 20, hwnd, NULL, NULL, NULL);
        hCombo = CreateWindowA("COMBOBOX", "", WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST, 65, 10, 200, 200, hwnd, NULL, NULL, NULL);
        hStatus = CreateWindowA("STATIC", "Ready.", WS_VISIBLE | WS_CHILD, 10, 45, 350, 20, hwnd, NULL, NULL, NULL);

        // 2. ボタンの作成（START: ID 101, REFRESH: ID 102）
        CreateWindowA("BUTTON", "START", WS_VISIBLE | WS_CHILD, 275, 10, 70, 25, hwnd, (HMENU)101, NULL, NULL);
        CreateWindowA("BUTTON", "REFRESH", WS_VISIBLE | WS_CHILD, 350, 10, 70, 25, hwnd, (HMENU)102, NULL, NULL);

        // 初期起動時にドライブリストを読み込む (REFRESHコマンド発行)
        SendMessage(hwnd, WM_COMMAND, 102, 0);
        return 0;
    }

    case WM_COMMAND: {
        // --- 処理1: START ボタン押下時 (ID: 101) ---
        if(LOWORD(wParam) == 101) {
            // 選択されたドライブのインデックスを取得
            int sel = SendMessage(hCombo, CB_GETCURSEL, 0, 0);
            if(sel == CB_ERR)
                return 0;

            char buf[128];
            int diskIndex = -1;
            SendMessageA(hCombo, CB_GETLBTEXT, sel, (LPARAM)buf);
            sscanf(buf, "Disk %d", &diskIndex);

            // システムドライブの保護チェック
            if(diskIndex == g_SystemDiskIndex) {
                MessageBoxA(hwnd, "Cannot format System Drive!", "Protection", MB_ICONSTOP);
                return 0;
            }

            // フォーマット実行の最終確認
            if(MessageBoxA(hwnd, "Erase all data?", "Confirm", MB_YESNO | MB_ICONWARNING) == IDYES) {
                SetWindowTextA(hStatus, "Formatting... Please wait.");
                UpdateWindow(hStatus); // UIを即時更新
                // フォーマットの実行
                if(DoFormat(diskIndex)) {
                    SetWindowTextA(hStatus, "Success!");
                } else {
                    SetWindowTextA(hStatus, "Error: Failed.");
                }
            }
        }
        // --- 処理2: REFRESH ボタン押下時 (ID: 102) ---
        else if(LOWORD(wParam) == 102) {
            SetWindowTextA(hStatus, "Updating Drives.");
            // コンボボックスの中身をリセット
            SendMessage(hCombo, CB_RESETCONTENT, 0, 0);

            // ディスク一覧を再取得してコンボボックスに追加
            for(int i = 0; i < 16; i++) {
                char path[64];
                sprintf(path, "\\\\.\\PhysicalDrive%d", i);
                HANDLE h = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
                if(h != INVALID_HANDLE_VALUE) {
                    DISK_GEOMETRY_EX geo;
                    DWORD br;
                    if(DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, &geo, sizeof(geo), &br, NULL)) {
                        char item[128];
                        double gb = (double)geo.DiskSize.QuadPart / (1024.0 * 1024.0 * 1024.0);
                        const char *tag = (i == g_SystemDiskIndex) ? " [SYSTEM-PROTECTED]" : "";
                        sprintf(item, "Disk %d (%.2f GB)%s", i, gb, tag);

                        SendMessageA(hCombo, CB_ADDSTRING, 0, (LPARAM)item);
                    }
                    CloseHandle(h);
                }
            }
            // 最初のアイテムを選択状態にする
            SendMessage(hCombo, CB_SETCURSEL, 0, 0);
            SetWindowTextA(hStatus, "Ready.");
        }
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0); // アプリケーション終了
        return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

// ----------------------------------------------------------------------------
// GUI: ウィンドウの作成とメッセージループの開始
// ----------------------------------------------------------------------------
void StartGui(HINSTANCE hInstance) {
    InitCommonControls(); // コモンコントロールの初期化 (XPスタイル等の適用に必要)

    const char CLASS_NAME[] = "fat32fmt_gui";
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); // デフォルトのウィンドウ背景色

    RegisterClassA(&wc);

    // ウィンドウを作成
    HWND hwnd = CreateWindowExA(0, CLASS_NAME, "fat32fmt - FAT32 Formatter",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, // サイズ変更を禁止
                                CW_USEDEFAULT, CW_USEDEFAULT, 445, 110, NULL, NULL, hInstance, NULL);

    ShowWindow(hwnd, SW_SHOW);

    // メッセージループ
    MSG msg;
    while(GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

// ----------------------------------------------------------------------------
// メインエントリポイント
// ----------------------------------------------------------------------------
int main(int argc, char *argv[]) {
    // 1. 管理者権限のチェック
    if(IsUserAnAdmin() == 0) {
        printf("[ERROR] This program requires Administrator privileges.\n");
        return 1;
    }

    // 2. システムディスク番号の特定（安全機能）
    g_SystemDiskIndex = GetSystemDiskIndex();

    // 3. 実行引数の判定
    if(argc < 2) {
        // 引数なしの場合：コンソールを切り離してGUIモードで起動
        FreeConsole();
        StartGui(GetModuleHandle(NULL));
        return 0; // GUIウィンドウが閉じられたらプログラム終了
    }

    // 4. コマンドライン引数の解析 (CUIモード)
    for(int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            ShowHelp();
            return 0;
        } else if(strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--list") == 0) {
            ListDrives();
            return 0;
        } else if(strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--format") == 0) {
            if(i + 1 < argc) {
                int idx = atoi(argv[i + 1]);
                return DoFormat(idx) ? 0 : 1; // フォーマット成功で0、失敗で1を返す
            } else {
                printf("[ERROR] Missing drive index. Usage: fat32fmt -f <index>\n");
                return 1;
            }
        }
    }

    // 認識できない引数の場合はヘルプを表示
    ShowHelp();
    return 0;
}