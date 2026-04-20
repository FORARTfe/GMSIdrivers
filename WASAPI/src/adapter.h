// SPDX-License-Identifier: GPL-2.0-only
/*
 * adapter.h  —  IAdapterCommon interface for Maestro-2E WaveRT driver
 */
#pragma once
#include "hw.h"

// ─── IAdapterCommon ───────────────────────────────────────────────────────
DECLARE_INTERFACE_(IAdapterCommon, IUnknown)
{
    STDMETHOD_(NTSTATUS, Init)(THIS_
        _In_ PRESOURCELIST ResourceList,
        _In_ PDEVICE_OBJECT PDO) PURE;

    STDMETHOD_(PINTERRUPTSYNC, GetInterruptSync)(THIS) PURE;

    STDMETHOD_(CHardware*, GetHardware)(THIS) PURE;

    STDMETHOD_(NTSTATUS, InstallSubdevice)(THIS_
        _In_  PDEVICE_OBJECT  pdo,
        _In_  PIRP            irp,
        _In_  PWSTR           name,
        _In_  REFGUID         PortClassId,
        _In_  REFGUID         MiniportClassId,
        _In_  ULONG           OutPortCLSID,
        _Out_ PUNKNOWN*       OutUnknown) PURE;
};
typedef IAdapterCommon* PADAPTERCOMMON;

// Factory
NTSTATUS NewAdapterCommon(
    _Out_ PUNKNOWN*     Unknown,
    _In_  REFCLSID      /*classId*/,
    _In_  PUNKNOWN      OuterUnknown,
    _In_  POOL_TYPE     PoolType);
