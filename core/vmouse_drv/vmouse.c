/*
 * vmouse.c  –  KMDF + VHF 가상 마우스 HID 드라이버
 *
 * 역할: 유저모드 Python(DeviceIoControl) → 커널 → VHF 가상 마우스 HID
 *   게임이 Raw Input(WM_INPUT)으로 구독 → 버튼/이동 이벤트 수신
 *   dx=0, dy=0, buttons!=0  →  커서 이동 없이 버튼만 전달 (핵심 기능)
 *
 * 빌드 요구사항:
 *   Visual Studio 2019/2022 + WDK 10 (KMDF 1.15+, Windows 10 1507+)
 *   vhfkm.lib  링크 필요 (WDK에 포함)
 *
 * 설치:
 *   1) bcdedit /set testsigning on  (관리자 CMD, 재부팅)
 *   2) devcon install vmouse.inf Root\VMouse  (관리자 권한)
 */

#include <ntddk.h>
#include <wdf.h>
#include <vhf.h>

/* ─── HID Report Descriptor ───────────────────────────────────────────
   상대 마우스: 3버튼 + X/Y(각 16비트 signed) + 휠(8비트 signed)
   구조: [buttons:1byte][pad:5bit 패딩][x:2byte][y:2byte][wheel:1byte]
   총 리포트 크기: 1+1(버튼+패딩)+2+2+1 = 7 bytes
   ─────────────────────────────────────────────────────────────────── */
static const UCHAR g_HidDesc[] = {
    0x05, 0x01,        /* Usage Page: Generic Desktop Controls */
    0x09, 0x02,        /* Usage: Mouse                         */
    0xA1, 0x01,        /* Collection: Application              */
    0x09, 0x01,        /*   Usage: Pointer                     */
    0xA1, 0x00,        /*   Collection: Physical               */

    /* --- 버튼 3개 (1bit × 3) --- */
    0x05, 0x09,        /*     Usage Page: Button               */
    0x19, 0x01,        /*     Usage Minimum: 1                 */
    0x29, 0x03,        /*     Usage Maximum: 3                 */
    0x15, 0x00,        /*     Logical Minimum: 0               */
    0x25, 0x01,        /*     Logical Maximum: 1               */
    0x75, 0x01,        /*     Report Size: 1                   */
    0x95, 0x03,        /*     Report Count: 3                  */
    0x81, 0x02,        /*     Input: Data, Variable, Absolute  */

    /* --- 패딩 5bit (바이트 정렬) --- */
    0x75, 0x05,        /*     Report Size: 5                   */
    0x95, 0x01,        /*     Report Count: 1                  */
    0x81, 0x03,        /*     Input: Constant                  */

    /* --- X, Y 상대 이동 (각 16bit signed) --- */
    0x05, 0x01,        /*     Usage Page: Generic Desktop      */
    0x09, 0x30,        /*     Usage: X                         */
    0x09, 0x31,        /*     Usage: Y                         */
    0x16, 0x00, 0x80,  /*     Logical Minimum: -32768          */
    0x26, 0xFF, 0x7F,  /*     Logical Maximum:  32767          */
    0x75, 0x10,        /*     Report Size: 16                  */
    0x95, 0x02,        /*     Report Count: 2                  */
    0x81, 0x06,        /*     Input: Data, Variable, Relative  */

    /* --- 휠 (8bit signed) --- */
    0x09, 0x38,        /*     Usage: Wheel                     */
    0x15, 0x81,        /*     Logical Minimum: -127            */
    0x25, 0x7F,        /*     Logical Maximum:  127            */
    0x75, 0x08,        /*     Report Size: 8                   */
    0x95, 0x01,        /*     Report Count: 1                  */
    0x81, 0x06,        /*     Input: Data, Variable, Relative  */

    0xC0,              /*   End Collection                     */
    0xC0               /* End Collection                       */
};

/* Python/드라이버 공유 구조체 (packed, little-endian)
   총 7 bytes: buttons(1) + x(2) + y(2) + wheel(1) + pad(1) */
