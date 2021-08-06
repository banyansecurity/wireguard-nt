/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2015-2021 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 */

/* We pretend we're Windows 8, and then hack around the limitation in Windows 7 below. */
#include <ntifs.h> /* Must be included before <wdm.h> */
#include <wdm.h>
#if NTDDI_VERSION == NTDDI_WIN7
#    undef NTDDI_VERSION
#    define NTDDI_VERSION NTDDI_WIN8
#    include <wsk.h>
#    undef NTDDI_VERSION
#    define NTDDI_VERSION NTDDI_WIN7
#endif

#include "device.h"
#include "messages.h"
#include "peer.h"
#include "queueing.h"
#include "rcu.h"
#include "socket.h"
#include "logging.h"
#include <wsk.h>
#include <netioapi.h>
/* The headers say that UDP_NOCHECKSUM is defined in ws2tcpip.h for hysterical raisins,
 * which we can't include from kernel space.
 */
#define UDP_NOCHECKSUM 1

static LONG RoutingGenerationV4 = 1, RoutingGenerationV6 = 1;
static HANDLE RouteNotifierV4, RouteNotifierV6;
static CONST WSK_CLIENT_DISPATCH WskAppDispatchV1 = { .Version = MAKE_WSK_VERSION(1, 0) };
static WSK_REGISTRATION WskRegistration;
static WSK_PROVIDER_NPI WskProviderNpi;
static BOOLEAN WskHasIpv4Transport, WskHasIpv6Transport;
static NTSTATUS WskInitStatus = STATUS_RETRY;
static EX_PUSH_LOCK WskIsIniting;
static LOOKASIDE_ALIGN LOOKASIDE_LIST_EX SocketSendCtxCache;

#define NET_BUFFER_WSK_BUF(Nb) ((WSK_BUF_LIST *)&NET_BUFFER_MINIPORT_RESERVED(Nb)[0])
static_assert(
    sizeof(NET_BUFFER_MINIPORT_RESERVED((NET_BUFFER *)0)) >= sizeof(WSK_BUF_LIST),
    "WSK_BUF_LIST is too large for NB");

typedef struct _SOCKET_SEND_CTX
{
    IRP Irp;
    IO_STACK_LOCATION IoStackData;
    ENDPOINT Endpoint;
    WG_DEVICE *Wg;
    union
    {
        NET_BUFFER_LIST *FirstNbl;
        WSK_BUF Buffer;
    };
    BOOLEAN IsNbl;
} SOCKET_SEND_CTX;

