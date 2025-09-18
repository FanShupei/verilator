// clang-format off

#include "verilated.h"
#include "verilated_fxt_c.h"

#include "zigfxt/zigfxt.h"

#include <sstream>

#define VL_SUB_T VerilatedFxt
#define VL_BUF_T VerilatedFxtBuffer
#include "verilated_trace_imp.h"
#undef VL_SUB_T
#undef VL_BUF_T

//=============================================================================
// VerilatedFxt
VerilatedFxt::VerilatedFxt(void* /*fxt*/) {}

VerilatedFxt::~VerilatedFxt() {
    if (m_fxt_hier) m_fxt_hier.discard();
    if (m_fxt) m_fxt.close();
    if (m_symbolp) VL_DO_CLEAR(delete[] m_symbolp, m_symbolp = nullptr);
}

void VerilatedFxt::open(const char* filename) VL_MT_SAFE_EXCLUDES(m_mutex) {
    const VerilatedLockGuard lock{m_mutex};
    m_fxt = FxtWriter::create(filename, nullptr);
    constDump(true);  // First dump must contain the const signals
    fullDump(true);  // First dump must contain the const signals

    m_fxt_hier = m_fxt.startHierarchy();
    Super::traceInit();
    m_fxt.endHierarchy(m_fxt_hier);

    // convert m_code2symbol into an array for fast lookup
    if (!m_symbolp) {
        m_symbolp = new fxt_writer_var[nextCode()]{0};
        for (const auto& i : m_code2symbol) m_symbolp[i.first] = i.second;
    }
    m_code2symbol.clear();
}

void VerilatedFxt::close() VL_MT_SAFE_EXCLUDES(m_mutex) {
    const VerilatedLockGuard lock{m_mutex};
    Super::closeBase();
    m_fxt.close();
}

void VerilatedFxt::flush() VL_MT_SAFE_EXCLUDES(m_mutex) {
    const VerilatedLockGuard lock{m_mutex};
    Super::flushBase();
    // TODO: fxt_writer_flush(m_fxt);
}

void VerilatedFxt::emitTimeChange(uint64_t timeui) {
    m_fxt.emitTimeChange(timeui);
}

static std::pair<bool, uint8_t> toFxtScopeType(VerilatedTracePrefixType type) {
    switch (type) {
    case VerilatedTracePrefixType::SCOPE_MODULE: return {true, 1};
    case VerilatedTracePrefixType::SCOPE_INTERFACE: return {true, 2};
    case VerilatedTracePrefixType::STRUCT_PACKED:
    case VerilatedTracePrefixType::STRUCT_UNPACKED: return {true, 3};
    case VerilatedTracePrefixType::UNION_PACKED: return {true, 4};
    default: return {false, /* unused so whatever, just need a value */ 0};
    }
}

void VerilatedFxt::pushPrefix(const std::string& name, VerilatedTracePrefixType type) {
    assert(!m_prefixStack.empty());  // Constructor makes an empty entry
    // An empty name means this is the root of a model created with
    // name()=="".  The tools get upset if we try to pass this as empty, so
    // we put the signals under a new $rootio scope, but the signals
    // further down will be peers, not children (as usual for name()!="").
    const std::string prevPrefix = m_prefixStack.back().first;
    if (name == "$rootio" && !prevPrefix.empty()) {
        // Upper has name, we can suppress inserting $rootio, but still push so popPrefix works
        m_prefixStack.emplace_back(prevPrefix, VerilatedTracePrefixType::ROOTIO_WRAPPER);
        return;
    } else if (name.empty()) {
        m_prefixStack.emplace_back(prevPrefix, VerilatedTracePrefixType::ROOTIO_WRAPPER);
        return;
    }

    const std::string newPrefix = prevPrefix + name;
    const auto pair = toFxtScopeType(type);
    const bool properScope = pair.first;
    m_prefixStack.emplace_back(newPrefix + (properScope ? " " : ""), type);
    if (properScope) {
        const std::string scopeName = lastWord(newPrefix);
        m_fxt_hier.scope(scopeName.c_str()); // TODO: pair.second
    }
}

void VerilatedFxt::popPrefix() {
    assert(!m_prefixStack.empty());
    const bool properScope = toFxtScopeType(m_prefixStack.back().second).first;
    if (properScope) {
        m_fxt_hier.upscope();
    }
    m_prefixStack.pop_back();
    assert(!m_prefixStack.empty());  // Always one left, the constructor's initial one
}

void VerilatedFxt::declare(uint32_t code, const char* name, int dtypenum,
                           VerilatedTraceSigDirection direction, VerilatedTraceSigKind kind,
                           VerilatedTraceSigType type, bool array, int arraynum, bool bussed,
                           int msb, int lsb) {
    const int bits = ((msb > lsb) ? (msb - lsb) : (lsb - msb)) + 1;

    const std::string hierarchicalName = m_prefixStack.back().first + name;

    const bool enabled = Super::declCode(code, hierarchicalName, bits);
    if (!enabled) return;

    assert(hierarchicalName.rfind(' ') != std::string::npos);
    std::stringstream name_ss;
    name_ss << lastWord(hierarchicalName);
    if (array) name_ss << "[" << arraynum << "]";
    if (bussed) name_ss << " [" << msb << ":" << lsb << "]";
    const std::string name_str = name_ss.str();

    // assert(dtypenum == 0); // TODO: support enum

    fxt_writer_var_info var_info;
    var_info.dir = (uint8_t)direction;
    var_info.type = (uint8_t)type;
    var_info.len = bits;

    const auto it = vlstd::as_const(m_code2symbol).find(code);
    if (it == m_code2symbol.end()) {  // New
        m_code2symbol[code]
            = m_fxt_hier.createVar(name_str.c_str(), &var_info);
    } else {  // Alias
        m_fxt_hier.createAlias(name_str.c_str(), &var_info, it->second);
    }
}

