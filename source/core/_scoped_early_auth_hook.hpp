#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace kirkware {

constexpr std::uint32_t kScopedEarlyMarkerEntryRva = 0xD0F15E;
constexpr std::uint32_t kScopedEarlyPhaseEntryRva = 0xBC336D;
constexpr std::uint32_t kScopedEarlyDecisionEntryRva = 0xBC3511;
constexpr std::uint32_t kScopedAccumulatorEntryRva = 0xCDF166;
constexpr std::uint32_t kScopedAccumulatorReturnRva = 0xCDF16E;
constexpr std::uint32_t kScopedE9SourceEntryRva = 0xD23E92;
constexpr std::uint32_t kScopedE9SourceReturnRva = 0xD23E9C;
constexpr std::uint32_t kScopedE9SourceStateRva = 0x110C538;
constexpr std::uint32_t kScopedGate2SourceEntryRva = 0xCE463E;
constexpr std::uint32_t kScopedGate2SourceReturnRva = 0xCE4648;
constexpr std::uint32_t kScopedGate2SourceStateRva = 0x110EAA2;
constexpr std::uint32_t kScopedEarlyCaveRva = 0x58EC90;
constexpr std::uint32_t kWatermarkDefaultEntryRva = 0x296F40;
constexpr std::uint32_t kWatermarkDefaultDrawRva = 0x296F49;
constexpr std::uint32_t kWatermarkDefaultReturnRva = 0x2987B8;
constexpr std::uint32_t kRendererStateVmHandoffRva = 0x1E9AFA;
constexpr std::uint32_t kRendererStateNativeExitRva = 0x1E9CA9;
constexpr std::size_t kScopedE9SourceStubOffset = 0x00;
constexpr std::size_t kScopedGate2SourceStubOffset = 0x40;
constexpr std::size_t kWatermarkDefaultStubOffset = 0x70;
constexpr std::size_t kWatermarkDefaultStateOffset = 0xAF;
constexpr std::size_t kScopedEarlyMarkerStubOffset = 0x00;
constexpr std::size_t kScopedEarlyPhaseStubOffset = 0x30;
constexpr std::size_t kScopedEarlyDecisionStubOffset = 0x50;
constexpr std::size_t kScopedEarlyUsedOffset = 0xB0;
constexpr std::size_t kScopedEarlyStateOffset = 0xB1;
constexpr std::size_t kScopedAccumulatorStubOffset = 0xC0;
constexpr std::size_t kScopedEarlyStubSize = 0x10C;

constexpr std::array<std::uint8_t, 7> kScopedEarlyMarkerOriginal{
    0x45, 0x8B, 0x00, 0x41, 0x80, 0xFC, 0x04};
constexpr std::array<std::uint8_t, 7> kScopedEarlyPhaseOriginal{
    0x49, 0x81, 0xEB, 0xFA, 0x00, 0x00, 0x00};
constexpr std::array<std::uint8_t, 6> kScopedEarlyDecisionOriginal{
    0x0F, 0x85, 0x4E, 0x00, 0x00, 0x00};
constexpr std::array<std::uint8_t, 6> kScopedAccumulatorOriginal{
    0x0F, 0x85, 0x02, 0x00, 0x00, 0x00};
constexpr std::array<std::uint8_t, 10> kScopedE9SourceOriginal{
    0x48, 0x89, 0x19,
    0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00};
constexpr std::array<std::uint8_t, 10> kScopedGate2SourceOriginal{
    0x49, 0x89, 0x30,
    0x48, 0x81, 0xEA, 0x1D, 0x00, 0x00, 0x00};
constexpr std::array<std::uint8_t, 9> kWatermarkDefaultEntryOriginal{
    0x40, 0x84, 0xFF, 0x0F, 0x84, 0x6F, 0x18, 0x00, 0x00};
constexpr std::array<std::uint8_t, 5> kRendererStateVmHandoffOriginal{
    0xE9, 0xB8, 0xE6, 0xC9, 0x00};
constexpr std::array<std::uint8_t, 7> kRendererStateNativeExitOriginal{
    0x44, 0x88, 0x35, 0xCD, 0x04, 0x60, 0x00};

inline bool ScopedEarlyWriteRelative(
    std::uint8_t* output, std::uint32_t next_rva,
    std::uint32_t target_rva) {
    const std::int64_t relative =
        static_cast<std::int64_t>(target_rva) -
        static_cast<std::int64_t>(next_rva);
    if (relative < std::numeric_limits<std::int32_t>::min() ||
        relative > std::numeric_limits<std::int32_t>::max())
        return false;
    const auto value = static_cast<std::int32_t>(relative);
    std::memcpy(output, &value, sizeof(value));
    return true;
}

inline bool BuildWatermarkDefaultEntryPatch(
    std::array<std::uint8_t, kWatermarkDefaultEntryOriginal.size()>& entry) {
    entry = kWatermarkDefaultEntryOriginal;
    return true;
}

inline bool BuildRendererStateVmExitPatch(
    std::array<std::uint8_t, kRendererStateVmHandoffOriginal.size()>& entry) {
    entry = {0xE9, 0x00, 0x00, 0x00, 0x00};
    return ScopedEarlyWriteRelative(
        entry.data() + 1, kRendererStateVmHandoffRva + 5,
        kRendererStateNativeExitRva);
}

