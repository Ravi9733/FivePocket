#pragma once
// v217: overlap query over an address-ordered map of fixed-size records (the registry's critical sections, 28 bytes
// each). Visits only the records that can overlap [address, address+bytes): key > address-(size-1)... strictly,
// key+size > address and key < address+bytes. Same predicate as a scan of every record (tests/cs_range_test.cpp).
#include <cstdint>

namespace recomp::bootstrap {
template<class Map,class Active>
bool anyActiveOverlap(const Map& records,std::uint32_t address,std::uint64_t bytes,std::uint32_t size,Active active) {
    const std::uint32_t from=address>=size?address-(size-1):0;
    for(auto it=records.lower_bound(from);it!=records.end() && it->first<std::uint64_t{address}+bytes;++it)
        if(address<std::uint64_t{it->first}+size && active(it->first,it->second))return true;
    return false;
}
template<class Map,class Active>
bool anyActiveOverlapScan(const Map& records,std::uint32_t address,std::uint64_t bytes,std::uint32_t size,Active active) {
    for(const auto& [at,state]:records)
        if(at<std::uint64_t{address}+bytes && address<std::uint64_t{at}+size && active(at,state))return true;
    return false;
}
}
