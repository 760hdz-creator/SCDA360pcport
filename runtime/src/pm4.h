// PM4 command packets (Xenos command processor format).
#pragma once
#include <cstdint>
#include <functional>
#include <type_traits>

// Total dwords of the packet starting with `header`, header included.
uint32_t Pm4PacketDwords(uint32_t header);

constexpr uint32_t Pm4Type(uint32_t h) { return h >> 30; }
constexpr uint32_t Pm4Opcode(uint32_t h) { return (h >> 8) & 0x7F; }

struct Pm4Ring
{
    uint32_t base;      // guest virtual
    uint32_t sizeBytes; // power of two
};

// Payload dword reader: a non-owning reference to a callable (no std::function dispatch; the CP reads
// ~1.5M payload dwords per frame on newyork_02). The callable must outlive the Pm4Payload: pass it
// as a temporary argument, never store one.
class Pm4Payload
{
    const void* m_obj;
    uint32_t (*m_call)(const void*, uint32_t);
    const uint32_t* m_raw = nullptr;

public:
    // raw: the payload's big-endian dwords when contiguous in host memory (indirect buffers), else null.
    template <class F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, Pm4Payload>)
    Pm4Payload(const F& f, const uint32_t* raw = nullptr)
        : m_obj(&f), m_call([](const void* o, uint32_t i) -> uint32_t { return (*static_cast<const F*>(o))(i); }), m_raw(raw)
    {
    }
    uint32_t operator()(uint32_t i) const { return m_call(m_obj, i); }
    const uint32_t* Raw() const { return m_raw; }
};
// Returns false to stop before this packet (it is retried on the next walk).
using Pm4Exec = std::function<bool(uint32_t header, const Pm4Payload& payload)>;

// Walks whole packets from readDword up to writeDword (dword indices into the ring), calling
// exec once per packet. A packet not completely below writeDword waits for the next call.
// Returns the new read index.
uint32_t Pm4WalkRing(const Pm4Ring& ring, uint32_t readDword, uint32_t writeDword, const Pm4Exec& exec);