static IO_COMPLETION_ROUTINE SendComplete;
_Use_decl_annotations_
static NTSTATUS
SendComplete(DEVICE_OBJECT *DeviceObject, IRP *Irp, VOID *VoidCtx)
{
    SOCKET_SEND_CTX *Ctx = VoidCtx;
    _Analysis_assume_(Ctx);
    if (Ctx->IsNbl)
        FreeSendNetBufferList(Ctx->Wg, Ctx->FirstNbl, 0);
    else
        MemFreeDataAndMdlChain(Ctx->Buffer.Mdl);
    ExFreeToLookasideListEx(&SocketSendCtxCache, Ctx);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

#if NTDDI_VERSION == NTDDI_WIN7
static BOOLEAN NoWskSendMessages;

typedef struct _POLYFILLED_SOCKET_SEND_CTX
{
    IRP Irp;
    IO_STACK_LOCATION IoStackData;
    PIRP OriginalIrp;
    LONG *RefCount;
} POLYFILLED_SOCKET_SEND_CTX;

static IO_COMPLETION_ROUTINE PolyfilledSendComplete;
_Use_decl_annotations_
static NTSTATUS
PolyfilledSendComplete(DEVICE_OBJECT *DeviceObject, IRP *Irp, VOID *VoidCtx)
{
    POLYFILLED_SOCKET_SEND_CTX *Ctx = VoidCtx;
    _Analysis_assume_(Ctx);
    if (!InterlockedDecrement(Ctx->RefCount))
    {
        IO_STACK_LOCATION *Stack = IoGetNextIrpStackLocation(Ctx->OriginalIrp);
        if (Stack && Stack->CompletionRoutine)
            Stack->CompletionRoutine(DeviceObject, Ctx->OriginalIrp, Stack->Context);
        MemFree(Ctx->RefCount);
    }
    MemFree(Ctx);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS
PolyfilledWskSendMessages(
    _In_ PWSK_SOCKET Socket,
    _In_ PWSK_BUF_LIST BufferList,
    _Reserved_ ULONG Flags,
    _In_opt_ PSOCKADDR RemoteAddress,
    _In_ ULONG ControlInfoLength,
    _In_reads_bytes_opt_(ControlInfoLength) PCMSGHDR ControlInfo,
    _Inout_ PIRP Irp)
{
#    pragma warning(suppress : 6014) /* `RefCount` is freed in PolyfilledSendComplete. */
    LONG *RefCount = MemAllocate(sizeof(*RefCount));
    if (!RefCount)
        return STATUS_INSUFFICIENT_RESOURCES;
    WriteNoFence(RefCount, 1);
    for (WSK_BUF_LIST *Buf = BufferList; Buf; Buf = Buf->Next)
    {
        POLYFILLED_SOCKET_SEND_CTX *Ctx = MemAllocate(sizeof(*Ctx));
        if (!Ctx)
            continue;
        Ctx->RefCount = RefCount;
        Ctx->OriginalIrp = Irp;
        IoInitializeIrp(&Ctx->Irp, sizeof(Ctx->IoStackData) + sizeof(Ctx->Irp), 1);
        IoSetCompletionRoutine(&Ctx->Irp, PolyfilledSendComplete, Ctx, TRUE, TRUE, TRUE);
        InterlockedIncrement(RefCount);
        ((WSK_PROVIDER_DATAGRAM_DISPATCH *)Socket->Dispatch)
            ->WskSendTo(Socket, &Buf->Buffer, Flags, RemoteAddress, ControlInfoLength, ControlInfo, &Ctx->Irp);
    }
    if (!InterlockedDecrement(RefCount))
    {
        IO_STACK_LOCATION *Stack = IoGetNextIrpStackLocation(Irp);
        if (Stack && Stack->CompletionRoutine)
            Stack->CompletionRoutine((DEVICE_OBJECT *)Socket, Irp, Stack->Context);
        MemFree(RefCount);
    }
    return STATUS_SUCCESS;
}
#endif

/* This function expects to have Ctx's Endpoint and *either* Buffer or FirstNbl
 * fields filled; it takes care of the rest. It will free/consume Ctx when
 * STATUS_SUCCESS is returned. Note that STATUS_SUCCESS means the actual
 * sending might fail asynchronously later on.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
static NTSTATUS
SendAsync(_In_ WG_DEVICE *Wg, _In_ __drv_aliasesMem SOCKET_SEND_CTX *Ctx)
{
    SOCKET *Socket = NULL;
    NTSTATUS Status;

    Ctx->Wg = Wg;
    IoInitializeIrp(&Ctx->Irp, sizeof(Ctx->IoStackData) + sizeof(Ctx->Irp), 1);
    IoSetCompletionRoutine(&Ctx->Irp, SendComplete, Ctx, TRUE, TRUE, TRUE);

    KIRQL Irql = RcuReadLock();
    if (Ctx->Endpoint.Addr.si_family == AF_INET)
        Socket = RcuDereference(SOCKET, Wg->Sock4);
    else if (Ctx->Endpoint.Addr.si_family == AF_INET6)
        Socket = RcuDereference(SOCKET, Wg->Sock6);
    if (!Socket)
    {
        Status = STATUS_NETWORK_UNREACHABLE;
        goto cleanup;
    }
    PFN_WSK_SEND_MESSAGES WskSendMessages = ((WSK_PROVIDER_DATAGRAM_DISPATCH *)Socket->Sock->Dispatch)->WskSendMessages;
#if NTDDI_VERSION == NTDDI_WIN7
    if (NoWskSendMessages)
        WskSendMessages = PolyfilledWskSendMessages;
#endif
    if (Ctx->IsNbl)
        WskSendMessages(
            Socket->Sock,
            NET_BUFFER_WSK_BUF(NET_BUFFER_LIST_FIRST_NB(Ctx->FirstNbl)),
            0,
            (PSOCKADDR)&Ctx->Endpoint.Addr,
            (ULONG)WSA_CMSGDATA_ALIGN(Ctx->Endpoint.SrcCmsghdr.cmsg_len),
            &Ctx->Endpoint.SrcCmsghdr,
            &Ctx->Irp);
    else
        ((WSK_PROVIDER_DATAGRAM_DISPATCH *)Socket->Sock->Dispatch)
            ->WskSendTo(
                Socket->Sock,
                &Ctx->Buffer,
                0,
                (PSOCKADDR)&Ctx->Endpoint.Addr,
                (ULONG)WSA_CMSGDATA_ALIGN(Ctx->Endpoint.SrcCmsghdr.cmsg_len),
                &Ctx->Endpoint.SrcCmsghdr,
                &Ctx->Irp);
    Status = STATUS_SUCCESS;
cleanup:
    RcuReadUnlock(Irql);
    return Status;
}

static BOOLEAN
CidrMaskMatchV4(_In_ CONST IN_ADDR *Addr, _In_ CONST IP_ADDRESS_PREFIX *Prefix)
{
    return Prefix->PrefixLength == 0 ||
           (Addr->s_addr & (Htonl(~0U << (32 - Prefix->PrefixLength)))) == Prefix->Prefix.Ipv4.sin_addr.s_addr;
}

static BOOLEAN
CidrMaskMatchV6(_In_ CONST IN6_ADDR *Addr, _In_ CONST IP_ADDRESS_PREFIX *Prefix)
{
    if (Prefix->PrefixLength == 0)
        return TRUE;
    ULONG WholeParts = Prefix->PrefixLength / 32;
    ULONG LeftoverBits = Prefix->PrefixLength % 32;
    if (!RtlEqualMemory(&Prefix->Prefix.Ipv6.sin6_addr, Addr, WholeParts * sizeof(UINT32)))
        return FALSE;
    if (WholeParts == 4 || LeftoverBits == 0)
        return TRUE;
    return (((UINT32 *)Addr)[WholeParts] & Htonl(~0U << (32 - LeftoverBits))) ==
           ((UINT32 *)&Prefix->Prefix.Ipv6.sin6_addr)[WholeParts];
}

_IRQL_requires_max_(PASSIVE_LEVEL)
_IRQL_raises_(DISPATCH_LEVEL)
_Requires_lock_not_held_(Peer->EndpointLock)
_Acquires_lock_(Peer->EndpointLock)
static NTSTATUS
SocketResolvePeerEndpointSrc(_Inout_ WG_PEER *Peer, _Out_ _At_(*Irql, _IRQL_saves_) KIRQL *Irql)
{
    NTSTATUS Status = STATUS_INVALID_PARAMETER;
    ENDPOINT *Endpoint = &Peer->Endpoint;
    UINT32 UpdateGeneration;

    /* TODO: We should probably cache the results of this to avoid a DoS,
     * whereby a client sends pings that change src address, resulting in new
     * lookups on the pong.
     */
retry:
    *Irql = ExAcquireSpinLockShared(&Peer->EndpointLock);
    UpdateGeneration = Endpoint->UpdateGeneration;
    if (Endpoint->Addr.si_family == AF_INET &&
        Endpoint->RoutingGeneration == (UINT32)ReadNoFence(&RoutingGenerationV4) && Endpoint->Src4.ipi_ifindex)
        return STATUS_SUCCESS;
    if (Endpoint->Addr.si_family == AF_INET6 &&
        Endpoint->RoutingGeneration == (UINT32)ReadNoFence(&RoutingGenerationV6) && Endpoint->Src6.ipi6_ifindex)
        return STATUS_SUCCESS;
    SOCKADDR_INET SrcAddr = { 0 };
    ExReleaseSpinLockShared(&Peer->EndpointLock, *Irql);
    ULONG BestIndex = 0, BestCidr = 0, BestMetric = ~0UL;
    NET_LUID BestLuid = { 0 };
    MIB_IPFORWARD_TABLE2 *Table;
    Status = GetIpForwardTable2(Endpoint->Addr.si_family, &Table);
    if (!NT_SUCCESS(Status))
        return Status;
    union
    {
        MIB_IF_ROW2 Interface;
        MIB_IPINTERFACE_ROW IpInterface;
    } *If = MemAllocate(sizeof(*If));
    if (!If)
        return STATUS_INSUFFICIENT_RESOURCES;
    for (ULONG i = 0; i < Table->NumEntries; ++i)
    {
        if (Table->Table[i].InterfaceLuid.Value == Peer->Device->InterfaceLuid.Value)
            continue;
        if (Table->Table[i].DestinationPrefix.PrefixLength < BestCidr)
            continue;
        if (Endpoint->Addr.si_family == AF_INET &&
            !CidrMaskMatchV4(&Endpoint->Addr.Ipv4.sin_addr, &Table->Table[i].DestinationPrefix))
            continue;
        if (Endpoint->Addr.si_family == AF_INET6 &&
            !CidrMaskMatchV6(&Endpoint->Addr.Ipv6.sin6_addr, &Table->Table[i].DestinationPrefix))
            continue;
        If->Interface = (MIB_IF_ROW2){ .InterfaceLuid = Table->Table[i].InterfaceLuid };
        if (!NT_SUCCESS(GetIfEntry2(&If->Interface)) || If->Interface.OperStatus != IfOperStatusUp)
            continue;
        If->IpInterface =
            (MIB_IPINTERFACE_ROW){ .Family = Endpoint->Addr.si_family, .InterfaceLuid = Table->Table[i].InterfaceLuid };
        if (!NT_SUCCESS(GetIpInterfaceEntry(&If->IpInterface)))
            continue;
        ULONG Metric = Table->Table[i].Metric + If->IpInterface.Metric;
        if (Table->Table[i].DestinationPrefix.PrefixLength == BestCidr && Metric > BestMetric)
            continue;
        BestCidr = Table->Table[i].DestinationPrefix.PrefixLength;
        BestMetric = Metric;
        BestIndex = Table->Table[i].InterfaceIndex;
        BestLuid = Table->Table[i].InterfaceLuid;
    }
    MemFree(If);
    if (Table->NumEntries && BestIndex)
        Status = GetBestRoute2(NULL, BestIndex, NULL, &Endpoint->Addr, 0, &Table->Table[0], &SrcAddr);
    FreeMibTable(Table);
    if (!NT_SUCCESS(Status))
        return Status;
    *Irql = ExAcquireSpinLockExclusive(&Peer->EndpointLock);
    if (Endpoint->UpdateGeneration != UpdateGeneration)
    {
        ExReleaseSpinLockExclusive(&Peer->EndpointLock, *Irql);
        goto retry;
    }
    if (Endpoint->Addr.si_family == AF_INET)
    {
        Endpoint->RoutingGeneration = ReadNoFence(&RoutingGenerationV4);
        Endpoint->Src4.ipi_addr = SrcAddr.Ipv4.sin_addr;
        Endpoint->Src4.ipi_ifindex = BestIndex;
        Endpoint->SrcCmsghdr.cmsg_len = WSA_CMSG_LEN(sizeof(Endpoint->Src4));
        Endpoint->SrcCmsghdr.cmsg_level = IPPROTO_IP;
        Endpoint->SrcCmsghdr.cmsg_type = IP_PKTINFO;
    }
    else if (Endpoint->Addr.si_family == AF_INET6)
    {
        Endpoint->RoutingGeneration = ReadNoFence(&RoutingGenerationV6);
        Endpoint->Src6.ipi6_addr = SrcAddr.Ipv6.sin6_addr;
        Endpoint->Src6.ipi6_ifindex = BestIndex;
        Endpoint->SrcCmsghdr.cmsg_len = WSA_CMSG_LEN(sizeof(Endpoint->Src6));
        Endpoint->SrcCmsghdr.cmsg_level = IPPROTO_IPV6;
        Endpoint->SrcCmsghdr.cmsg_type = IPV6_PKTINFO;
    }
    else
        BestIndex = 0;
    ++Endpoint->UpdateGeneration, ++UpdateGeneration;
    ExReleaseSpinLockExclusive(&Peer->EndpointLock, *Irql);
    if (!BestIndex)
        return STATUS_BAD_NETWORK_PATH;
    *Irql = ExAcquireSpinLockShared(&Peer->EndpointLock);
    if (Endpoint->UpdateGeneration != UpdateGeneration)
    {
        ExReleaseSpinLockShared(&Peer->EndpointLock, *Irql);
        goto retry;
    }
    return STATUS_SUCCESS;
}

#pragma warning(suppress : 28194) /* `Nbl` is aliased in Ctx->Nbl or freed on failure. */
#pragma warning(suppress : 28167) /* IRQL is either not raised on SocketResolvePeerEndpointSrc failure, or \
                                     restored by ExReleaseSpinLockShared */
_Use_decl_annotations_
NTSTATUS
SocketSendNblsToPeer(WG_PEER *Peer, NET_BUFFER_LIST *First, BOOLEAN *AllKeepalive)
{
    if (!First)
        return STATUS_ALREADY_COMPLETE;
    NTSTATUS Status = STATUS_INSUFFICIENT_RESOURCES;
    SOCKET_SEND_CTX *Ctx = ExAllocateFromLookasideListEx(&SocketSendCtxCache);
    if (!Ctx)
        goto cleanupNbls;
    KIRQL Irql;
    Status = SocketResolvePeerEndpointSrc(Peer, &Irql); /* Takes read-side EndpointLock */
    if (!NT_SUCCESS(Status))
        goto cleanupCtx;
    Ctx->Endpoint = Peer->Endpoint;
    ExReleaseSpinLockShared(&Peer->EndpointLock, Irql);
    Ctx->IsNbl = TRUE;
    Ctx->FirstNbl = First;
    *AllKeepalive = TRUE;

    WSK_BUF_LIST *FirstWskBuf = NULL, *LastWskBuf = NULL;
    ULONG64 DataLength = 0, Packets = 0;
    for (NET_BUFFER_LIST *Nbl = First; Nbl; Nbl = NET_BUFFER_LIST_NEXT_NBL(Nbl))
    {
        for (NET_BUFFER *Nb = NET_BUFFER_LIST_FIRST_NB(Nbl); Nb; Nb = NET_BUFFER_NEXT_NB(Nb))
        {
            NET_BUFFER_WSK_BUF(Nb)->Buffer.Mdl = NET_BUFFER_CURRENT_MDL(Nb);
            NET_BUFFER_WSK_BUF(Nb)->Buffer.Length = NET_BUFFER_DATA_LENGTH(Nb);
            NET_BUFFER_WSK_BUF(Nb)->Buffer.Offset = NET_BUFFER_CURRENT_MDL_OFFSET(Nb);
            NET_BUFFER_WSK_BUF(Nb)->Next = NULL;
            *(LastWskBuf ? &LastWskBuf->Next : &FirstWskBuf) = NET_BUFFER_WSK_BUF(Nb);
            LastWskBuf = NET_BUFFER_WSK_BUF(Nb);
            DataLength += NET_BUFFER_DATA_LENGTH(Nb);
            ++Packets;
            if (NET_BUFFER_DATA_LENGTH(Nb) != MessageDataLen(0))
                *AllKeepalive = FALSE;
        }
    }
    Status = SendAsync(Peer->Device, Ctx);
    if (!NT_SUCCESS(Status))
        goto cleanupCtx;
    Peer->TxBytes += DataLength;
    Peer->Device->Statistics.ifHCOutOctets += DataLength;
    Peer->Device->Statistics.ifHCOutUcastOctets += DataLength;
    Peer->Device->Statistics.ifHCOutUcastPkts += Packets;
    return STATUS_SUCCESS;

cleanupCtx:
    ExFreeToLookasideListEx(&SocketSendCtxCache, Ctx);
cleanupNbls:
    FreeSendNetBufferList(Peer->Device, First, 0);
    return Status;
}

#pragma warning(suppress : 28167) /* IRQL is either not raised on SocketResolvePeerEndpointSrc failure, or \
                                     restored by ExReleaseSpinLockShared */
_Use_decl_annotations_
NTSTATUS
SocketSendBufferToPeer(WG_PEER *Peer, CONST VOID *Buffer, ULONG Len)
{
    NTSTATUS Status;
    SOCKET_SEND_CTX *Ctx = ExAllocateFromLookasideListEx(&SocketSendCtxCache);
    if (!Ctx)
        return STATUS_INSUFFICIENT_RESOURCES;
    Ctx->IsNbl = FALSE;
    Ctx->Buffer.Length = Len;
    Ctx->Buffer.Offset = 0;
    Ctx->Buffer.Mdl = MemAllocateDataAndMdlChain(Len);
    if (!Ctx->Buffer.Mdl)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto cleanupCtx;
    }
    RtlCopyMemory(MmGetMdlVirtualAddress(Ctx->Buffer.Mdl), Buffer, Len);
    KIRQL Irql;
    Status = SocketResolvePeerEndpointSrc(Peer, &Irql); /* Takes read-side endpoint_lock */
    if (!NT_SUCCESS(Status))
        goto cleanupMdl;
    Ctx->Endpoint = Peer->Endpoint;
    ExReleaseSpinLockShared(&Peer->EndpointLock, Irql);
    Status = SendAsync(Peer->Device, Ctx);
    if (!NT_SUCCESS(Status))
        goto cleanupMdl;
    Peer->TxBytes += Len;
    return STATUS_SUCCESS;
cleanupMdl:
    MemFreeDataAndMdlChain(Ctx->Buffer.Mdl);
cleanupCtx:
    ExFreeToLookasideListEx(&SocketSendCtxCache, Ctx);
    return Status;
}

