#include "debugging.hpp"

#include <util/formatting.hpp>
#include <util/misc.hpp>

#ifdef GEODE_IS_WINDOWS
# include <dbghelp.h>
# pragma comment(lib, "dbghelp.lib")
#endif
#include <any>

using namespace geode::prelude;

namespace util::debugging {
    time::micros Benchmarker::end(const std::string_view id) {
        auto idstr = std::string(id);
        auto micros = time::as<time::micros>(time::now() - _entries[idstr]);
        _entries.erase(idstr);

        return micros;
    }

    std::vector<size_t> DataWatcher::updateLastData(DataWatcher::WatcherEntry& entry) {
        std::vector<size_t> changedBytes;

        for (size_t off = 0; off < entry.size; off++) {
            auto rbyte = *(data::byte*)(entry.address + off);
            if (rbyte != entry.lastData[off]) {
                changedBytes.push_back(off);
                entry.lastData[off] = rbyte;
            }
        }

        return changedBytes;
    }

    void DataWatcher::updateAll() {
        for (auto& [key, value] : _entries) {
            auto modified = this->updateLastData(value);
            if (modified.empty()) continue;

            geode::log::debug("[DW] {} modified - {}, hexdump: {}", key, modified, hexDumpAddress(value.address, value.size));
        }
    }

    void PacketLogSummary::print() {
        geode::log::debug("====== Packet summary ======");
        if (total == 0) {
            geode::log::debug("No packets have been sent during this period.");
        } else {
            geode::log::debug("Total packets: {} ({} sent, {} received)", total, totalOut, totalIn);
            geode::log::debug("Encrypted packets: {} ({} cleartext, ratio: {}%)", totalEncrypted, totalCleartext, encryptedRatio * 100);
            geode::log::debug(
                "Total bytes transferred: {} ({} sent, {} received)",
                formatting::formatBytes(totalBytes),
                formatting::formatBytes(totalBytesOut),
                formatting::formatBytes(totalBytesIn)
            );
            geode::log::debug("Average bytes per packet: {}", formatting::formatBytes((uint64_t)bytesPerPacket));

            // sort packets by the counts
            std::vector<std::pair<packetid_t, size_t>> pc(packetCounts.begin(), packetCounts.end());
            std::sort(pc.begin(), pc.end(), [](const auto& a, const auto& b) {
                return a.second > b.second;
            });

            for (const auto& [id, count] : pc) {
                geode::log::debug("Packet {} - {} occurrences", id, count);
            }
        }
        geode::log::debug("==== Packet summary end ====");
    }

    PacketLogSummary PacketLogger::getSummary() {
        PacketLogSummary summary = {};

        for (auto& log : queue.extract()) {
            summary.total++;

            summary.totalBytes += log.bytes;
            if (log.outgoing) {
                summary.totalOut++;
                summary.totalBytesOut += log.bytes;
            } else {
                summary.totalIn++;
                summary.totalBytesIn += log.bytes;
            }

            log.encrypted ? summary.totalEncrypted++ : summary.totalCleartext++;

            if (!summary.packetCounts.contains(log.id)) {
                summary.packetCounts[log.id] = 0;
            }

            summary.packetCounts[log.id]++;
        }

        summary.bytesPerPacket = (float)summary.totalBytes / summary.total;
        summary.encryptedRatio = (float)summary.totalEncrypted / summary.total;

        return summary;
    }

    std::string hexDumpAddress(uintptr_t addr, size_t bytes) {
        unsigned char* ptr = reinterpret_cast<unsigned char*>(addr);

        std::stringstream ss;

        for (size_t i = 0; i < bytes; i++) {
            ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(i[ptr]);
        }

        return ss.str();
    }

    std::string hexDumpAddress(void* ptr, size_t bytes) {
        return hexDumpAddress(reinterpret_cast<uintptr_t>(ptr), bytes);
    }

#if GLOBED_CAN_USE_SOURCE_LOCATION
    std::string sourceLocation(const std::source_location loc) {
        return
            std::string(loc.file_name())
            + ":"
            + std::to_string(loc.line())
            + " ("
            + std::string(loc.function_name())
            + ")";
    }

    [[noreturn]] void suicide(const std::source_location loc) {
        geode::log::error("suicide called at {}, terminating.", sourceLocation(loc));
		geode::log::error("If you see this, something very, very bad happened.");
        GLOBED_SUICIDE
    }
#else
    std::string sourceLocation() {
        return "unknown file sorry";
    }

    [[noreturn]] void suicide() {
        geode::log::error("suicide called at <unknown location>, terminating.");
        geode::log::error("If you see this, something very, very bad happened.");
        GLOBED_SUICIDE
    }
#endif

    void timedLog(const std::string_view message) {
        geode::log::info("\r[{}] [Globed] {}", util::formatting::formatDateTime(util::time::now()), message);
    }

