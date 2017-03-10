#include "stdafx.h"
#include <cstdint>

static RIO_EXTENSION_FUNCTION_TABLE gRio;
static SOCKET gSocket;
static uint8_t gData[1024*1024];
static RIO_BUFFERID gBufferId;
static HANDLE gIocp;
static LARGE_INTEGER gPerfFreq;
static double gPerfFreqUs;
static uint64_t gWorstCaseLatency;
static RIO_RQ gRq;

static RIO_CQ gSendCq;
static RIO_NOTIFICATION_COMPLETION gSendNotify;
static OVERLAPPED gSendOverlapped;

static RIO_CQ gRecvCq;
static RIO_NOTIFICATION_COMPLETION gRecvNotify;
static OVERLAPPED gRecvOverlapped;

static int gIterations = 100000;
static int gIterationsLeft = gIterations;

static void clientLoop(const char *addr);
static void serverLoop();

#ifdef DEBUG_LOG
#define dprintf printf
#else
#define dprintf(...)
#endif

int main(int argc, const char **argv)
{
    WSADATA wsaData;
    bool isClient = argc > 1;

    setvbuf(stdout, 0, _IONBF, 0);
    setvbuf(stderr, 0, _IONBF, 0);

    if (!SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS)) {
        fprintf(stderr, "Unable to set realtime priority class.\r\n");
        return 1;
    }

    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL)) {
        fprintf(stderr, "Unable to set realtime thread priority.\r\n");
        return 1;
    }

    if (!QueryPerformanceFrequency(&gPerfFreq)) {
        fprintf(stderr, "Failed to query performance counter frequency.\r\n");
        return 1;
    }

    gPerfFreqUs = (double)gPerfFreq.QuadPart / 1000000.0;

    if (WSAStartup(MAKEWORD(2,2), &wsaData)) {
        fprintf(stderr, "Unable to initialize winsock.\r\n");
        return 1;
    }

    gSocket = WSASocket(
        AF_INET, SOCK_DGRAM, IPPROTO_UDP, 0, 0, WSA_FLAG_REGISTERED_IO);

    if (gSocket == INVALID_SOCKET) {
        fprintf(stderr, "Unable to create socket.\r\n");
        return 1;
    }

    struct sockaddr_in saddr;

    saddr.sin_addr.s_addr = INADDR_ANY;
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(isClient ? 0 : 10000);

    if (bind(gSocket, (struct sockaddr *)&saddr, sizeof(sockaddr_in)) < 0) {
        fprintf(stderr, "Unable to bind socket.\r\n");
        return 1;
    }

    GUID rioGuid = WSAID_MULTIPLE_RIO;
    DWORD rioSz;

    if (WSAIoctl(gSocket, SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER,
        &rioGuid, sizeof(GUID), &gRio, sizeof(gRio),
        &rioSz, 0, 0) < 0) {

        fprintf(stderr, "Unable to get RIO function pointer table.\r\n");
        return 1;
    }

    gBufferId = gRio.RIORegisterBuffer((PCHAR)gData, sizeof(gData));

    if (gBufferId == RIO_INVALID_BUFFERID) {
        fprintf(stderr, "Unable to register RIO buffer.\r\n");
        return 1;
    }

    gIocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, 0);
    gSendNotify.Type = RIO_IOCP_COMPLETION;
    gSendNotify.Iocp.IocpHandle = gIocp;
    gSendNotify.Iocp.Overlapped = &gSendOverlapped;
    gSendCq = gRio.RIOCreateCompletionQueue(1, &gSendNotify);
    gRecvNotify.Type = RIO_IOCP_COMPLETION;
    gRecvNotify.Iocp.IocpHandle = gIocp;
    gRecvNotify.Iocp.Overlapped = &gRecvOverlapped;
    gRecvCq = gRio.RIOCreateCompletionQueue(1, &gRecvNotify);

    if (gSendCq == RIO_INVALID_CQ || gRecvCq == RIO_INVALID_CQ) {
        fprintf(stderr, "Unable to create RIO completion queues.\r\n");
        return 1;
    }

    gRq = gRio.RIOCreateRequestQueue(
        gSocket, 1, 1, 1, 1, gRecvCq, gSendCq, 0);

    if (gRq == RIO_INVALID_RQ) {
        fprintf(stderr, "Unable to create RIO request queue.\r\n");
        return 1;
    }

    if (isClient) {
        LARGE_INTEGER start, end;
        printf("Starting ping client.\r\n");
        QueryPerformanceCounter(&start);
        clientLoop(argv[1]);
        QueryPerformanceCounter(&end);

        printf("Average latency: %lldus\r\n", (uint64_t)(
            (end.QuadPart - start.QuadPart) / gPerfFreqUs / gIterations));
        printf("Worst case latency: %lldus\r\n", gWorstCaseLatency);
    } else {
        printf("Starting ping server.\r\n");
        serverLoop();
    }

    return 0;
}

static bool dumpCompletions(RIO_CQ cq)
{
    RIORESULT results[8];
    ULONG status;
    int total = 0;

    while (true) {
        status = gRio.RIODequeueCompletion(cq, results, 8);

        if (status == 0 || status == RIO_CORRUPT_CQ) {
            break;
        }

        total += status;
    }

    return total > 0;
}