_Use_decl_annotations_
NTSTATUS
SocketSendBufferAsReplyToNbl(WG_DEVICE *Wg, CONST NET_BUFFER_LIST *InNbl, CONST VOID *Buffer, ULONG Len)
{
    NTSTATUS Status;
    SOCKET_SEND_CTX *Ctx = ExAllocateFromLookasideListEx(&SocketSendCtxCache);
    if (!Ctx)
        return STATUS_INSUFFICIENT_RESOURCES;
    Ctx->IsNbl = FALSE;
    Ctx->Buffer.Length = Len;
    Ctx->Buffer.Offset = 0;
    Ctx->Buffer.Mdl = MemAllocateDataAndMdlChain(Len);
    if (!Ctx->Buffer.Mdl)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto cleanupCtx;
    }
    RtlCopyMemory(MmGetMdlVirtualAddress(Ctx->Buffer.Mdl), Buffer, Len);
    Status = SocketEndpointFromNbl(&Ctx->Endpoint, InNbl);
    if (!NT_SUCCESS(Status))
        goto cleanupMdl;
    Status = SendAsync(Wg, Ctx);
    if (!NT_SUCCESS(Status))
        goto cleanupMdl;
    return STATUS_SUCCESS;
cleanupMdl:
    MemFreeDataAndMdlChain(Ctx->Buffer.Mdl);
