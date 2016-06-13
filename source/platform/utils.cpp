//---------------------------------------------------------------------------------------------------------------------
// Copyright (c) 2015-2016 librapid project. All rights reserved.
// More license information, please see LICENSE file in module root folder.
//---------------------------------------------------------------------------------------------------------------------

#include <cstdint>
#include <vector>
#include <fstream>
#include <bitset>

#include <rapid/platform/platform.h>

#include <tlhelp32.h>
#include <Objbase.h>
#include <Winsock2.h>
#include <Psapi.h>

#include <rapid/exception.h>

#include <rapid/details/contracts.h>
#include <rapid/details/common.h>
#include <rapid/platform/utils.h>

namespace rapid {

namespace platform {

struct SocketInitializer {
    SocketInitializer() noexcept {
		auto retval = ::WSAStartup(MAKEWORD(2, 2), &wsadata_);
        if (retval != ERROR_SUCCESS) {
			lastError_ = ::WSAGetLastError();
        } else {
			lastError_ = ERROR_SUCCESS;
        }
    }

    ~SocketInitializer() noexcept {
		if (lastError_ == ERROR_SUCCESS) {
            ::WSACleanup();
        }
    }

    WSADATA	wsadata_;
    uint32_t lastError_;
};

class SystemInfo::SystemInfoImpl {
public:
    SystemInfoImpl();

    ~SystemInfoImpl() noexcept;

    uint32_t getNumberOfProcessors(uint32_t perCPU) const noexcept;

    uint32_t getPageSize() const noexcept;

    size_t getPageBoundarySize() const noexcept;

	uint32_t roundUpToPageSize(uint32_t size) const noexcept;

    bool isNumaSystem();

    uint64_t getNumaNodeProcessorMask(uint8_t node);

    std::vector<NumaProcessor> getNumaProcessorInformation();

