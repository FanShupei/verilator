// -*- mode: C++; c-file-style: "cc-mode" -*-
//=============================================================================
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0


#ifndef VERILATOR_VERILATED_FXT_C_H
#define VERILATOR_VERILATED_FXT_C_H

#include "verilated.h"
#include "verilated_trace.h"

#include <map>

#include "zigfxt/zigfxt.h"

class VerilatedFxtBuffer;

class VerilatedFxt final : public VerilatedTrace<VerilatedFxt, VerilatedFxtBuffer> {
public:
    using Super = VerilatedTrace<VerilatedFxt, VerilatedFxtBuffer>;

private:
    friend VerilatedFxtBuffer;  // Give the buffer access to the private bits

    //=========================================================================
    // FXT-specific internals

    FxtWriter m_fxt;
    FxtWriter::HierBuilder m_fxt_hier;
    std::map<uint32_t, fxt_writer_var> m_code2symbol;
    fxt_writer_var* m_symbolp = nullptr;  // same as m_code2symbol, but as an array

    // Prefixes to add to signal names/scope types
    std::vector<std::pair<std::string, VerilatedTracePrefixType>> m_prefixStack{
        {"", VerilatedTracePrefixType::SCOPE_MODULE}};

    // CONSTRUCTORS
    VL_UNCOPYABLE(VerilatedFxt);
    void declare(uint32_t code, const char* name, int dtypenum, VerilatedTraceSigDirection,
                 VerilatedTraceSigKind, VerilatedTraceSigType, bool array, int arraynum,
                 bool bussed, int msb, int lsb);

protected:
    //=========================================================================
    // Implementation of VerilatedTrace interface

    // Called when the trace moves forward to a new time point
    void emitTimeChange(uint64_t timeui) override;

    // Hooks called from VerilatedTrace
    bool preFullDump() override { return isOpen(); }
    bool preChangeDump() override { return isOpen(); }

    // Trace buffer management
    Buffer* getTraceBuffer(uint32_t fidx) override;
    void commitTraceBuffer(Buffer*) override;

    // Configure sub-class
    void configure(const VerilatedTraceConfig&) override {};

public:
    //=========================================================================
    // External interface to client code

    // CONSTRUCTOR
    explicit VerilatedFxt(void* fxt = nullptr);
    ~VerilatedFxt();

    // METHODS - All must be thread safe
    // Open the file; call isOpen() to see if errors
    void open(const char* filename) VL_MT_SAFE_EXCLUDES(m_mutex);
    // Close the file
    void close() VL_MT_SAFE_EXCLUDES(m_mutex);
    // Flush any remaining data to this file
    void flush() VL_MT_SAFE_EXCLUDES(m_mutex);
    // Return if file is open
    bool isOpen() const VL_MT_SAFE { return m_fxt.ptr != nullptr; }

    //=========================================================================
    // Internal interface to Verilator generated code

    void pushPrefix(const std::string&, VerilatedTracePrefixType);
    void popPrefix();

    void declEvent(uint32_t code, uint32_t fidx, const char* name, int dtypenum,
                   VerilatedTraceSigDirection, VerilatedTraceSigKind, VerilatedTraceSigType,
                   bool array, int arraynum);
    void declBit(uint32_t code, uint32_t fidx, const char* name, int dtypenum,
                 VerilatedTraceSigDirection, VerilatedTraceSigKind, VerilatedTraceSigType,
                 bool array, int arraynum);
    void declBus(uint32_t code, uint32_t fidx, const char* name, int dtypenum,
                 VerilatedTraceSigDirection, VerilatedTraceSigKind, VerilatedTraceSigType,
                 bool array, int arraynum, int msb, int lsb);
    void declQuad(uint32_t code, uint32_t fidx, const char* name, int dtypenum,
                  VerilatedTraceSigDirection, VerilatedTraceSigKind, VerilatedTraceSigType,
                  bool array, int arraynum, int msb, int lsb);
    void declArray(uint32_t code, uint32_t fidx, const char* name, int dtypenum,
                   VerilatedTraceSigDirection, VerilatedTraceSigKind, VerilatedTraceSigType,
                   bool array, int arraynum, int msb, int lsb);
    void declDouble(uint32_t code, uint32_t fidx, const char* name, int dtypenum,
                    VerilatedTraceSigDirection, VerilatedTraceSigKind, VerilatedTraceSigType,
                    bool array, int arraynum);

    void declDTypeEnum(int dtypenum, const char* name, uint32_t elements, unsigned int minValbits,
                       const char** itemNamesp, const char** itemValuesp);
};