cleanupCtx:
    ExFreeToLookasideListEx(&SocketSendCtxCache, Ctx);
    return Status;
}

_Post_maybenull_
static VOID *
FindInCmsgHdr(_In_ WSK_DATAGRAM_INDICATION *Data, _In_ CONST INT Level, _In_ CONST INT Type)
{
    SIZE_T Len = Data->ControlInfoLength;
    WSACMSGHDR *Hdr = Data->ControlInfo;

    while (Len > 0 && Hdr)
    {
        if (Hdr->cmsg_level == Level && Hdr->cmsg_type == Type)
            return (VOID *)WSA_CMSG_DATA(Hdr);
        Len -= WSA_CMSGHDR_ALIGN(Hdr->cmsg_len);
        Hdr = (WSACMSGHDR *)((UCHAR *)Hdr + WSA_CMSGHDR_ALIGN(Hdr->cmsg_len));
    }
    return NULL;
}

_Use_decl_annotations_
NTSTATUS
SocketEndpointFromNbl(ENDPOINT *Endpoint, CONST NET_BUFFER_LIST *Nbl)
{
    WSK_DATAGRAM_INDICATION *Data = NET_BUFFER_LIST_DATAGRAM_INDICATION(Nbl);
    SOCKADDR *Addr = Data->RemoteAddress;
    VOID *Pktinfo;
    RtlZeroMemory(Endpoint, sizeof(*Endpoint));
    if (Addr->sa_family == AF_INET && (Pktinfo = FindInCmsgHdr(Data, IPPROTO_IP, IP_PKTINFO)) != NULL)
    {
        Endpoint->Addr.Ipv4 = *(SOCKADDR_IN *)Addr;
        Endpoint->Src4 = *(IN_PKTINFO *)Pktinfo;
        Endpoint->RoutingGeneration = ReadNoFence(&RoutingGenerationV4);
        Endpoint->SrcCmsghdr.cmsg_len = WSA_CMSG_LEN(sizeof(Endpoint->Src4));
        Endpoint->SrcCmsghdr.cmsg_level = IPPROTO_IP;
        Endpoint->SrcCmsghdr.cmsg_type = IP_PKTINFO;
    }
    else if (Addr->sa_family == AF_INET6 && (Pktinfo = FindInCmsgHdr(Data, IPPROTO_IPV6, IPV6_PKTINFO)) != NULL)
    {
        Endpoint->Addr.Ipv6 = *(SOCKADDR_IN6 *)Addr;
        Endpoint->Src6 = *(IN6_PKTINFO *)Pktinfo;
        Endpoint->RoutingGeneration = ReadNoFence(&RoutingGenerationV6);
        Endpoint->SrcCmsghdr.cmsg_len = WSA_CMSG_LEN(sizeof(Endpoint->Src6));
        Endpoint->SrcCmsghdr.cmsg_level = IPPROTO_IPV6;
        Endpoint->SrcCmsghdr.cmsg_type = IPV6_PKTINFO;
    }
    else
        return STATUS_INVALID_ADDRESS;
    return STATUS_SUCCESS;
}