void VerilatedFxt::declEvent(uint32_t code, uint32_t fidx, const char* name, int dtypenum,
                             VerilatedTraceSigDirection direction, VerilatedTraceSigKind kind,
                             VerilatedTraceSigType type, bool array, int arraynum) {
    declare(code, name, dtypenum, direction, kind, type, array, arraynum, false, 0, 0);
}
void VerilatedFxt::declBit(uint32_t code, uint32_t fidx, const char* name, int dtypenum,
                           VerilatedTraceSigDirection direction, VerilatedTraceSigKind kind,
                           VerilatedTraceSigType type, bool array, int arraynum) {
    declare(code, name, dtypenum, direction, kind, type, array, arraynum, false, 0, 0);
}
void VerilatedFxt::declBus(uint32_t code, uint32_t fidx, const char* name, int dtypenum,
                           VerilatedTraceSigDirection direction, VerilatedTraceSigKind kind,
                           VerilatedTraceSigType type, bool array, int arraynum, int msb,
                           int lsb) {
    declare(code, name, dtypenum, direction, kind, type, array, arraynum, true, msb, lsb);
}
void VerilatedFxt::declQuad(uint32_t code, uint32_t fidx, const char* name, int dtypenum,
                            VerilatedTraceSigDirection direction, VerilatedTraceSigKind kind,
                            VerilatedTraceSigType type, bool array, int arraynum, int msb,
                            int lsb) {
    declare(code, name, dtypenum, direction, kind, type, array, arraynum, true, msb, lsb);
}
void VerilatedFxt::declArray(uint32_t code, uint32_t fidx, const char* name, int dtypenum,
                             VerilatedTraceSigDirection direction, VerilatedTraceSigKind kind,
                             VerilatedTraceSigType type, bool array, int arraynum, int msb,
                             int lsb) {
    declare(code, name, dtypenum, direction, kind, type, array, arraynum, true, msb, lsb);
}
void VerilatedFxt::declDouble(uint32_t code, uint32_t fidx, const char* name, int dtypenum,
                              VerilatedTraceSigDirection direction, VerilatedTraceSigKind kind,
                              VerilatedTraceSigType type, bool array, int arraynum) {
    declare(code, name, dtypenum, direction, kind, type, array, arraynum, false, 63, 0);
}
//=============================================================================
// Get/commit trace buffer

VerilatedFxt::Buffer* VerilatedFxt::getTraceBuffer(uint32_t fidx) { return new Buffer{*this}; }

void VerilatedFxt::commitTraceBuffer(VerilatedFxt::Buffer* bufp) { delete bufp; }

//=============================================================================
// VerilatedFxtBuffer implementation

//=============================================================================
// Trace rendering primitives

// Note: emit* are only ever called from one place (full* in
// verilated_trace_imp.h, which is included in this file at the top),
// so always inline them.

VL_ATTR_ALWINLINE
void VerilatedFxtBuffer::emitEvent(uint32_t code) {
    VL_DEBUG_IFDEF(assert(m_symbolp[code]););
    fxt_todo("emit event");
}

VL_ATTR_ALWINLINE
void VerilatedFxtBuffer::emitBit(uint32_t code, CData newval) {
    VL_DEBUG_IFDEF(assert(m_symbolp[code]););
    m_fxt.emitValueChange(m_symbolp[code], newval);
}

VL_ATTR_ALWINLINE
void VerilatedFxtBuffer::emitCData(uint32_t code, CData newval, int bits) {
    VL_DEBUG_IFDEF(assert(m_symbolp[code]););
    m_fxt.emitValueChange(m_symbolp[code], newval);
}

VL_ATTR_ALWINLINE
void VerilatedFxtBuffer::emitSData(uint32_t code, SData newval, int bits) {
    VL_DEBUG_IFDEF(assert(m_symbolp[code]););
    m_fxt.emitValueChange(m_symbolp[code], newval);
}

VL_ATTR_ALWINLINE
void VerilatedFxtBuffer::emitIData(uint32_t code, IData newval, int bits) {
    VL_DEBUG_IFDEF(assert(m_symbolp[code]););
    m_fxt.emitValueChange(m_symbolp[code], newval);
}

VL_ATTR_ALWINLINE
void VerilatedFxtBuffer::emitQData(uint32_t code, QData newval, int bits) {
    VL_DEBUG_IFDEF(assert(m_symbolp[code]););
    m_fxt.emitValueChange(m_symbolp[code], newval);
}

VL_ATTR_ALWINLINE
void VerilatedFxtBuffer::emitWData(uint32_t code, const WData* newvalp, int bits) {
    VL_DEBUG_IFDEF(assert(m_symbolp[code]););
    fxt_todo("emit WData slice");
}

VL_ATTR_ALWINLINE
void VerilatedFxtBuffer::emitDouble(uint32_t code, double newval) {
    VL_DEBUG_IFDEF(assert(m_symbolp[code]););
    fxt_todo("emit double");
}
