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

class BuildPlanEmitter final {
    // METHODS

    // STATIC FUNCTIONS

    static void emitBuildPlan() {
        const std::unique_ptr<std::ofstream> of{
            V3File::new_ofstream(v3Global.opt.makeDir() + "/vl_compile.json")};

        *of << "# build plan\n";
    }

public:
    explicit BuildPlanEmitter() { emitBuildPlan(); }
    virtual ~BuildPlanEmitter() = default;
};

void V3EmitBuildPlan::emit() {
    UINFO(2, __FUNCTION__ << ": " << endl);
    const BuildPlanEmitter emitter;
}
