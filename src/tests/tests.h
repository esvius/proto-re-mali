// Registry of differential tests, one per reconstructed function.
#pragma once

#include <cstddef>

#include "../host/difftest.h"

namespace tests {

const protore::DiffTest* All(size_t* count);

void TestReport(protore::DiffResult* r);
void TestTraceVector(protore::DiffResult* r);
void TestNameHash(protore::DiffResult* r);
void TestCementLookup(protore::DiffResult* r);
void TestCementFileRange(protore::DiffResult* r);
void TestCementState(protore::DiffResult* r);
void TestMemory(protore::DiffResult* r);
void TestCementLoad(protore::DiffResult* r);
void TestCementLifetime(protore::DiffResult* r);
void TestFile(protore::DiffResult* r);

}  // namespace tests