    ProcessorInformation getProcessorInformation();

private:
    bool isNumaSystem_;
    SYSTEM_INFO info_;
};

SystemInfo::SystemInfoImpl::SystemInfoImpl()
    : isNumaSystem_(false) {
    ::GetSystemInfo(&info_);
}

SystemInfo::SystemInfoImpl::~SystemInfoImpl() noexcept {
}

uint32_t SystemInfo::SystemInfoImpl::getNumberOfProcessors(uint32_t perCPU) const noexcept {
    return perCPU * info_.dwNumberOfProcessors;
}

uint32_t SystemInfo::SystemInfoImpl::getPageSize() const noexcept {
    return info_.dwPageSize;
}

size_t SystemInfo::SystemInfoImpl::getPageBoundarySize() const noexcept {
    return info_.dwAllocationGranularity;
}

uint32_t SystemInfo::SystemInfoImpl::roundUpToPageSize(uint32_t size) const noexcept {
    return details::roundUp(size, getPageSize());
}

static inline uint32_t countSetBits(ULONG_PTR bitMask) {
    uint32_t LSHIFT = sizeof(ULONG_PTR) * 8 - 1;
    uint32_t bitSetCount = 0;
    auto bitTest = static_cast<ULONG_PTR>(1) << LSHIFT;
    uint32_t i;

    for (i = 0; i <= LSHIFT; ++i) {
        bitSetCount += ((bitMask & bitTest) ? 1 : 0);
        bitTest /= 2;
    }
    return bitSetCount;
}

std::ostream& operator << (std::ostream& ostr, ProcessorInformation const &info) {
    ostr << "Number of NUMA nodes: " << info.numaNodeCount << "\r\n"
        << "Number of physical processor packages: " << info.processorPackageCount << "\r\n"
        << "Number of processor cores: " << info.processorCoreCount << "\r\n"
        << "Number of logical processors: " << info.logicalProcessorCount << "\r\n"
        << "L1/L2/L3 caches: "
        << info.processorL1CacheCount << " MB"
        << "/" << info.processorL2CacheCount << " MB"
        << "/" << info.processorL3CacheCount << " MB";
    return ostr;
}

std::ostream & operator<<(std::ostream & ostr, std::vector<NumaProcessor> const & numaProcessors) {
    for (auto processor : numaProcessors) {
        ostr << "processor: " << (int)processor.processor << "\r\n"
            << "node: " << (int)processor.node << "\r\n"
            << "processorMask: " << std::bitset<64>(processor.processorMask).to_string() << "\r\n";
    }
    return ostr;
}

ProcessorInformation SystemInfo::SystemInfoImpl::getProcessorInformation() {
    ProcessorInformation info;

    DWORD returnLength = 0;
    uint32_t byteOffset = 0;
    PCACHE_DESCRIPTOR cache = nullptr;
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION pLogicalProcInfo = nullptr;

    std::vector<uint8_t> buffer;

    if (!::GetLogicalProcessorInformation(nullptr, &returnLength)) {
        auto lastError = ::GetLastError();

        if (lastError == ERROR_INSUFFICIENT_BUFFER) {
            buffer.resize(returnLength);
            pLogicalProcInfo = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION>(buffer.data());
            ::GetLogicalProcessorInformation(pLogicalProcInfo, &returnLength);
        } else {
            throw Exception(lastError);
        }
    }

    while (byteOffset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) <= returnLength) {
        switch (pLogicalProcInfo->Relationship) {
        case RelationNumaNode:
        info.numaNodeCount++;
        break;
        case RelationProcessorCore:
        info.processorCoreCount++;
        // A hyper-threaded core supplies more than one logical processor.
        info.logicalProcessorCount += countSetBits(pLogicalProcInfo->ProcessorMask);
        break;
        case RelationCache:
        cache = &pLogicalProcInfo->Cache;
        if (cache->Level == 1) {
            info.processorL1CacheCount++;
        } else if (cache->Level == 2) {
            info.processorL2CacheCount++;
        } else if (cache->Level == 3) {
            info.processorL3CacheCount++;
        }
        break;
        case RelationProcessorPackage:
        // Logical processors share a physical package.
        info.processorPackageCount++;
        break;
        case RelationGroup:
        break;
        case RelationAll:
        break;
        default:
        break;
        }

        byteOffset += sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
        pLogicalProcInfo++;
    }
    return info;
}

uint64_t SystemInfo::SystemInfoImpl::getNumaNodeProcessorMask(uint8_t node) {
    uint64_t processorMask = 0;
    if (!::GetNumaNodeProcessorMask(node, &processorMask)) {
        throw Exception();
    }
    return processorMask;
}

std::vector<NumaProcessor> SystemInfo::SystemInfoImpl::getNumaProcessorInformation() {
    std::vector<NumaProcessor> numaProcessorInfo;

    auto processorInfo = getProcessorInformation();
    for (uint32_t i = 0; i < processorInfo.logicalProcessorCount; ++i) {
        NumaProcessor numaProcessor;

        numaProcessor.processor = i;
        if (!::GetNumaProcessorNode(i, &numaProcessor.node)) {
            throw Exception();
        }

        if (!::GetNumaNodeProcessorMask(numaProcessor.node, &numaProcessor.processorMask)) {
            throw Exception();
        }
        numaProcessorInfo.push_back(numaProcessor);
    }
    return numaProcessorInfo;
}

bool SystemInfo::SystemInfoImpl::isNumaSystem() {
    ULONG highestNodeNumber = 0;
    if (!::GetNumaHighestNodeNumber(&highestNodeNumber)) {
        throw Exception();
    }
    return highestNodeNumber > 0;
}

SystemInfo::SystemInfo()
    : pImpl_(std::make_unique<SystemInfoImpl>()) {
}

SystemInfo::~SystemInfo() noexcept {
}

uint32_t SystemInfo::getPageSize() const noexcept {
    return pImpl_->getPageSize();
}

uint32_t SystemInfo::getPageBoundarySize() const noexcept {
    return pImpl_->getPageBoundarySize();
}

uint32_t SystemInfo::roundUpToPageSize(uint32_t size) const noexcept {
    return pImpl_->roundUpToPageSize(size);
}

uint32_t SystemInfo::getNumberOfProcessors(uint32_t perCPU) const noexcept {
    return pImpl_->getNumberOfProcessors(perCPU);
}

bool SystemInfo::isNumaSystem() {
    return pImpl_->isNumaSystem();
}

uint64_t SystemInfo::getNumaNodeProcessorMask(uint8_t node) {
    return pImpl_->getNumaNodeProcessorMask(node);
}

std::vector<NumaProcessor> SystemInfo::getNumaProcessorInformation() {
    return pImpl_->getNumaProcessorInformation();
}

ProcessorInformation SystemInfo::getProcessorInformation() {
    return pImpl_->getProcessorInformation();
}

bool startupWinSocket() {
    static const SocketInitializer socketIniter;
    return socketIniter.lastError_ == ERROR_SUCCESS;
}

std::wstring getApplicationFilePath() {
    wchar_t fileName[MAX_PATH] = { 0 };
    ::GetModuleFileNameW(nullptr, fileName, MAX_PATH - 1);
    return fileName;
}

std::wstring getApplicationFileName() {
	wchar_t drive[MAX_PATH];
	wchar_t dir[MAX_PATH];
	wchar_t filename[MAX_PATH];
	wchar_t ext[MAX_PATH];
	wchar_t exe_filename[MAX_PATH];

	auto filePath = getApplicationFilePath();
	::_wsplitpath_s(filePath.c_str(),
		drive,
		MAX_PATH,
		dir,
		MAX_PATH,
		filename,
		MAX_PATH,
		ext,
		MAX_PATH);
	::_wmakepath_s(exe_filename, MAX_PATH, nullptr, nullptr, filename, ext);
	return filename;
}

std::string getFinalPathNameByHandle(HANDLE fileHandle) {
    RAPID_ENSURE(fileHandle != INVALID_HANDLE_VALUE);
    char path[MAX_PATH] = { 0 };
    ::GetFinalPathNameByHandleA(fileHandle, path, MAX_PATH - 1, VOLUME_NAME_NT);
    return path;
}

std::vector<uint32_t> enumThreadIds() {
	auto const processId = ::GetCurrentProcessId();
	std::unique_ptr<void, decltype(&::CloseHandle)> snapshotHandle(
		::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0), &::CloseHandle);
	THREADENTRY32 threadEntry = { 0 };
	threadEntry.dwSize = sizeof(THREADENTRY32);
	if (!::Thread32First(snapshotHandle.get(), &threadEntry)) {
		throw Exception();
	}
	std::vector<uint32_t> threadIds;
	for (; ::Thread32Next(snapshotHandle.get(), &threadEntry);) {
		if (threadEntry.th32OwnerProcessID == processId) {
			threadIds.push_back(threadEntry.th32ThreadID);
		}
	}
	return threadIds;
}

void setProcessPriorityBoost(bool enableBoost) {
    if (!::SetProcessPriorityBoost(::GetCurrentProcess(), enableBoost)) {
        throw Exception();
    }
}

int64_t getFileSize(std::string const &filePath) {
	struct __stat64 fileStat;
	_stat64(filePath.c_str(), &fileStat);
	return fileStat.st_size;
}

void prefetchVirtualMemory(char const * virtualAddress, size_t size) {
	// TODO: Windows 2008 not support!
	WIN32_MEMORY_RANGE_ENTRY entry;
	entry.VirtualAddress = const_cast<void*>(reinterpret_cast<const void*>(virtualAddress));
	entry.NumberOfBytes = size;

	if (!::PrefetchVirtualMemory(::GetCurrentProcess(), 1, &entry, 0)) {
		throw rapid::Exception();
	}
}

}

}