static inline BOOLEAN
Ipv6AddrEq(_In_ CONST IN6_ADDR *A1, _In_ CONST IN6_ADDR *A2)
{
    UINT64 *B1 = (UINT64 *)A1, *B2 = (UINT64 *)A2;
    return ((B1[0] ^ B2[0]) | (B1[1] ^ B2[1])) == 0;
}

static BOOLEAN
EndpointEq(_In_ CONST ENDPOINT *A, _In_ CONST ENDPOINT *B)
{
    return (A->Addr.si_family == AF_INET && B->Addr.si_family == AF_INET &&
            A->Addr.Ipv4.sin_port == B->Addr.Ipv4.sin_port &&
            A->Addr.Ipv4.sin_addr.s_addr == B->Addr.Ipv4.sin_addr.s_addr &&
            A->Src4.ipi_addr.s_addr == B->Src4.ipi_addr.s_addr && A->Src4.ipi_ifindex == B->Src4.ipi_ifindex) ||
           (A->Addr.si_family == AF_INET6 && B->Addr.si_family == AF_INET6 &&
            A->Addr.Ipv6.sin6_port == B->Addr.Ipv6.sin6_port &&
            Ipv6AddrEq(&A->Addr.Ipv6.sin6_addr, &B->Addr.Ipv6.sin6_addr) &&
            A->Addr.Ipv6.sin6_scope_id == B->Addr.Ipv6.sin6_scope_id &&
            Ipv6AddrEq(&A->Src6.ipi6_addr, &B->Src6.ipi6_addr) && A->Src6.ipi6_ifindex == B->Src6.ipi6_ifindex) ||
           !A->Addr.si_family && !B->Addr.si_family;
}

_Use_decl_annotations_
VOID
SocketSetPeerEndpoint(WG_PEER *Peer, CONST ENDPOINT *Endpoint)
{
    KIRQL Irql;

    /* First we check unlocked, in order to optimize, since it's pretty rare
     * that an endpoint will change. If we happen to be mid-write, and two
     * CPUs wind up writing the same thing or something slightly different,
     * it doesn't really matter much either.
     */
    if (EndpointEq(Endpoint, &Peer->Endpoint))
        return;
    Irql = ExAcquireSpinLockExclusive(&Peer->EndpointLock);
    if (Endpoint->Addr.si_family == AF_INET)
    {
        Peer->Endpoint.Addr.Ipv4 = Endpoint->Addr.Ipv4;
        Peer->Endpoint.Src4 = Endpoint->Src4;
        Peer->Endpoint.SrcCmsghdr.cmsg_len = WSA_CMSG_LEN(sizeof(Endpoint->Src4));
        Peer->Endpoint.SrcCmsghdr.cmsg_level = IPPROTO_IP;
        Peer->Endpoint.SrcCmsghdr.cmsg_type = IP_PKTINFO;
    }
    else if (Endpoint->Addr.si_family == AF_INET6)
    {
        Peer->Endpoint.Addr.Ipv6 = Endpoint->Addr.Ipv6;
        Peer->Endpoint.Src6 = Endpoint->Src6;
        Peer->Endpoint.SrcCmsghdr.cmsg_len = WSA_CMSG_LEN(sizeof(Endpoint->Src6));
        Peer->Endpoint.SrcCmsghdr.cmsg_level = IPPROTO_IPV6;
        Peer->Endpoint.SrcCmsghdr.cmsg_type = IPV6_PKTINFO;
    }
    else
    {
        goto out;
    }
    Peer->Endpoint.RoutingGeneration = Endpoint->RoutingGeneration;
    ++Peer->Endpoint.UpdateGeneration;
out:
    ExReleaseSpinLockExclusive(&Peer->EndpointLock, Irql);
}