#pragma pack(push, 1)
typedef struct {
    UCHAR  buttons;   /* bit0=Left, bit1=Right, bit2=Middle   */
    SHORT  x;         /* 상대 X (픽셀, -32768~32767)          */
    SHORT  y;         /* 상대 Y (픽셀, -32768~32767)          */
    CHAR   wheel;     /* 휠 (-127~127, 양수=위)               */
} VMOUSE_REPORT, *PVMOUSE_REPORT;
#pragma pack(pop)

/*
 * IOCTL 코드 계산:
 *   CTL_CODE(FILE_DEVICE_UNKNOWN=0x22, Func=0x800,
 *            METHOD_BUFFERED=0, FILE_ANY_ACCESS=0)
 *   = (0x22<<16)|(0<<14)|(0x800<<2)|0 = 0x00222000
 */
#define IOCTL_VMOUSE_INJECT  0x00222000UL

#define VMOUSE_SYM_LINK  L"\\DosDevices\\VMouse"

static VHFHANDLE g_VhfHandle = NULL;

EVT_WDF_DRIVER_DEVICE_ADD          EvtDeviceAdd;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL EvtIoControl;

/* ─── DriverEntry ─────────────────────────────────────────── */
NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG cfg;
    WDF_DRIVER_CONFIG_INIT(&cfg, EvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath,
                           WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}

/* ─── EvtDeviceAdd ───────────────────────────────────────── */
NTSTATUS
EvtDeviceAdd(
    _In_    WDFDRIVER       Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS           status;
    WDFDEVICE          device;
    WDFQUEUE           queue;
    UNICODE_STRING     symLink;
    VHF_CONFIG         vhfCfg;
    WDF_IO_QUEUE_CONFIG qCfg;

    UNREFERENCED_PARAMETER(Driver);

    /* 디바이스 생성 */
    status = WdfDeviceCreate(&DeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &device);
    if (!NT_SUCCESS(status)) return status;

    /* \\.\VMouse 심볼릭 링크 → 유저모드에서 CreateFile로 열 수 있음 */
    RtlInitUnicodeString(&symLink, VMOUSE_SYM_LINK);
    status = WdfDeviceCreateSymbolicLink(device, &symLink);
    if (!NT_SUCCESS(status)) return status;

    /* IOCTL 처리 큐 (순차 처리) */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qCfg, WdfIoQueueDispatchSequential);
    qCfg.EvtIoDeviceControl = EvtIoControl;
    status = WdfIoQueueCreate(device, &qCfg, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status)) return status;

    /* DIAGNOSTIC: VHF disabled to isolate BSOD source.
       If driver loads without BSOD => VHF is the crash cause.
       If BSOD persists => KMDF/INF class is the cause. */
    UNREFERENCED_PARAMETER(vhfCfg);
    g_VhfHandle = NULL;
    return STATUS_SUCCESS;
}

/* ─── EvtIoControl ───────────────────────────────────────── */
VOID
EvtIoControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputLen,
    _In_ size_t     InputLen,
    _In_ ULONG      IoCode)
{
    NTSTATUS       status = STATUS_INVALID_DEVICE_REQUEST;
    PVMOUSE_REPORT report;
    size_t         size;
    HID_XFER_PACKET pkt;  /* WDK 28000: VhfReadReportSubmit 2-param 버전 */

    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(OutputLen);
    UNREFERENCED_PARAMETER(InputLen);

    if (IoCode == IOCTL_VMOUSE_INJECT && g_VhfHandle) {
        status = WdfRequestRetrieveInputBuffer(
            Request, sizeof(VMOUSE_REPORT), (PVOID*)&report, &size);

        if (NT_SUCCESS(status)) {
            /* WDK 28000: VhfReadReportSubmit(VhfHandle, PHID_XFER_PACKET) */
            pkt.reportBuffer    = (PUCHAR)report;
            pkt.reportBufferLen = sizeof(VMOUSE_REPORT);
            pkt.reportId        = 0;
            status = VhfReadReportSubmit(g_VhfHandle, &pkt);
        }
    }

    WdfRequestComplete(Request, status);
}