#ifndef DOXYGEN
// Declare specialization here as it's used in VerilatedFxtC just below
template <>
void VerilatedFxt::Super::dump(uint64_t time);
template <>
void VerilatedFxt::Super::set_time_unit(const char* unitp);
template <>
void VerilatedFxt::Super::set_time_unit(const std::string& unit);
template <>
void VerilatedFxt::Super::set_time_resolution(const char* unitp);
template <>
void VerilatedFxt::Super::set_time_resolution(const std::string& unit);
template <>
void VerilatedFxt::Super::dumpvars(int level, const std::string& hier);
#endif

class VerilatedFxtBuffer VL_NOT_FINAL {
    // Give the trace file access to the private bits
    friend VerilatedFxt;
    friend VerilatedFxt::Super;
    friend VerilatedFxt::Buffer;
    friend VerilatedFxt::OffloadBuffer;

    VerilatedFxt& m_owner;

    // The FXT file handle
    FxtWriter m_fxt = m_owner.m_fxt;
    // code to fxt_writer_var map, as an array
    const fxt_writer_var* const m_symbolp = m_owner.m_symbolp;

    // CONSTRUCTOR
    explicit VerilatedFxtBuffer(VerilatedFxt& owner)
        : m_owner{owner} {}
    virtual ~VerilatedFxtBuffer() = default;

    //=========================================================================
    // Implementation of VerilatedTraceBuffer interface

    // Implementations of duck-typed methods for VerilatedTraceBuffer. These are
    // called from only one place (the full* methods), so always inline them.
    VL_ATTR_ALWINLINE void emitEvent(uint32_t code);
    VL_ATTR_ALWINLINE void emitBit(uint32_t code, CData newval);
    VL_ATTR_ALWINLINE void emitCData(uint32_t code, CData newval, int bits);
    VL_ATTR_ALWINLINE void emitSData(uint32_t code, SData newval, int bits);
    VL_ATTR_ALWINLINE void emitIData(uint32_t code, IData newval, int bits);
    VL_ATTR_ALWINLINE void emitQData(uint32_t code, QData newval, int bits);
    VL_ATTR_ALWINLINE void emitWData(uint32_t code, const WData* newvalp, int bits);
    VL_ATTR_ALWINLINE void emitDouble(uint32_t code, double newval);
};

class VerilatedFxtC VL_NOT_FINAL : public VerilatedTraceBaseC {
    VerilatedFxt m_sptrace;

    VL_UNCOPYABLE(VerilatedFxtC);

public:
    /// Construct the dump. Optional argument is ignored.
    explicit VerilatedFxtC(void* filep = nullptr)
        : m_sptrace{filep} {}
    /// Destruct, flush, and close the dump
    virtual ~VerilatedFxtC() { close(); }

    // METHODS - User called

    /// Return if file is open
    bool isOpen() const override VL_MT_SAFE { return m_sptrace.isOpen(); }
    /// Open a new FXT file
    virtual void open(const char* filename) VL_MT_SAFE { m_sptrace.open(filename); }
    /// Close dump
    void close() VL_MT_SAFE {
        m_sptrace.close();
        modelConnected(false);
    }
    /// Flush dump
    void flush() VL_MT_SAFE { m_sptrace.flush(); }
    /// Write one cycle of dump data
    /// Call with the current context's time just after eval'ed,
    /// e.g. ->dump(contextp->time())
    void dump(uint64_t timeui) { m_sptrace.dump(timeui); }
    /// Write one cycle of dump data - backward compatible and to reduce
    /// conversion warnings.  It's better to use a uint64_t time instead.
    void dump(double timestamp) { dump(static_cast<uint64_t>(timestamp)); }
    void dump(uint32_t timestamp) { dump(static_cast<uint64_t>(timestamp)); }
    void dump(int timestamp) { dump(static_cast<uint64_t>(timestamp)); }

    // METHODS - Internal/backward compatible
    // \protectedsection

    // Set time units (s/ms, defaults to ns)
    // Users should not need to call this, as for Verilated models, these
    // propagate from the Verilated default timeunit
    void set_time_unit(const char* unitp) VL_MT_SAFE { m_sptrace.set_time_unit(unitp); }
    void set_time_unit(const std::string& unit) VL_MT_SAFE { m_sptrace.set_time_unit(unit); }
    // Set time resolution (s/ms, defaults to ns)
    // Users should not need to call this, as for Verilated models, these
    // propagate from the Verilated default timeprecision
    void set_time_resolution(const char* unitp) VL_MT_SAFE {
        m_sptrace.set_time_resolution(unitp);
    }
    void set_time_resolution(const std::string& unit) VL_MT_SAFE {
        m_sptrace.set_time_resolution(unit);
    }
    // Set variables to dump, using $dumpvars format
    // If level = 0, dump everything and hier is then ignored
    void dumpvars(int level, const std::string& hier) VL_MT_SAFE {
        m_sptrace.dumpvars(level, hier);
    }

    // Internal class access
    VerilatedFxt* spTrace() { return &m_sptrace; }
};

#endif  // guard