_Use_decl_annotations_
VOID
SocketSetPeerEndpointFromNbl(WG_PEER *Peer, CONST NET_BUFFER_LIST *Nbl)
{
    ENDPOINT Endpoint;

    if (NT_SUCCESS(SocketEndpointFromNbl(&Endpoint, Nbl)))
        SocketSetPeerEndpoint(Peer, &Endpoint);
}

_Use_decl_annotations_
VOID
SocketClearPeerEndpointSrc(WG_PEER *Peer)
{
    KIRQL Irql;

    Irql = ExAcquireSpinLockExclusive(&Peer->EndpointLock);
    Peer->Endpoint.RoutingGeneration = 0;
    ++Peer->Endpoint.UpdateGeneration;
    RtlZeroMemory(&Peer->Endpoint.Src6, sizeof(Peer->Endpoint.Src6));
    ExReleaseSpinLockExclusive(&Peer->EndpointLock, Irql);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Must_inspect_result_
static NTSTATUS WSKAPI
Receive(_In_opt_ PVOID SocketContext, _In_ ULONG Flags, _In_opt_ WSK_DATAGRAM_INDICATION *DataIndication)
{
    SOCKET *Socket = SocketContext;
    if (!Socket || !Socket->Sock || !DataIndication)
        return STATUS_SUCCESS;
    WG_DEVICE *Wg = Socket->Device;
    NET_BUFFER_LIST *First = NULL, **Link = &First;
    for (WSK_DATAGRAM_INDICATION *DataIndicationNext; DataIndication; DataIndication = DataIndicationNext)
    {
        DataIndicationNext = DataIndication->Next;
        DataIndication->Next = NULL;
        NET_BUFFER_LIST *Nbl = NULL;
        ULONG Length;
        if (!NT_SUCCESS(RtlSIZETToULong(DataIndication->Buffer.Length, &Length)))
            goto skipDatagramIndication;
        Nbl = MemAllocateNetBufferList(0, Length, 0);
        if (!Nbl || !ReadBooleanNoFence(&Wg->IsUp) || !ExAcquireRundownProtection(&Socket->ItemsInFlight))
            goto skipDatagramIndication;
        NET_BUFFER_LIST_DATAGRAM_INDICATION(Nbl) = DataIndication;
        DataIndication->Next = (VOID *)Socket;
        *Link = Nbl;
        Link = &NET_BUFFER_LIST_NEXT_NBL(Nbl);
        continue;

    skipDatagramIndication:
        ((WSK_PROVIDER_DATAGRAM_DISPATCH *)Socket->Sock->Dispatch)->WskRelease(Socket->Sock, DataIndication);
        if (Nbl)
            MemFreeNetBufferList(Nbl);
        ++Wg->Statistics.ifInDiscards;
    }
    if (First)
        PacketReceive(Wg, First);
    return STATUS_PENDING;
}

static IO_COMPLETION_ROUTINE RaiseEventOnComplete;
_Use_decl_annotations_
static NTSTATUS
RaiseEventOnComplete(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
    _Analysis_assume_(Context);
    KeSetEvent((KEVENT *)Context, IO_NETWORK_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

_IRQL_requires_max_(APC_LEVEL)
static VOID
CloseSocket(_Frees_ptr_opt_ SOCKET *Socket)
{
    if (!Socket)
        return;
    ExWaitForRundownProtectionRelease(&Socket->ItemsInFlight);
    if (Socket->Sock)
    {
        KEVENT Done;
        KeInitializeEvent(&Done, SynchronizationEvent, FALSE);
        IRP *Irp = IoAllocateIrp(1, FALSE);
        if (!Irp)
            goto freeIt;
        IoSetCompletionRoutine(Irp, RaiseEventOnComplete, &Done, TRUE, TRUE, TRUE);
        NTSTATUS Status = ((WSK_PROVIDER_DATAGRAM_DISPATCH *)Socket->Sock->Dispatch)->WskCloseSocket(Socket->Sock, Irp);
        if (Status == STATUS_PENDING)
        {
            KeWaitForSingleObject(&Done, Executive, KernelMode, FALSE, NULL);
            Status = Irp->IoStatus.Status;
        }
        IoFreeIrp(Irp);
    }
freeIt:
    MemFree(Socket);
}

_IRQL_requires_max_(APC_LEVEL)
static NTSTATUS
SetSockOpt(
    _In_ WSK_SOCKET *Sock,
    _In_ ULONG Level,
    _In_ ULONG Option,
    _In_reads_bytes_(Len) VOID *Input,
    _In_ ULONG Len)
{
    KEVENT Done;
    KeInitializeEvent(&Done, SynchronizationEvent, FALSE);
    IRP *Irp = IoAllocateIrp(1, FALSE);
    if (!Irp)
        return STATUS_INSUFFICIENT_RESOURCES;
    IoSetCompletionRoutine(Irp, RaiseEventOnComplete, &Done, TRUE, TRUE, TRUE);
    NTSTATUS Status = ((WSK_PROVIDER_DATAGRAM_DISPATCH *)Sock->Dispatch)
                          ->WskControlSocket(Sock, WskSetOption, Option, Level, Len, Input, 0, NULL, NULL, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Done, Executive, KernelMode, FALSE, NULL);
        Status = Irp->IoStatus.Status;
    }
    IoFreeIrp(Irp);
    return Status;
}

_IRQL_requires_max_(APC_LEVEL)
static NTSTATUS
CreateAndBindSocket(_In_ WG_DEVICE *Wg, _Inout_ SOCKADDR *Sa, _Out_ SOCKET **RetSocket)
{
    NTSTATUS Status = STATUS_INSUFFICIENT_RESOURCES;
    SOCKET *Socket = MemAllocate(sizeof(*Socket));
    if (!Socket)
        return Status;
    Socket->Device = Wg;
    Socket->Sock = NULL;
    ExInitializeRundownProtection(&Socket->ItemsInFlight);
    IRP *Irp = IoAllocateIrp(1, FALSE);
    if (!Irp)
        goto cleanupSocket;
    KEVENT Done;
    KeInitializeEvent(&Done, SynchronizationEvent, FALSE);
    IoSetCompletionRoutine(Irp, RaiseEventOnComplete, &Done, TRUE, TRUE, TRUE);
    static CONST WSK_CLIENT_DATAGRAM_DISPATCH WskClientDatagramDispatch = { .WskReceiveFromEvent = Receive };
    Status = WskProviderNpi.Dispatch->WskSocket(
        WskProviderNpi.Client,
        Sa->sa_family,
        SOCK_DGRAM,
        IPPROTO_UDP,
        WSK_FLAG_DATAGRAM_SOCKET,
        Socket,
        &WskClientDatagramDispatch,
        Wg->SocketOwnerProcess,
        NULL,
        NULL,
        Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Done, Executive, KernelMode, FALSE, NULL);
        Status = Irp->IoStatus.Status;
    }
    if (!NT_SUCCESS(Status))
        goto cleanupIrp;
    WSK_SOCKET *Sock = (WSK_SOCKET *)Irp->IoStatus.Information;
    WritePointerNoFence(&Socket->Sock, Sock);
    ULONG True = TRUE;

    if (Sa->sa_family == AF_INET)
    {
        Status = SetSockOpt(Sock, IPPROTO_UDP, UDP_NOCHECKSUM, &True, sizeof(True));
        if (!NT_SUCCESS(Status))
            goto cleanupIrp;
    }
    else if (Sa->sa_family == AF_INET6)
    {
        Status = SetSockOpt(Sock, IPPROTO_IPV6, IPV6_V6ONLY, &True, sizeof(True));
        if (!NT_SUCCESS(Status))
            goto cleanupIrp;
    }

    Status = SetSockOpt(
        Sock,
        Sa->sa_family == AF_INET6 ? IPPROTO_IPV6 : IPPROTO_IP,
        Sa->sa_family == AF_INET6 ? IPV6_PKTINFO : IP_PKTINFO,
        &True,
        sizeof(True));
    if (!NT_SUCCESS(Status))
        goto cleanupIrp;

    IoReuseIrp(Irp, STATUS_UNSUCCESSFUL);
    IoSetCompletionRoutine(Irp, RaiseEventOnComplete, &Done, TRUE, TRUE, TRUE);
    Status = ((WSK_PROVIDER_DATAGRAM_DISPATCH *)Sock->Dispatch)->WskBind(Sock, Sa, 0, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Done, Executive, KernelMode, FALSE, NULL);
        Status = Irp->IoStatus.Status;
    }
    if (!NT_SUCCESS(Status))
    {
        CHAR Address[SOCKADDR_STR_MAX_LEN];
        SockaddrToString(Address, (SOCKADDR_INET *)Sa);
        LogErr(Wg, "Could not bind socket to %s (%#x)", Address, Status);
        goto cleanupIrp;
    }

    IoReuseIrp(Irp, STATUS_UNSUCCESSFUL);
    IoSetCompletionRoutine(Irp, RaiseEventOnComplete, &Done, TRUE, TRUE, TRUE);
    Status = ((WSK_PROVIDER_DATAGRAM_DISPATCH *)Sock->Dispatch)->WskGetLocalAddress(Sock, Sa, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Done, Executive, KernelMode, FALSE, NULL);
        Status = Irp->IoStatus.Status;
    }
    if (!NT_SUCCESS(Status))
        goto cleanupIrp;

    IoFreeIrp(Irp);
    *RetSocket = Socket;
    return STATUS_SUCCESS;