inline bool BuildScopedEarlyAuthPatch(
    std::array<std::uint8_t, 7>& marker_entry,
    std::array<std::uint8_t, 7>& phase_entry,
    std::array<std::uint8_t, 6>& decision_entry,
    std::array<std::uint8_t, 6>& accumulator_entry,
    std::array<std::uint8_t, 10>& e9_source_entry,
    std::array<std::uint8_t, 10>& gate2_source_entry,
    std::array<std::uint8_t, kScopedEarlyStubSize>& stub) {
    marker_entry = kScopedEarlyMarkerOriginal;
    phase_entry = kScopedEarlyPhaseOriginal;
    decision_entry = kScopedEarlyDecisionOriginal;
    accumulator_entry = {0xE9, 0x00, 0x00, 0x00, 0x00, 0x90};
    e9_source_entry = {
        0xE9, 0x00, 0x00, 0x00, 0x00,
        0x90, 0x90, 0x90, 0x90, 0x90};
    gate2_source_entry = {
        0xE9, 0x00, 0x00, 0x00, 0x00,
        0x90, 0x90, 0x90, 0x90, 0x90};
    stub.fill(0);

    constexpr std::array<std::uint8_t, 36> e9_source{
        0x48, 0x8D, 0x05, 0x00, 0x00, 0x00, 0x00,
        0x48, 0x39, 0x85, 0x5C, 0x01, 0x00, 0x00,
        0x75, 0x05,
        0xBB, 0x46, 0x00, 0x00, 0x00,
        0x48, 0x89, 0x19,
        0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00,
        0xE9, 0x00, 0x00, 0x00, 0x00};

    constexpr std::array<std::uint8_t, 38> gate2_source{
        0x50,
        0x48, 0x8D, 0x05, 0x00, 0x00, 0x00, 0x00,
        0x48, 0x39, 0x85, 0x5C, 0x01, 0x00, 0x00,
        0x75, 0x05,
        0xBE, 0x02, 0x00, 0x00, 0x00,
        0x58,
        0x49, 0x89, 0x30,
        0x48, 0x81, 0xEA, 0x1D, 0x00, 0x00, 0x00,
        0xE9, 0x00, 0x00, 0x00, 0x00};

    constexpr std::array<std::uint8_t, 76> accumulator_source{
        0x75, 0x45,
        0x48, 0x2B, 0x85, 0x4B, 0x01, 0x00, 0x00,
        0x48, 0x83, 0xF8, 0x1B,
        0x72, 0x26,
        0x48, 0x3D, 0x1C, 0x02, 0x00, 0x00,
        0x73, 0x1E,
        0x48, 0x03, 0x85, 0x4B, 0x01, 0x00, 0x00,
        0x80, 0x38, 0x00,
        0x75, 0x19,
        0x80, 0x78, 0xFF, 0x00,
        0x74, 0x13,
        0xC7, 0x85, 0x85, 0x00, 0x00, 0x00,
        0x1F, 0xDD, 0xB3, 0x52,
        0xEB, 0x07,
        0x48, 0x03, 0x85, 0x4B, 0x01, 0x00, 0x00,
        0x41, 0x80, 0xFD, 0x01,
        0x8A, 0x00,
        0xE9, 0x00, 0x00, 0x00, 0x00,
        0xE9, 0x00, 0x00, 0x00, 0x00};

    std::memcpy(stub.data() + kScopedE9SourceStubOffset,
                e9_source.data(), e9_source.size());
    std::memcpy(stub.data() + kScopedGate2SourceStubOffset,
                gate2_source.data(), gate2_source.size());
    std::memcpy(stub.data() + kScopedAccumulatorStubOffset,
                accumulator_source.data(), accumulator_source.size());

    const auto cave = kScopedEarlyCaveRva;
    const auto accumulator = cave + static_cast<std::uint32_t>(
                                        kScopedAccumulatorStubOffset);
    const auto e9_source_stub = cave + static_cast<std::uint32_t>(
                                           kScopedE9SourceStubOffset);
    const auto gate2_source_stub = cave + static_cast<std::uint32_t>(
                                              kScopedGate2SourceStubOffset);
    return
        ScopedEarlyWriteRelative(e9_source_entry.data() + 1,
                                 kScopedE9SourceEntryRva + 5,
                                 e9_source_stub) &&
        ScopedEarlyWriteRelative(stub.data() +
                                     kScopedE9SourceStubOffset + 3,
                                 e9_source_stub + 7,
                                 kScopedE9SourceStateRva) &&
        ScopedEarlyWriteRelative(stub.data() +
                                     kScopedE9SourceStubOffset + 32,
                                 e9_source_stub + 36,
                                 kScopedE9SourceReturnRva) &&
        ScopedEarlyWriteRelative(gate2_source_entry.data() + 1,
                                 kScopedGate2SourceEntryRva + 5,
                                 gate2_source_stub) &&
        ScopedEarlyWriteRelative(stub.data() +
                                     kScopedGate2SourceStubOffset + 4,
                                 gate2_source_stub + 8,
                                 kScopedGate2SourceStateRva) &&
        ScopedEarlyWriteRelative(stub.data() +
                                     kScopedGate2SourceStubOffset + 34,
                                 gate2_source_stub + 38,
                                 kScopedGate2SourceReturnRva) &&
        ScopedEarlyWriteRelative(accumulator_entry.data() + 1,
                                 kScopedAccumulatorEntryRva + 5,
                                 accumulator) &&
        ScopedEarlyWriteRelative(stub.data() +
                                     kScopedAccumulatorStubOffset + 67,
                                 accumulator + 71,
                                 kScopedAccumulatorReturnRva) &&
        ScopedEarlyWriteRelative(stub.data() +
                                     kScopedAccumulatorStubOffset + 72,
                                 accumulator + 76,
                                 kScopedAccumulatorReturnRva);
}

}
