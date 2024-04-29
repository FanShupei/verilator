// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Emit Makefile
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// Copyright 2004-2024 by Wilson Snyder. This program is free software; you
// can redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3EmitBuildPlan.h"

#include "V3EmitCBase.h"
#include "V3HierBlock.h"
#include "V3Os.h"

VL_DEFINE_DEBUG_FUNCTIONS;

const char* const BUILD_PLAN_VERSION = "preview-20240428";

class BuildPlanEmitter final {
    // METHODS

    // STATIC FUNCTIONS

    static std::string traceFormatJson() {
        if (v3Global.opt.trace()) {
            if (v3Global.opt.traceFormat().fst()) {
                return "\"fst\"";
            } else {
                return "\"vcd\"";
            }
        } else {
            return "null";
        }
    }

    static void globalFeaturesJson(std::ostream& of) {
        std::vector<std::string> features;
        if (v3Global.dpi()) features.push_back("dpi");
        if (v3Global.opt.vpi()) features.push_back("vpi");
        if (v3Global.opt.savable()) features.push_back("save");
        if (v3Global.opt.coverage()) features.push_back("cov");
        if (v3Global.opt.trace()) {
            if (v3Global.opt.traceFormat().fst()) {
                features.push_back("fst_c");
            } else {
                features.push_back("vcd_c");
            }
        }
        if (v3Global.usesProbDist()) features.push_back("probdist");
        if (v3Global.usesTiming()) features.push_back("timing");
        if (v3Global.opt.usesProfiler()) features.push_back("profiler");
        
        if (features.empty()) {
            of << "[]";
        } else {
            of << "[\"" << features[0] << "\"";
            for (size_t i = 1; i < features.size(); i++) {
                of << ", \"" << features[i] << "\"";
            }
            of << "]";
        }
    }

    static void globalFileListJson(std::ostream& of) {
        // FIXME: deduplicate code with globalFeaturesJson
        of << "[\"verilated.cpp\"";
        of << ", \"verilated_threads.cpp\"";
        if (v3Global.dpi()) of << ", \"verilated_dpi.cpp\"";
        if (v3Global.opt.vpi()) of << ", \"verilated_vpi.cpp\"";
        if (v3Global.opt.savable()) of << ", \"verilated_save.cpp\"";
        if (v3Global.opt.coverage()) of << ", \"verilated_cov.cpp\"";
        if (v3Global.opt.trace()) {
            of << ", \"" << v3Global.opt.traceSourceBase() << "_c.cpp\"";
        }
        if (v3Global.usesProbDist()) of << ", \"verilated_probdist.cpp\"";
        if (v3Global.usesTiming()) of << ", \"verilated_timing.cpp\"";
        if (v3Global.opt.usesProfiler()) {
            of << ", \"verilated_profiler.cpp\"";
        }
        of << "]";
    }

    static std::string globalMacrosJson() {
        if (v3Global.opt.systemC()) {
            return "[\"VM_SC=1\"]";
        } else {
            return "[]";
        }
    }

    static void modelSourcesJson(std::ostream& os) {
        std::vector<std::string> files;

        // FIXME: separate fast/slow, supported/non-supported files
        for (AstNodeFile* nodep = v3Global.rootp()->filesp(); nodep; nodep = VN_AS(nodep->nextp(), NodeFile)) {
            const AstCFile* const cfilep = VN_CAST(nodep, CFile);
            if (cfilep && cfilep->source()) {
                files.push_back(V3Os::filenameNonDir(cfilep->name()));
            }
        }

        if (files.empty()) {
            os << "[]";
        } else {
            os << "[\"" << files[0] << "\"";
            for (size_t i = 1; i < files.size(); i++) {
                os << ", \"" << files[i] << "\"";
            }
            os << "]";
        }
    }

    static void modelHeadersJson(std::ostream& os) {
        std::vector<std::string> files;

        // FIXME: separate fast/slow, supported/non-supported files
        for (AstNodeFile* nodep = v3Global.rootp()->filesp(); nodep; nodep = VN_AS(nodep->nextp(), NodeFile)) {
            const AstCFile* const cfilep = VN_CAST(nodep, CFile);
            if (cfilep && !cfilep->source()) {
                files.push_back(V3Os::filenameNonDir(cfilep->name()));
            }
        }

        if (files.empty()) {
            os << "[]";
        } else {
            os << "[\"" << files[0] << "\"";
            for (size_t i = 1; i < files.size(); i++) {
                os << ", \"" << files[i] << "\"";
            }
            os << "]";
        }
    }

    static void emitBuildPlan() {
        const std::unique_ptr<std::ofstream> of{
            V3File::new_ofstream(v3Global.opt.makeDir() + "/vl_build.json")};
        std::ostream& os = *of; 

        os << "{\n";
        os << "  \"version\": \"" << BUILD_PLAN_VERSION << "\",\n";
        os << "  \"config\": {\n";
        os << "    \"VERILATOR_ROOT\": \"" << V3Options::getenvVERILATOR_ROOT() << "\"\n",
        os << "  },\n";
        os << "  \"libverilated\": {\n";
        os << "    \"mode\": \"" << (v3Global.opt.systemC() ? "systemc" : "cpp") << "\",\n";

        os << "    \"features\": ";
        globalFeaturesJson(os);
        os << ",\n";

        os << "    \"compile_sources\": ";
        globalFileListJson(os);
        os << ",\n";

        os << "    \"compile_macros\": " << globalMacrosJson() << "\n";
        os << "  },\n";
        os << "  \"model\": {\n";
        os << "    \"config\": {\n";
        os << "      \"threads\": " << v3Global.opt.threads() << ",\n";
        os << "      \"trace\": " << traceFormatJson() << ",\n";
        os << "      \"timing\": " << (v3Global.usesTiming() ? "1" : "0") << ",\n";
        os << "      \"coverage\": " << (v3Global.opt.coverage() ? "1" : "0") << "\n";
        os << "    },\n";
        os << "    \"prefix\": \"" << v3Global.opt.prefix() << "\",\n";
        // TODO: os << "    \"top_ports\": {},\n";
        os << "    \"compile_sources\": ";
        modelSourcesJson(os);
        os << ",\n";

        os << "    \"compile_headers\": ";
        modelHeadersJson(os);
        os << ",\n";

        os << "    \"compile_macros\": " << globalMacrosJson() << "\n";
        os << "  }\n";
        os << "}\n";
    }

public:
    explicit BuildPlanEmitter() { emitBuildPlan(); }
    virtual ~BuildPlanEmitter() = default;
};

void V3EmitBuildPlan::emit() {
    UINFO(2, __FUNCTION__ << ": " << endl);
    const BuildPlanEmitter emitter;
}