cleanupIrp:
    IoFreeIrp(Irp);
cleanupSocket:
    CloseSocket(Socket);
    return Status;
}

_Use_decl_annotations_
NTSTATUS
SocketInit(WG_DEVICE *Wg, UINT16 Port)
{
    NTSTATUS Status;
    SOCKADDR_IN Sa4 = { .sin_family = AF_INET, .sin_addr.s_addr = Htonl(INADDR_ANY), .sin_port = Htons(Port) };
    SOCKADDR_IN6 Sa6 = { .sin6_family = AF_INET6, .sin6_addr = IN6ADDR_ANY_INIT };
    SOCKET *New4 = NULL, *New6 = NULL;
    LONG Retries = 0;

retry:
    if (WskHasIpv4Transport)
    {
        Status = CreateAndBindSocket(Wg, (SOCKADDR *)&Sa4, &New4);
        if (!NT_SUCCESS(Status))
            goto out;
    }

    if (WskHasIpv6Transport)
    {
        Sa6.sin6_port = Sa4.sin_port;
        Status = CreateAndBindSocket(Wg, (SOCKADDR *)&Sa6, &New6);
        if (!NT_SUCCESS(Status))
        {
            CloseSocket(New4);
            New4 = NULL;
            if (Status == STATUS_ADDRESS_ALREADY_EXISTS && !Port && Retries++ < 100)
                goto retry;
            goto out;
        }
    }

    SocketReinit(
        Wg,
        New4,
        New6,
        WskHasIpv4Transport   ? Ntohs(Sa4.sin_port)
        : WskHasIpv6Transport ? Ntohs(Sa6.sin6_port)
                              : Port);
    Status = STATUS_SUCCESS;
out:
    return Status;
}

_Use_decl_annotations_
VOID
SocketReinit(WG_DEVICE *Wg, SOCKET *New4, SOCKET *New6, UINT16 Port)
{
    MuAcquirePushLockExclusive(&Wg->SocketUpdateLock);
    SOCKET *Old4 = RcuDereferenceProtected(SOCKET, Wg->Sock4, &Wg->SocketUpdateLock);
    SOCKET *Old6 = RcuDereferenceProtected(SOCKET, Wg->Sock6, &Wg->SocketUpdateLock);
    RcuAssignPointer(Wg->Sock4, New4);
    RcuAssignPointer(Wg->Sock6, New6);
    if (New4 || New6)
        Wg->IncomingPort = Port;
    MuReleasePushLockExclusive(&Wg->SocketUpdateLock);
    RcuSynchronize();
    CloseSocket(Old4);
    CloseSocket(Old6);
}