    void nop(ptrdiff_t offset, size_t bytes) {
#ifdef GEODE_IS_WINDOWS
        std::vector<uint8_t> bytevec;
        for (size_t i = 0; i < bytes; i++) {
            bytevec.push_back(0x90);
        }

        (void) geode::Mod::get()->patch(reinterpret_cast<void*>(geode::base::get() + offset), bytevec);
#else
        throw std::runtime_error("nop not implemented");
#endif
    }

#ifdef GEODE_IS_WINDOWS
    static int exceptionDummy() {
        return EXCEPTION_EXECUTE_HANDLER;
    }
#endif

    static uintptr_t adjustPointerForMaps(uintptr_t ptr) {
#ifdef GEODE_IS_ANDROID64
        // remove the tag (msb)
        const uint64_t mask = 0xFF00000000000000;
        return ptr & ~mask;
#else
        return ptr;
#endif
    }

    static uintptr_t isPointerTagged(uintptr_t ptr) {
#ifdef GEODE_IS_ANDROID64
        return (ptr >> 56) == 0xb4;
#else
        return true;
#endif
    }

    bool canReadPointer(uintptr_t address, size_t align = 1) {
        if (address < 0x1000) return false;
        if (address % align != 0) return false;

#ifdef GEODE_IS_ANDROID
        address = adjustPointerForMaps(address);

        static misc::OnceCell<std::unordered_map<size_t, ProcMapEntry>> _maps;
        auto& maps = _maps.getOrInit([] {
            std::unordered_map<size_t, ProcMapEntry> entries;

            std::ifstream maps("/proc/self/maps");

            std::string line;
            while (std::getline(maps, line, '\n')) {
                geode::log::debug("{}", line);
                size_t spacePos = line.find(' ');
                auto addressRange = line.substr(0, spacePos);

                size_t dashPos = addressRange.find('-');
                if (dashPos == std::string::npos || dashPos == 0) continue;

                std::string baseStr = addressRange.substr(0, dashPos);
                std::string endStr = addressRange.substr(dashPos + 1);

                uintptr_t base = std::strtoul(baseStr.c_str(), nullptr, 16);
                ptrdiff_t size = std::strtoul(endStr.c_str(), nullptr, 16) - base;

                auto worldReadable = line[spacePos + 1] == 'r';

                entries.emplace(base, ProcMapEntry {
                    .size = size,
                    .readable = worldReadable,
                });

            }

            return entries;
        });

        for (const auto& [base, entry] : maps) {
            if (!(address > base && address - base < entry.size)) continue;
            return entry.readable;
        }

        return false;
#elif defined(GEODE_IS_WINDOWS)
        bool isBad = IsBadReadPtr((void*)address, 4);
        if (isBad) return false;
        return true;

        // this one doesnt work
        __try {
            (void) *(volatile char*)(address);
            return true;
        } __except(exceptionDummy()) {
            return false;
        }
#endif
    }

#ifdef GEODE_IS_ANDROID
    struct Typeinfo {
        void* _unkptr;
        const char* namePtr;
    };
#elif defined(GEODE_IS_WINDOWS)
    struct TypeDescriptor {
        void *pVFTable, *spare;
        char name[];
    };

    struct Typeinfo {
        unsigned int _unk1, _unk2, _unk3;
        TypeDescriptor* descriptor;
    };
#endif

    geode::Result<std::string> getTypename(void* address) {
        if (!canReadPointer((uintptr_t)address, 4)) return Err("invalid address");

        void* vtablePtr = *(void**)(address);
        return getTypenameFromVtable(vtablePtr);
    }

    geode::Result<std::string> getTypenameFromVtable(void* address) {
        // TODO make this faster, dont use string <Unknown> and use Result
        if (!canReadPointer((uintptr_t)address, 4)) return Err("invalid vtable");

        Typeinfo** typeinfoPtrPtr = (Typeinfo**)((uintptr_t)address - sizeof(void*));
        if (!canReadPointer((uintptr_t)typeinfoPtrPtr, 4)) return Err("invalid typeinfo");
        Typeinfo* typeinfoPtr = *typeinfoPtrPtr;
        if (!canReadPointer((uintptr_t)typeinfoPtr, 4)) return Err("invalid typeinfo");
        Typeinfo typeinfo = *typeinfoPtr;

#ifdef GEODE_IS_ANDROID
        const char* namePtr = typeinfo.namePtr;
        // geode::log::debug("name ptr: {:X}", (uintptr_t)namePtr - geode::base::get());
        if (!canReadPointer((uintptr_t)namePtr, 4)) return Err("invalid class name");

        int status;
        char* demangledBuf = abi::__cxa_demangle(namePtr, nullptr, nullptr, &status);
        if (status != 0) {
            return Err("demangle failed");
        }

        std::string demangled(demangledBuf);
        free(demangledBuf);

        return Ok(demangled);
#elif defined(GEODE_IS_WINDOWS)

        if (!canReadPointer((uintptr_t)typeinfo.descriptor, 4)) return Err("invalid descriptor");
        const char* namePtr = typeinfo.descriptor->name;

        if (!canReadPointer((uintptr_t)namePtr)) return Err("invalid class name");

        // TODO windows: check if the name is proper

        if (!namePtr || namePtr[0] == '\0' || namePtr[1] == '\0') {
            return Err("failed to demangle");
        } else {
            char demangledBuf[256];
            size_t written = UnDecorateSymbolName(namePtr + 1, demangledBuf, 256, UNDNAME_NO_ARGUMENTS);
            if (written == 0) {
                return Err("failed to demangle;");
            } else {
                return Ok(std::string(demangledBuf, demangledBuf + written));
            }
        }
#endif
    }

