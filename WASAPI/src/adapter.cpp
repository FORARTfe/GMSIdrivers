// SPDX-License-Identifier: GPL-2.0-only
/*
 * adapter.cpp  —  Adapter common + interrupt + PortCls entry
 *                 Guillemot Maxi Studio ISIS WDM/WaveRT driver
 */

#include "adapter.h"
#include "minwave.h"
#include "mintopo.h"

// ─── CMaestroAdapter ─────────────────────────────────────────────────────

class CMaestroAdapter : public IAdapterCommon, public CUnknown
{
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMaestroAdapter);
    ~CMaestroAdapter();

    CHardware*      m_pHW;
    PINTERRUPTSYNC  m_pInterruptSync;
    PDEVICE_OBJECT  m_pPDO;

    static NTSTATUS InterruptRoutine(
        _In_ PINTERRUPTSYNC SyncObject,
        _In_ PVOID DynamicContext);

public:
    // IAdapterCommon
    STDMETHODIMP_(NTSTATUS) Init(
        _In_ PRESOURCELIST rl,
        _In_ PDEVICE_OBJECT pdo);

    STDMETHODIMP_(PINTERRUPTSYNC) GetInterruptSync()
    { return m_pInterruptSync; }

    STDMETHODIMP_(CHardware*) GetHardware()
    { return m_pHW; }

    STDMETHODIMP_(NTSTATUS) InstallSubdevice(
        _In_  PDEVICE_OBJECT  pdo,
        _In_  PIRP            irp,
        _In_  PWSTR           name,
        _In_  REFGUID         PortClassId,
        _In_  REFGUID         MiniportClassId,
        _In_  ULONG           OutPortCLSID,
        _Out_ PUNKNOWN*       OutUnknown);
};

