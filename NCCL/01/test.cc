#include "cuda_runtime.h"
#include "nccl.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mpich/mpi.h>
#include <mutex>
#include <signal.h>
#include <thread>
#include <unistd.h>

#define MPICHECK(cmd)                                \
    do {                                             \
	int e = cmd;                                 \
	if (e != MPI_SUCCESS) {                      \
	    printf("Failed: MPI error %s:%d '%d'\n", \
		__FILE__, __LINE__, e);              \
	    exit(EXIT_FAILURE);                      \
	}                                            \
    } while (0)

#define CUDACHECK(cmd)                                      \
    do {                                                    \
	cudaError_t e = cmd;                                \
	if (e != cudaSuccess) {                             \
	    printf("Failed: Cuda error %s:%d '%s'\n",       \
		__FILE__, __LINE__, cudaGetErrorString(e)); \
	    exit(EXIT_FAILURE);                             \
	}                                                   \
    } while (0)

#define NCCLCHECK(cmd)                                      \
    do {                                                    \
	ncclResult_t r = cmd;                               \
	if (r != ncclSuccess) {                             \
	    printf("Failed, NCCL error %s:%d '%s'\n",       \
		__FILE__, __LINE__, ncclGetErrorString(r)); \
	    exit(EXIT_FAILURE);                             \
	}                                                   \
    } while (0)

// RAII wrapper for NCCL communicator
class NCCLComm {
public:
    explicit NCCLComm(ncclComm_t nccl_comm)
	: nccl_comm_(nccl_comm){};

    ~NCCLComm()
    {
	//finalizing NCCL
	ncclCommDestroy(nccl_comm_);
    }

    ncclComm_t GetNCCLComm() const
    {
	std::lock_guard<std::mutex> lock(mutex_);
	return nccl_comm_;
    }

    void CheckNCCLError()
    {

	std::lock_guard<std::mutex> lock(mutex_);
	ncclResult_t result;
	NCCLCHECK(ncclCommGetAsyncError(nccl_comm_, &result));
	if (result != ncclSuccess) {
	    printf("ncclCommGetAsyncError result: %s\n", ncclGetErrorString(result));
	    printf("[DEBUG] ncclComAbort starts!\n");
	    auto start = std::chrono::steady_clock::now();
	    NCCLCHECK(ncclCommAbort(nccl_comm_));
	    auto end = std::chrono::steady_clock::now();
	    double time_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
	    printf("[DEBUG] ncclComAbort finishes! Time elapsed = %2.f ms.\n", time_elapsed);
	}
    }

private:
    ncclComm_t nccl_comm_;
    mutable std::mutex mutex_;
};

static uint64_t getHostHash(const char* string)
{
    // Based on DJB2a, result = result * 33 ^ char
    uint64_t result = 5381;
    for (int c = 0; string[c] != '\0'; c++) {
	result = ((result << 5) + result) ^ string[c];
    }
    return result;
}

static void getHostName(char* hostname, int maxlen)
{
    gethostname(hostname, maxlen);
    for (int i = 0; i < maxlen; i++) {
	if (hostname[i] == '.') {
	    hostname[i] = '\0';
	    return;
	}
    }
}

void checkNCCLError(NCCLComm& comm)
{
    while (true) {
	comm.CheckNCCLError();
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void handler(int signum)
{
    printf("receive signal %d. exit.", signum);
    // use 0 to keep MPI from aborting the job
    exit(0);
}

int main(int argc, char* argv[])
{
    int size = 32 * 1024 * 1024;

    int myRank, nRanks, localRank = 0;

    signal(SIGTERM, handler);

    //initializing MPI
    MPICHECK(MPI_Init(&argc, &argv));
    MPICHECK(MPI_Comm_rank(MPI_COMM_WORLD, &myRank));
    MPICHECK(MPI_Comm_size(MPI_COMM_WORLD, &nRanks));

    //calculating localRank based on hostname which is used in selecting a GPU
    uint64_t hostHashs[nRanks];
    char hostname[1024];
    getHostName(hostname, 1024);
    hostHashs[myRank] = getHostHash(hostname);
    MPICHECK(MPI_Allgather(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, hostHashs, sizeof(uint64_t), MPI_BYTE, MPI_COMM_WORLD));
    for (int p = 0; p < nRanks; p++) {
	if (p == myRank)
	    break;
	if (hostHashs[p] == hostHashs[myRank])
	    localRank++;
    }

    ncclUniqueId id;
    ncclComm_t comm;
    float *sendbuff, *recvbuff;
    cudaStream_t s;

    //get NCCL unique ID at rank 0 and broadcast it to all others
    if (myRank == 0)
	ncclGetUniqueId(&id);
    MPICHECK(MPI_Bcast((void*)&id, sizeof(id), MPI_BYTE, 0, MPI_COMM_WORLD));

    //finalizing MPI
    MPICHECK(MPI_Finalize());

    //picking a GPU based on localRank, allocate device buffers
    CUDACHECK(cudaSetDevice(localRank));
    CUDACHECK(cudaMalloc(&sendbuff, size * sizeof(float)));
    CUDACHECK(cudaMalloc(&recvbuff, size * sizeof(float)));
    CUDACHECK(cudaStreamCreate(&s));

    //initializing NCCL
    NCCLCHECK(ncclCommInitRank(&comm, nRanks, id, myRank));

    NCCLComm nccl_comm(comm);
    std::thread background_watchdog_thread(checkNCCLError, std::ref(nccl_comm));

    //communicating using NCCL
    while (true) {
	NCCLCHECK(ncclAllReduce((const void*)sendbuff, (void*)recvbuff, size, ncclFloat, ncclSum,
	    nccl_comm.GetNCCLComm(), s));
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    //free device buffers
    CUDACHECK(cudaFree(sendbuff));
    CUDACHECK(cudaFree(recvbuff));

    background_watchdog_thread.join();

    printf("[Rank %d] Success \n", myRank);
    return 0;
}