    static bool likelyFloat(uint32_t bits) {
        float value = util::data::bit_cast<float>(bits);
        float absv = std::abs(value);

        return std::isfinite(value) && absv <= 100000.f && absv > 0.001f;
    }

    static bool likelyDouble(uint64_t bits) {
        double value = util::data::bit_cast<double>(bits);
        double absv = std::abs(value);

        return std::isfinite(value) && absv <= 1000000.0 && absv > 0.0001;
    }

    static bool likelySeedValue(uint32_t val1, uint32_t val2, uint32_t val3) {
        size_t invalids = 0;
        invalids += val1 == 0 | val1 == 0xffffffff;
        invalids += val2 == 0 | val2 == 0xffffffff;
        invalids += val3 == 0 | val3 == 0xffffffff;

        if (invalids > 1) return false;

        return val1 + val2 == val3 || val1 + val3 == val2 || val2 + val3 == val1;
    }

    static bool likelyString(uintptr_t address) {
        // cursed code
        const unsigned char* data = (unsigned char*)address;

        size_t asciiBytes = 0;
        while (*data != '\0') {
            if (*data <= 127) {
                asciiBytes++;
            } else break;

            data++;
        };

        return asciiBytes > 2 && data[asciiBytes] == '\0';
    }

    enum class ScanItemType {
        Float, Double, SeedValue, HeapPointer, String, EmptyString
    };

#ifdef GEODE_IS_ANDROID
#endif
    uintptr_t getEmptyString() {
        static misc::OnceCell<uintptr_t> _internalstr;
        return _internalstr.getOrInit([] {
            // thank you matcool from run info
            auto* cc = new CCString();
            uintptr_t address = (uintptr_t)*(const char**) &cc->m_sString;
            delete cc;
            return address;
        });
    }

    static std::unordered_map<ptrdiff_t, ScanItemType> scanMemory(void* address, size_t size) {
        // 4-byte pass
        std::unordered_map<ptrdiff_t, ScanItemType> out;
        for (ptrdiff_t node = 0; node < size; node += 4) {
            uint32_t nodeValue = *(uint32_t*)((uintptr_t)address + node);
            if (likelyFloat(nodeValue)) {
                out.emplace(node, ScanItemType::Float);
                continue;
            }

#if UINTPTR_MAX <= 0xffffffff
# ifdef GEODE_IS_ANDROID
            if (getEmptyString() == nodeValue) {
                out.emplace(node, ScanItemType::String);
                continue;
            } else if (canReadPointer((uintptr_t)nodeValue)) {
                if (likelyString((uintptr_t)nodeValue)) {
                    out.emplace(node, ScanItemType::String);
                } else {
                    out.emplace(node, ScanItemType::HeapPointer);
                }
                continue;
            }
# else // GEODE_IS_ANDROID
            if (canReadPointer((uintptr_t)nodeValue)) {
                if (likelyString((uintptr_t)nodeValue)) {
                    out.emplace(node, ScanItemType::String);
                } else {
                    out.emplace(node, ScanItemType::HeapPointer);
                }
                continue;
            }
# endif // GEODE_IS_ANDROID
#endif
        }

        // 8-byte pass
        for (ptrdiff_t node = 0; node < size; node += 4) {
            uint64_t nodeValue = *(uint64_t*)((uintptr_t)address + node);
            if (nodeValue % alignof(double) == 0 && !out.contains(node) && !out.contains(node + 4) && likelyDouble(nodeValue)) {
                out.emplace(node, ScanItemType::Double);
                continue;
            }

#if UINTPTR_MAX > 0xffffffff
            if (node % alignof(void*) == 0) {
# ifdef GEODE_IS_ANDROID
                if (getEmptyString() == nodeValue) {
                    out.emplace(node, ScanItemType::EmptyString);
                    continue;
                } else if (canReadPointer((uintptr_t)nodeValue)) {
                    if (likelyString((uintptr_t)nodeValue)) {
                        out.emplace(node, ScanItemType::String);
                    } else {
                        out.emplace(node, ScanItemType::HeapPointer);
                    }
                    continue;
                }
# else // GEODE_IS_ANDROID
                if (canReadPointer((uintptr_t)nodeValue)) {
                    if (likelyString((uintptr_t)nodeValue)) {
                        out.emplace(node, ScanItemType::String);
                    } else {
                        out.emplace(node, ScanItemType::HeapPointer);
                    }
                    continue;
                }
# endif // GEODE_IS_ANDROID
            }
#endif // UINTPTR_MAX > 0xffffffff
        }

        // seed value pass
        for (ptrdiff_t node = 8; node < size; node += 4) {
            uint32_t nodeValue1 = *(uint32_t*)((uintptr_t)address + node - 8);
            uint32_t nodeValue2 = *(uint32_t*)((uintptr_t)address + node - 4);
            uint32_t nodeValue3 = *(uint32_t*)((uintptr_t)address + node);

            if (!out.contains(node - 8) && !out.contains(node - 4) && !out.contains(node) && likelySeedValue(nodeValue1, nodeValue2, nodeValue3)) {
                out.emplace(node - 8, ScanItemType::SeedValue);
                node += 8;
            }
        }

        return out;
    }