static VOID
RouteNotification(
    _In_ VOID *CallerContext,
    _In_opt_ MIB_IPFORWARD_ROW2 *Row,
    _In_ MIB_NOTIFICATION_TYPE NotificationType)
{
    InterlockedAdd((LONG *)CallerContext, 2);
}

_Use_decl_annotations_
NTSTATUS
WskInit(VOID)
{
    NTSTATUS Status = ReadNoFence(&WskInitStatus);
    if (Status != STATUS_RETRY)
        return Status;
    MuAcquirePushLockExclusive(&WskIsIniting);
    Status = ReadNoFence(&WskInitStatus);
    if (Status != STATUS_RETRY)
        goto cleanupIniting;

#if NTDDI_VERSION == NTDDI_WIN7
    RTL_OSVERSIONINFOW OsVersionInfo = { .dwOSVersionInfoSize = sizeof(OsVersionInfo) };
    NoWskSendMessages =
        NT_SUCCESS(RtlGetVersion(&OsVersionInfo)) &&
        (OsVersionInfo.dwMajorVersion < 6 || (OsVersionInfo.dwMajorVersion == 6 && OsVersionInfo.dwMinorVersion < 2));
#endif

    Status = ExInitializeLookasideListEx(
        &SocketSendCtxCache, NULL, NULL, NonPagedPool, 0, sizeof(SOCKET_SEND_CTX), MEMORY_TAG, 0);
    if (!NT_SUCCESS(Status))
        goto cleanupIniting;
    WSK_CLIENT_NPI WskClientNpi = { .Dispatch = &WskAppDispatchV1 };
    Status = WskRegister(&WskClientNpi, &WskRegistration);
    if (!NT_SUCCESS(Status))
        goto cleanupLookaside;
    Status = WskCaptureProviderNPI(&WskRegistration, WSK_INFINITE_WAIT, &WskProviderNpi);
    if (!NT_SUCCESS(Status))
        goto cleanupWskRegister;
    SIZE_T WskTransportsSize = 0x10 * sizeof(WSK_TRANSPORT);
    for (;;)
    {
        WSK_TRANSPORT *WskTransports = MemAllocate(WskTransportsSize);
        if (!WskTransports)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto cleanupWskProviderNPI;
        }
        Status = WskProviderNpi.Dispatch->WskControlClient(
            WskProviderNpi.Client,
            WSK_TRANSPORT_LIST_QUERY,
            0,
            NULL,
            WskTransportsSize,
            WskTransports,
            &WskTransportsSize,
            NULL);
        if (NT_SUCCESS(Status))
        {
            for (SIZE_T i = 0, n = WskTransportsSize / sizeof(*WskTransports); i < n; ++i)
            {
                if (WskTransports[i].SocketType == SOCK_DGRAM && WskTransports[i].Protocol == IPPROTO_UDP)
                {
                    if (WskTransports[i].AddressFamily == AF_UNSPEC)
                    {
                        WskHasIpv4Transport = TRUE;
                        WskHasIpv6Transport = TRUE;
                    }
                    else if (WskTransports[i].AddressFamily == AF_INET)
                        WskHasIpv4Transport = TRUE;
                    else if (WskTransports[i].AddressFamily == AF_INET6)
                        WskHasIpv6Transport = TRUE;
                }
            }
            MemFree(WskTransports);
            break;
        }
        MemFree(WskTransports);
        if (Status != STATUS_BUFFER_OVERFLOW)
            goto cleanupWskProviderNPI;
    }
    WSK_EVENT_CALLBACK_CONTROL WskEventCallbackControl = { .NpiId = &NPI_WSK_INTERFACE_ID,
                                                           .EventMask = WSK_EVENT_RECEIVE_FROM };
    Status = WskProviderNpi.Dispatch->WskControlClient(
        WskProviderNpi.Client,
        WSK_SET_STATIC_EVENT_CALLBACKS,
        sizeof(WskEventCallbackControl),
        &WskEventCallbackControl,
        0,
        NULL,
        NULL,
        NULL);
    if (!NT_SUCCESS(Status))
        goto cleanupWskProviderNPI;

    Status = NotifyRouteChange2(AF_INET, RouteNotification, &RoutingGenerationV4, FALSE, &RouteNotifierV4);
    if (!NT_SUCCESS(Status))
        goto cleanupWskProviderNPI;
    Status = NotifyRouteChange2(AF_INET6, RouteNotification, &RoutingGenerationV6, FALSE, &RouteNotifierV6);
    if (!NT_SUCCESS(Status))
        goto cleanupRouteNotifierV4;

    WriteNoFence(&WskInitStatus, STATUS_SUCCESS);
    MuReleasePushLockExclusive(&WskIsIniting);
    return STATUS_SUCCESS;

cleanupRouteNotifierV4:
    CancelMibChangeNotify2(RouteNotifierV4);
cleanupWskProviderNPI:
    WskReleaseProviderNPI(&WskRegistration);
cleanupWskRegister:
    WskDeregister(&WskRegistration);
cleanupLookaside:
    ExDeleteLookasideListEx(&SocketSendCtxCache);
cleanupIniting:
    WriteNoFence(&WskInitStatus, Status);
    MuReleasePushLockExclusive(&WskIsIniting);
    return Status;
}

_Use_decl_annotations_
VOID WskUnload(VOID)
{
    MuAcquirePushLockExclusive(&WskIsIniting);
    if (ReadNoFence(&WskInitStatus) != STATUS_SUCCESS)
        goto out;
    CancelMibChangeNotify2(RouteNotifierV6);
    CancelMibChangeNotify2(RouteNotifierV4);
    WskReleaseProviderNPI(&WskRegistration);
    WskDeregister(&WskRegistration);
    ExDeleteLookasideListEx(&SocketSendCtxCache);
out:
    MuReleasePushLockExclusive(&WskIsIniting);
}