static void clientLoop(const char *addr)
{
    struct sockaddr_in target;
    uint64_t index = 0;
    RIO_BUF sendData = { gBufferId, 0, sizeof(uint64_t) };
    RIO_BUF sendAddress = {
        gBufferId, 2 * sizeof(uint64_t), sizeof(SOCKADDR_INET)
    };
    RIO_BUF recvData = { gBufferId, sizeof(uint64_t), sizeof(uint64_t) };
    LARGE_INTEGER startQpc, endQpc;

    target.sin_family = AF_INET;
    target.sin_port = htons(10000);

    if (!inet_pton(AF_INET, addr, &target.sin_addr)) {
        fprintf(stderr, "Error in inet_pton.\r\n");
        return;
    }

    memcpy(gData + sendAddress.Offset, &target, sizeof(struct sockaddr_in));

    while (gIterationsLeft--) {
        bool sendComplete = false, recvComplete = false;

        QueryPerformanceCounter(&startQpc);
        ++index;
        memcpy(gData + sendData.Offset, &index, sizeof(index));

        if (!gRio.RIOReceiveEx(gRq, &recvData, 1, 0, 0, 0, 0, 0, 0)) {
            fprintf(stderr, "Error in RIOReceiveEx.\r\n");
            return;
        }

        dprintf("started recv %08llX\r\n", index);

        if (!gRio.RIOSendEx(gRq, &sendData, 1, 0, &sendAddress, 0, 0, 0, 0)) {
            fprintf(stderr, "Error in RIOSendEx.\r\n");
            return;
        }

        dprintf("started send %08llX\r\n", index);

        while (!sendComplete || !recvComplete) {
            bool didSend = false, didRecv = false;

            didSend = !sendComplete && dumpCompletions(gSendCq);
            didRecv = !recvComplete && dumpCompletions(gRecvCq);

            if (didSend) {
                dprintf("send complete\r\n");
                sendComplete = true;
            }

            if (didRecv) {
                dprintf("recv complete\r\n");
                recvComplete = true;
                QueryPerformanceCounter(&endQpc);
                uint64_t latency = (uint64_t)(
                    (endQpc.QuadPart - startQpc.QuadPart) / gPerfFreqUs);

                if (latency > gWorstCaseLatency && index > 1) {
                    printf("worst case %08llX: %lldus\r\n", index, latency);
                    gWorstCaseLatency = latency;
                }
            }
        }

        uint64_t rIndex;

        memcpy(&rIndex, gData + recvData.Offset, sizeof(rIndex));

        if (index != rIndex) {
            fprintf(stderr, "Returned index doesn't match.\r\n");
        }
    }
}

static void serverLoop()
{
    RIO_BUF recvData = { gBufferId, 0, sizeof(uint64_t) };
    RIO_BUF sendData = { gBufferId, sizeof(uint64_t), sizeof(uint64_t) };
    RIO_BUF recvAddress = {
        gBufferId,
        2 * sizeof(uint64_t),
        sizeof(SOCKADDR_INET)
    };
    RIO_BUF sendAddress = {
        gBufferId,
        2 * sizeof(uint64_t) + sizeof(SOCKADDR_INET),
        sizeof(SOCKADDR_INET)
    };
    bool firstIteration = true;

    while (gIterationsLeft--) {
        bool sendComplete = false, recvComplete = false;

        if (firstIteration) {
            firstIteration = false;

            if (!gRio.RIOReceiveEx(
                gRq, &recvData, 1, 0, &recvAddress, 0, 0, 0, 0)) {

                fprintf(stderr, "Error in RIOReceiveEx.\r\n");
                return;
            }

            dprintf("started recv\r\n");
        }

        while (!sendComplete || !recvComplete) {
            bool didSend = false, didRecv = false;

            didSend = !sendComplete && dumpCompletions(gSendCq);
            didRecv = !recvComplete && dumpCompletions(gRecvCq);

            if (didSend) {
                dprintf("send complete\r\n");
                sendComplete = true;
            }

            if (didRecv) {
#ifdef DEBUG_LOG
                char nodeBuf[256], svcBuf[256];
#endif
                uint64_t index;

                memcpy(&index, gData + recvData.Offset, sizeof(uint64_t));
                dprintf("recv %08llX complete\r\n", index);
                recvComplete = true;

                memcpy(
                    gData + sendData.Offset,
                    gData + recvData.Offset,
                    sendData.Length);
                memcpy(
                    gData + sendAddress.Offset,
                    gData + recvAddress.Offset,
                    sendAddress.Length);

#ifdef DEBUG_LOG
                getnameinfo(
                    (SOCKADDR *)(gData + sendAddress.Offset),
                    sizeof(SOCKADDR_INET),
                    nodeBuf, sizeof(nodeBuf), svcBuf, sizeof(svcBuf), 0);
#endif

                // Re-arm receive before sending response.
                if (!gRio.RIOReceiveEx(
                    gRq, &recvData, 1, 0, &recvAddress, 0, 0, 0, 0)) {

                    fprintf(stderr, "Error in RIOReceiveEx.\r\n");
                    return;
                }

                dprintf("rearmed recv\r\n");
                dprintf("sending pong to %s:%s\r\n", nodeBuf, svcBuf);

                if (!gRio.RIOSendEx(
                    gRq, &sendData, 1, 0, &sendAddress, 0, 0, 0, 0)) {

                    fprintf(stderr, "Error in RIOSendEx.\r\n");
                    return;
                }

                dprintf("started send\r\n");
            }
        }
    }
}