    void dumpStruct(void* address, size_t size) {
        auto typenameResult = getTypename(address);

        if (typenameResult.isErr()) {
            geode::log::warn("Failed to dump struct: {}", typenameResult.unwrapErr());
            return;
        }

        geode::log::debug("Struct {}", typenameResult.unwrap());
        auto scanResult = scanMemory(address, size);

        for (uintptr_t node = 0; node < size; node += 4) {
            uint32_t nodeValue32 = *(uint32_t*)((uintptr_t)address + node);
            uint64_t nodeValue64 = *(uint64_t*)((uintptr_t)address + node);
            uintptr_t nodeValuePtr = *(uintptr_t*)((uintptr_t)address + node);

            std::string prefix32 = fmt::format("0x{:X} : {:08X}", node, nodeValue32);
            std::string prefix64 = fmt::format("0x{:X} : {:016X}", node, nodeValue64);
            std::string& prefixPtr = sizeof(void*) == 4 ? prefix32 : prefix64;

            if (node % alignof(void*) == 0 && canReadPointer(nodeValuePtr)) {
                auto name = getTypename((void*)nodeValuePtr);

                // valid type with a known typeinfo
                if (name.isOk()) {
                    geode::log::debug("{} ({}*)", prefixPtr, name.unwrap());
                    node += sizeof(void*) - 4;
                    continue;
                }

                // vtable ptr
                name = getTypenameFromVtable((void*)nodeValuePtr);
                if (name.isOk()) {
                    geode::log::debug("{} (vtable for {})", prefixPtr, name.unwrap());
                    node += sizeof(void*) - 4;
                    continue;
                }
            }

            // pre analyzed stuff
            if (scanResult.contains(node)) {
                auto type = scanResult.at(node);
                switch (type) {
                case ScanItemType::Float:
                    geode::log::debug("{} ({}f)", prefix32, data::bit_cast<float>(nodeValue32));
                    break;
                case ScanItemType::Double:
                    geode::log::debug("{} ({}d)", prefix64, data::bit_cast<double>(nodeValue64));
                    node += 4;
                    break;
                case ScanItemType::HeapPointer:
                    geode::log::debug("{} ({}) (ptr)", prefixPtr, nodeValuePtr);
                    node += sizeof(void*) - 4;
                    break;
                case ScanItemType::String:
                    geode::log::debug("{} ({}) (string: \"{}\")", prefixPtr, nodeValuePtr, (const char*)nodeValuePtr);
                    node += sizeof(void*) - 4;
                    break;
                case ScanItemType::EmptyString:
                    geode::log::debug("{} ({}) (string: \"\")", prefixPtr, nodeValuePtr);
                    node += sizeof(void*) - 4;
                    break;
                case ScanItemType::SeedValue: {
                    uint32_t valueNext = *(uint32_t*)((uintptr_t)address + node + 4);
                    uint32_t valueNext2 = *(uint32_t*)((uintptr_t)address + node + 8);
                    std::string prefix96 = fmt::format("0x{:X} : {:024X}", node, nodeValue64);
                    geode::log::debug("0x{:X} : ({:08X} {:08X} {:08X}) seed value: {}, {}, {}", node, nodeValue32, valueNext, valueNext2, nodeValue32, valueNext, valueNext2);
                    node += 8;
                    break;
                }
                }

                continue;
            }

            // anything tbh
            geode::log::debug("{} ({})", prefix32, nodeValue32);
        }
    }
}
