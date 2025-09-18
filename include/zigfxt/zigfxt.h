#pragma once

#ifndef _ZIGFXT_H_
#define _ZIGFXT_H_

#include <stdint.h>

struct fxt_writer;
struct fxt_writer_hier_builder;

// zero is always invalid
typedef uint32_t fxt_writer_var;

struct fxt_writer_create_info {
    int8_t timescale;
};

struct fxt_writer_var_info {
    uint8_t type;
    uint8_t dir;
    uint32_t len;
};

#ifdef __cplusplus
extern "C" {
#endif

struct fxt_writer* fxt_writer_create(
    const char* path,
    const struct fxt_writer_create_info* info
);

void fxt_writer_close(
    struct fxt_writer* writer
);

struct fxt_writer_hier_builder* fxt_writer_start_hierarchy(
    struct fxt_writer* writer
);

void fxt_writer_end_hierarchy(
    struct fxt_writer* writer,
    struct fxt_writer_hier_builder* hier
);

void fxt_writer_hier_discard(
    struct fxt_writer_hier_builder* hier
);

void fxt_writer_hier_scope(
    struct fxt_writer_hier_builder* hier,
    const char* name,
    uint8_t type
);

void fxt_writer_hier_upscope(
    struct fxt_writer_hier_builder* hier
);

fxt_writer_var fxt_writer_hier_create_var(
    struct fxt_writer_hier_builder* hier,
    const char* name,
    const struct fxt_writer_var_info* info
);

void fxt_writer_hier_create_alias(
    struct fxt_writer_hier_builder* hier,
    const char* name,
    const struct fxt_writer_var_info* info,
    fxt_writer_var target
);

void fxt_writer_emit_time_change(
    struct fxt_writer* writer,
    uint64_t time
);

void fxt_writer_emit_value_change(
    struct fxt_writer* writer,
    fxt_writer_var var,
    uint64_t value
);

void fxt_writer_flush(struct fxt_writer* writer);

void fxt_todo(const char* message);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

class FxtWriter {
public:
    fxt_writer* ptr;

    using VarHandle = fxt_writer_var;
    class HierBuilder {
    public:
        fxt_writer_hier_builder* ptr;
    
        explicit HierBuilder(fxt_writer_hier_builder* ptr=nullptr): ptr(ptr) {}
        void discard() { fxt_writer_hier_discard(ptr); }
        explicit operator bool() const { return ptr != nullptr; }

        void scope(const char* name) { fxt_writer_hier_scope(ptr, name, 0); }
        void upscope() { fxt_writer_hier_upscope(ptr); }

        VarHandle createVar(const char* name, const fxt_writer_var_info* info) {
            return fxt_writer_hier_create_var(ptr, name, info);
        }

        void createAlias(const char* name, const fxt_writer_var_info* info, VarHandle target) {
            fxt_writer_hier_create_alias(ptr, name, info, target);
        }
    };

    explicit FxtWriter(fxt_writer* ptr=nullptr): ptr(ptr) {}
    explicit operator bool() const { return ptr != nullptr; }

    static FxtWriter create(const char* path, const fxt_writer_create_info* info) {
        return FxtWriter{fxt_writer_create(path, info)};
    }

    void close() {
        fxt_writer_close(ptr);
        ptr = nullptr;
    }

    HierBuilder startHierarchy() {
        return HierBuilder{fxt_writer_start_hierarchy(ptr)};
    }

    void endHierarchy(HierBuilder& hier) {
        fxt_writer_end_hierarchy(ptr, hier.ptr);
        hier.ptr = nullptr;
    }

    void emitTimeChange(uint64_t time) {
        fxt_writer_emit_time_change(ptr, time);
    }

    void emitValueChange(VarHandle var, uint64_t value) {
        fxt_writer_emit_value_change(ptr, var, value);
    }
};

#endif

#endif