NTSTATUS NewAdapterCommon(
    _Out_ PUNKNOWN*  Unknown,
    _In_  REFCLSID   classId,
    _In_  PUNKNOWN   OuterUnknown,
    _In_  POOL_TYPE  PoolType)
{
    UNREFERENCED_PARAMETER(classId);

    CMaestroAdapter* p = new(PoolType, 'rtsM')
        CMaestroAdapter(OuterUnknown);
    if (!p) return STATUS_INSUFFICIENT_RESOURCES;

    *Unknown = (PUNKNOWN)(IAdapterCommon*)p;
    (*Unknown)->AddRef();
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMaestroAdapter::Init(
    _In_ PRESOURCELIST rl,
    _In_ PDEVICE_OBJECT pdo)
{
    m_pPDO = pdo;

    // ── Get I/O port base from resource list ──────────────────────────
    ULONG portIndex = 0;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR portDesc =
        rl->FindTranslatedPort(portIndex);

    if (!portDesc || portDesc->Type != CmResourceTypePort) {
        KdPrint(("maestro2em: no I/O port resource\n"));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    PULONG iobase = (PULONG)(ULONG_PTR)portDesc->u.Port.Start.QuadPart;

    // ── Get interrupt resource ─────────────────────────────────────────
    ULONG irqIndex = 0;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR irqDesc =
        rl->FindTranslatedInterrupt(irqIndex);

    if (!irqDesc) {
        KdPrint(("maestro2em: no interrupt resource\n"));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    // ── Create hardware object ─────────────────────────────────────────
    m_pHW = new(NonPagedPoolNx, 'HrtsM') CHardware();
    if (!m_pHW) return STATUS_INSUFFICIENT_RESOURCES;

    NTSTATUS status = m_pHW->Initialize(iobase, pdo);
    if (!NT_SUCCESS(status)) {
        delete m_pHW;
        m_pHW = nullptr;
        return status;
    }

    // ── Create interrupt sync ──────────────────────────────────────────
    status = PcNewInterruptSync(
        &m_pInterruptSync,
        nullptr,
        rl,
        0,
        InterruptSyncModeNormal);

    if (!NT_SUCCESS(status)) {
        m_pHW->Shutdown();
        delete m_pHW;
        m_pHW = nullptr;
        return status;
    }

    status = m_pInterruptSync->RegisterServiceRoutine(
        InterruptRoutine, (PVOID)this, FALSE);
    if (!NT_SUCCESS(status)) goto cleanup;

    status = m_pInterruptSync->Connect();
    if (!NT_SUCCESS(status)) goto cleanup;

    return STATUS_SUCCESS;

cleanup:
    m_pInterruptSync->Release();
    m_pInterruptSync = nullptr;
    m_pHW->Shutdown();
    delete m_pHW;
    m_pHW = nullptr;
    return status;
}

CMaestroAdapter::~CMaestroAdapter()
{
    if (m_pInterruptSync) {
        m_pInterruptSync->Disconnect();
        m_pInterruptSync->Release();
    }
    if (m_pHW) {
        m_pHW->Shutdown();
        delete m_pHW;
    }
}

// ─── ISR ─────────────────────────────────────────────────────────────────

NTSTATUS CMaestroAdapter::InterruptRoutine(
    _In_ PINTERRUPTSYNC /*sync*/,
    _In_ PVOID ctx)
{
    CMaestroAdapter* adapter = (CMaestroAdapter*)ctx;
    CHardware*       hw      = adapter->m_pHW;

    if (!hw) return STATUS_UNSUCCESSFUL;

    // Read IRQ status (BAR0+0x1A)
    UCHAR event = READ_PORT_UCHAR(
        (PUCHAR)((ULONG_PTR)hw + 0x1A));  // placeholder — hw exposes IoRead8
    if (!event) return STATUS_UNSUCCESSFUL;

    // Acknowledge WP interrupt
    WRITE_PORT_USHORT((PUSHORT)((ULONG_PTR)hw + 0x04),
                      READ_PORT_USHORT((PUSHORT)((ULONG_PTR)hw + 0x04)) & 1);

    // Acknowledge all
    WRITE_PORT_UCHAR((PUCHAR)((ULONG_PTR)hw + 0x1A), 0xFF);

    // DPC handled by miniport stream notification events
    hw->InterruptDpc();

    return STATUS_SUCCESS;
}

// ─── InstallSubdevice ─────────────────────────────────────────────────────

STDMETHODIMP_(NTSTATUS) CMaestroAdapter::InstallSubdevice(
    _In_  PDEVICE_OBJECT  pdo,
    _In_  PIRP            irp,
    _In_  PWSTR           name,
    _In_  REFGUID         PortClassId,
    _In_  REFGUID         MiniportClassId,
    _In_  ULONG           /*OutPortCLSID*/,
    _Out_ PUNKNOWN*       OutUnknown)
{
    PPORT     port;
    PMINIPORT miniport;
    PUNKNOWN  unknownPort     = nullptr;
    PUNKNOWN  unknownMiniport = nullptr;

    NTSTATUS status = PcNewPort(&port, PortClassId);
    if (!NT_SUCCESS(status)) return status;

    status = PcNewMiniport(&miniport, MiniportClassId);
    if (!NT_SUCCESS(status)) {
        port->Release();
        return status;
    }

    status = port->Init(pdo, irp, miniport,
                        (PUNKNOWN)(IAdapterCommon*)this,
                        m_pInterruptSync);

    miniport->Release();

    if (!NT_SUCCESS(status)) {
        port->Release();
        return status;
    }

    status = PcRegisterSubdevice(pdo, name, port);
    if (OutUnknown) {
        *OutUnknown = (PUNKNOWN)port;
        port->AddRef();
    }
    port->Release();
    return status;
}

// ─── Driver Entry Points ──────────────────────────────────────────────────

extern "C" NTSTATUS AddDevice(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PDEVICE_OBJECT  PhysicalDeviceObject)
{
    return PcAddAdapterDevice(DriverObject, PhysicalDeviceObject,
                              StartDevice, MAX_MINIPORTS, 0);
}

extern "C" NTSTATUS StartDevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP           Irp,
    _In_ PRESOURCELIST  ResourceList)
{
    PADAPTERCOMMON pAdapterCommon = nullptr;
    PUNKNOWN       pUnknownCommon = nullptr;

    NTSTATUS status = NewAdapterCommon(
        &pUnknownCommon, CLSID_NULL, nullptr, NonPagedPoolNx);
    if (!NT_SUCCESS(status)) return status;

    status = pUnknownCommon->QueryInterface(
        IID_IAdapterCommon, (PVOID*)&pAdapterCommon);
    pUnknownCommon->Release();
    if (!NT_SUCCESS(status)) return status;

    status = pAdapterCommon->Init(ResourceList, DeviceObject);
    if (!NT_SUCCESS(status)) {
        pAdapterCommon->Release();
        return status;
    }

    // Register adapter with PortCls
    status = PcRegisterAdapterPowerManagement(
        (PUNKNOWN)pAdapterCommon, DeviceObject);
    // (non-fatal if power management not available)

    // ── Install Wave subdevice (WaveRT — enables WASAPI) ──────────────
    status = pAdapterCommon->InstallSubdevice(
        DeviceObject, Irp,
        L"Wave",
        CLSID_PortWaveRT,
        CLSID_MiniportWaveRT_Maestro,  // defined in minwave.h
        0, nullptr);
    if (!NT_SUCCESS(status)) goto done;

    // ── Install Topology subdevice (AC97 mixer) ────────────────────────
    status = pAdapterCommon->InstallSubdevice(
        DeviceObject, Irp,
        L"Topology",
        CLSID_PortTopology,
        CLSID_MiniportTopology_Maestro, // defined in mintopo.h
        0, nullptr);

done:
    pAdapterCommon->Release();
    return status;
}

extern "C" NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    KdPrint(("maestro2em: DriverEntry v0.4\n"));
    return PcInitializeAdapterDriver(
        DriverObject, RegistryPath, AddDevice);
}
