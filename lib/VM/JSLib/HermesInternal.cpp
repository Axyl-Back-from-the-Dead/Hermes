/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "JSLibInternal.h"

#include "hermes/BCGen/HBC/Bytecode.h"
#include "hermes/BCGen/HBC/BytecodeDisassembler.h"
#include "hermes/BCGen/HBC/BytecodeFileFormat.h"
#include "hermes/BCGen/HBC/HBC.h"
#include "hermes/Support/Base64vlq.h"
#include "hermes/Support/OSCompat.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/HiddenClass.h"
#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSArrayBuffer.h"
#include "hermes/VM/JSLib.h"
#include "hermes/VM/JSLib/RuntimeCommonStorage.h"
#include "hermes/VM/JSTypedArray.h"
#include "hermes/VM/JSWeakMapImpl.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/StackFrame-inline.h"
#include "hermes/VM/StringView.h"

#include <cstring>
#include <random>
#include <fcntl.h>
#include <sys/stat.h>
#include <memory>
#include <system_error>
#include <cstdlib>
#include <utility>

namespace hermes {
namespace vm {

// #! Pyoncord added: Taken from https://github.com/Aliucord/hermes/blob/e3f228be48e54b6eb32a357688ed1323b568bcc6/lib/VM/JSLib/AliuHermes.cpp#L189C1-L279C1
class StringVisitor : public hbc::BytecodeVisitor {
 private:
  inst::OpCode opcode_;
  uint32_t i_ = 0;

  /// Check if the zero based \p operandIndex in instruction \p opCode is a
  /// string table ID.
  static bool isOperandStringID(inst::OpCode opCode, unsigned operandIndex) {
    #define OPERAND_STRING_ID(name, operandNumber)                     \
      if (opCode == inst::OpCode::name && operandIndex == operandNumber - 1) \
        return true;
    #include "hermes/BCGen/HBC/BytecodeList.def"

    return false;
  }

 protected:
  void preVisitInstruction(inst::OpCode opcode, const uint8_t *ip, int length)
      override {
    opcode_ = opcode;
  }

  CallResult<HermesValue> makeStr(
      ArrayRef<unsigned char> storage,
      StringTableEntry entry) const {
    if (entry.isUTF16()) {
      const auto *s =
          (const char16_t *)(storage.begin() + entry.getOffset());
      return StringPrimitive::create(runtime_, UTF16Ref{s, entry.getLength()});
    } else {
      const char *s = (const char *)storage.begin() + entry.getOffset();
      return StringPrimitive::create(runtime_, ASCIIRef{s, entry.getLength()});
    }
  }

  void visitString(StringID stringID) {
    auto storage = bcProvider_->getStringStorage();
    auto entry = bcProvider_->getStringTableEntry(stringID);

    CallResult<HermesValue> strResult = makeStr(storage, entry);

    auto str = runtime_.makeHandle<StringPrimitive>(*strResult);

    JSArray::setElementAt(array_, runtime_, i_, str);
    i_++;
  }

  void visitOperand(
      const uint8_t *ip,
      inst::OperandType operandType,
      const uint8_t *operandBuf,
      int operandIndex) override {
    const bool isStringID = isOperandStringID(opcode_, operandIndex);
    if (!isStringID)
      return;

    switch (operandType) {
#define DEFINE_OPERAND_TYPE(name, ctype)         \
  case inst::OperandType::name: {                      \
    ctype operandVal;                            \
    hbc::decodeOperand(operandBuf, &operandVal);      \
    if (operandType == inst::OperandType::Addr8 ||     \
        operandType == inst::OperandType::Addr32) {    \
      /* operandVal is relative to current ip.*/ \
      return;                                    \
    }                                            \
    visitString(operandVal);                     \
    break;                                       \
  }
#include "hermes/BCGen/HBC/BytecodeList.def"
    }
  }

  void afterStart() override {
    if (LLVM_UNLIKELY(
            JSArray::setLengthProperty(array_, runtime_, i_) ==
            ExecutionStatus::EXCEPTION)) {
      (void) runtime_.raiseTypeError(static_cast<const llvh::StringRef>(
          "Failed to set array length in StringVisitor: " + std::string(strerror(errno))));
    }
  }

 public:
  hermes::vm::Handle<hermes::vm::JSArray> array_;
  Runtime &runtime_;
  StringVisitor(
      std::shared_ptr<hbc::BCProvider> bcProvider,
      hermes::vm::Handle<hermes::vm::JSArray> array,
      Runtime &runtime)
      : BytecodeVisitor(std::move(bcProvider)), array_(std::move(array)), runtime_(runtime) {}
};

/// \return a SymbolID  for a given C string \p s.
static inline CallResult<Handle<SymbolID>> symbolForCStr(
    Runtime &rt,
    const char *s) {
  return rt.getIdentifierTable().getSymbolHandle(rt, ASCIIRef{s, strlen(s)});
}

// ES7 24.1.1.3
CallResult<HermesValue>
hermesInternalDetachArrayBuffer(void *, Runtime &runtime, NativeArgs args) {
  auto buffer = args.dyncastArg<JSArrayBuffer>(0);
  if (!buffer) {
    return runtime.raiseTypeError(
        "Cannot use detachArrayBuffer on something which "
        "is not an ArrayBuffer foo");
  }
  buffer->detach(runtime.getHeap());
  // "void" return
  return HermesValue::encodeUndefinedValue();
}

CallResult<HermesValue>
hermesInternalGetEpilogues(void *, Runtime &runtime, NativeArgs args) {
  // Create outer array with one element per module.
  auto eps = runtime.getEpilogues();
  auto outerLen = eps.size();
  auto outerResult = JSArray::create(runtime, outerLen, outerLen);

  if (outerResult == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto outer = *outerResult;
  if (outer->setStorageEndIndex(outer, runtime, outerLen) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // Set each element to a Uint8Array holding the epilogue for that module.
  for (unsigned i = 0; i < outerLen; ++i) {
    auto innerLen = eps[i].size();
    if (innerLen != 0) {
      auto result = Uint8Array::allocate(runtime, innerLen);
      if (result == ExecutionStatus::EXCEPTION) {
        return ExecutionStatus::EXCEPTION;
      }
      auto ta = result.getValue();
      std::memcpy(ta->begin(runtime), eps[i].begin(), innerLen);
      const auto shv = SmallHermesValue::encodeObjectValue(*ta, runtime);
      JSArray::unsafeSetExistingElementAt(*outer, runtime, i, shv);
    }
  }
  return HermesValue::encodeObjectValue(*outer);
}

/// Used for testing, determines how many live values
/// are in the given WeakMap or WeakSet.
CallResult<HermesValue>
hermesInternalGetWeakSize(void *, Runtime &runtime, NativeArgs args) {
  if (auto M = args.dyncastArg<JSWeakMap>(0)) {
    return HermesValue::encodeNumberValue(
        JSWeakMap::debugFreeSlotsAndGetSize(runtime, *M));
  }

  if (auto S = args.dyncastArg<JSWeakSet>(0)) {
    return HermesValue::encodeNumberValue(
        JSWeakSet::debugFreeSlotsAndGetSize(runtime, *S));
  }

  return runtime.raiseTypeError(
      "getWeakSize can only be called on a WeakMap/WeakSet");
}

namespace {
/// Populates the instrumentes stats object using \p addProp as the handler for
/// adding properties to the object. \p addProp's should be invocable with
/// (const char *), or (const char *, double). The first prototype is used when
/// property passthrough is enable (i.e., the \p addProp will figure out what
/// the property value is), and the second, when new/actual values should be
/// recorded. Note that any property below added with PASSTHROUGH_PROP is only
/// populated in passthrough mode (i.e., when replaying calls to
/// getInstrumentedStats during synth trace replay).
template <typename AP>
ExecutionStatus populateInstrumentedStats(Runtime &runtime, AP addProp) {
  constexpr bool addPropTakesValue =
      std::is_invocable_v<AP, const char *, double>;
  constexpr bool addPropGeneratesValue = std::is_invocable_v<AP, const char *>;
  static_assert(
      addPropGeneratesValue || addPropTakesValue, "invalid addProp prototype");

  // PASSTHROUGH_PROP is used to populate the instrumented stats objects with
  // properties that are no longer being returned by hermes, but at one point
  // where, and thus are kept here for synth trace playback compatibility only.
#define PASSTHROUGH_PROP(name)                                          \
  do {                                                                  \
    if constexpr (addPropGeneratesValue) {                              \
      if (LLVM_UNLIKELY(addProp(name) == ExecutionStatus::EXCEPTION)) { \
        return ExecutionStatus::EXCEPTION;                              \
      }                                                                 \
    }                                                                   \
  } while (0)

#define ADD_PROP(name, value)                                                  \
  do {                                                                         \
    if constexpr (addPropGeneratesValue) {                                     \
      PASSTHROUGH_PROP(name);                                                  \
    } else {                                                                   \
      if (LLVM_UNLIKELY(addProp(name, value) == ExecutionStatus::EXCEPTION)) { \
        return ExecutionStatus::EXCEPTION;                                     \
      }                                                                        \
    }                                                                          \
  } while (0)

  auto &heap = runtime.getHeap();
  GCBase::HeapInfo info;
  heap.getHeapInfo(info);

  // To ensure synth trace compatibility, properties should not be removed nor
  // reordered. To "remove" a property use PASSTHROUGH_PROP instead of ADD_PROP.
  PASSTHROUGH_PROP("js_hostFunctionTime");
  PASSTHROUGH_PROP("js_hostFunctionCPUTime");
  PASSTHROUGH_PROP("js_hostFunctionCount");
  PASSTHROUGH_PROP("js_evaluateJSTime");
  PASSTHROUGH_PROP("js_evaluateJSCPUTime");
  PASSTHROUGH_PROP("js_evaluateJSCount");
  PASSTHROUGH_PROP("js_incomingFunctionTime");
  PASSTHROUGH_PROP("js_incomingFunctionCPUTime");
  PASSTHROUGH_PROP("js_incomingFunctionCount");
  ADD_PROP("js_VMExperiments", runtime.getVMExperimentFlags());
  PASSTHROUGH_PROP("js_hermesTime");
  PASSTHROUGH_PROP("js_hermesCPUTime");
  PASSTHROUGH_PROP("js_hermesThreadMinorFaults");
  PASSTHROUGH_PROP("js_hermesThreadMajorFaults");
  ADD_PROP("js_numGCs", heap.getNumGCs());
  ADD_PROP("js_gcCPUTime", heap.getGCCPUTime());
  ADD_PROP("js_gcTime", heap.getGCTime());
  ADD_PROP("js_totalAllocatedBytes", info.totalAllocatedBytes);
  ADD_PROP("js_allocatedBytes", info.allocatedBytes);
  ADD_PROP("js_heapSize", info.heapSize);
  ADD_PROP("js_mallocSizeEstimate", info.mallocSizeEstimate);
  ADD_PROP("js_vaSize", info.va);
  ADD_PROP("js_markStackOverflows", info.numMarkStackOverflows);
  PASSTHROUGH_PROP("js_hermesVolCtxSwitches");
  PASSTHROUGH_PROP("js_hermesInvolCtxSwitches");
  PASSTHROUGH_PROP("js_pageSize");
  PASSTHROUGH_PROP("js_threadAffinityMask");
  PASSTHROUGH_PROP("js_threadCPU");
  PASSTHROUGH_PROP("js_bytecodePagesResident");
  PASSTHROUGH_PROP("js_bytecodePagesResidentRuns");
  PASSTHROUGH_PROP("js_bytecodePagesAccessed");
  PASSTHROUGH_PROP("js_bytecodeSize");
  PASSTHROUGH_PROP("js_bytecodePagesTraceHash");
  PASSTHROUGH_PROP("js_bytecodeIOTime");
  PASSTHROUGH_PROP("js_bytecodePagesTraceSample");

#undef PASSTHROUGH_PROP
#undef ADD_PROP

  return ExecutionStatus::RETURNED;
}

/// Converts \p val to a HermesValue.
CallResult<HermesValue> statsTableValueToHermesValue(
    Runtime &runtime,
    const MockedEnvironment::StatsTableValue &val) {
  if (val.isNum()) {
    return HermesValue::encodeDoubleValue(val.num());
  }

  auto strRes =
      StringPrimitive::create(runtime, createASCIIRef(val.str().c_str()));
  if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return *strRes;
}
} // namespace

/// \return an object containing various instrumented statistics.
CallResult<HermesValue>
hermesInternalGetInstrumentedStats(void *, Runtime &runtime, NativeArgs args) {
  GCScope gcScope(runtime);
  auto resultHandle = runtime.makeHandle(JSObject::create(runtime));
  // Printing the values would be unstable, so prevent that.
  if (runtime.shouldStabilizeInstructionCount())
    return resultHandle.getHermesValue();

  MockedEnvironment::StatsTable *statsTable = nullptr;
  auto *const storage = runtime.getCommonStorage();
  if (storage->env) {
    if (!storage->env->callsToHermesInternalGetInstrumentedStats.empty()) {
      statsTable =
          &storage->env->callsToHermesInternalGetInstrumentedStats.front();
    }
  }

  std::unique_ptr<MockedEnvironment::StatsTable> newStatsTable;
  if (storage->shouldTrace) {
    newStatsTable.reset(new MockedEnvironment::StatsTable());
  }

  /// Adds \p key with \p val to the resultHandle object. \p newStatsTableVal is
  /// the value \p key should have in the newStatsTable object (i.e., during
  /// synth trace recording).
  auto addToResultHandle =
      [&](llvh::StringRef key, HermesValue val, auto newStatsTableVal) {
        Handle<> valHandle = runtime.makeHandle(val);
        auto keySym = symbolForCStr(runtime, key.data());
        if (LLVM_UNLIKELY(keySym == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }

        auto status = JSObject::defineNewOwnProperty(
            resultHandle,
            runtime,
            **keySym,
            PropertyFlags::defaultNewNamedPropertyFlags(),
            valHandle);

        if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }

        if (newStatsTable) {
          newStatsTable->try_emplace(key, newStatsTableVal);
        }

        return ExecutionStatus::RETURNED;
      };

  ExecutionStatus populateRes;
  if (!statsTable) {
    /// Adds a property to resultHandle. \p key provides its name, and \p val,
    /// its value. Adds {\p key, \p val} to newStatsTable if it is not null.
    populateRes = populateInstrumentedStats(
        runtime, [&](llvh::StringRef key, double val) {
          GCScopeMarkerRAII marker{gcScope};

          return addToResultHandle(
              key, HermesValue::encodeDoubleValue(val), val);
        });
  } else {
    /// Adds a property named \p key to resultHandle if it is present in
    /// statsTable. Also copies it to newStatsTable if it is not null. Does
    /// nothing if \p key is not in statsTable.
    populateRes = populateInstrumentedStats(runtime, [&](llvh::StringRef key) {
      auto it = statsTable->find(key);
      if (it == statsTable->end()) {
        return ExecutionStatus::RETURNED;
      }

      GCScopeMarkerRAII marker{gcScope};

      auto valRes = statsTableValueToHermesValue(runtime, it->getValue());
      if (LLVM_UNLIKELY(valRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }

      return addToResultHandle(key, *valRes, it->getValue());
    });
  }

  if (LLVM_UNLIKELY(populateRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  if (storage->env && statsTable) {
    storage->env->callsToHermesInternalGetInstrumentedStats.pop_front();
  }
  if (LLVM_UNLIKELY(storage->shouldTrace)) {
    storage->tracedEnv.callsToHermesInternalGetInstrumentedStats.push_back(
        *newStatsTable);
  }

  return resultHandle.getHermesValue();
}

/// \return a static string summarising the presence and resolution type of
/// CommonJS modules across all RuntimeModules that have been loaded into \c
/// runtime.
static const char *getCJSModuleModeDescription(Runtime &runtime) {
  bool hasCJSModulesDynamic = false;
  bool hasCJSModulesStatic = false;
  for (const auto &runtimeModule : runtime.getRuntimeModules()) {
    if (runtimeModule.hasCJSModules()) {
      hasCJSModulesDynamic = true;
    }
    if (runtimeModule.hasCJSModulesStatic()) {
      hasCJSModulesStatic = true;
    }
  }
  if (hasCJSModulesDynamic && hasCJSModulesStatic) {
    return "Mixed dynamic/static";
  }
  if (hasCJSModulesDynamic) {
    return "Dynamically resolved";
  }
  if (hasCJSModulesStatic) {
    return "Statically resolved";
  }
  return "None";
}

/// \return an object mapping keys to runtime property values.
CallResult<HermesValue>
hermesInternalGetRuntimeProperties(void *, Runtime &runtime, NativeArgs args) {
  GCScope gcScope(runtime);
  auto resultHandle = runtime.makeHandle(JSObject::create(runtime));
  MutableHandle<> tmpHandle{runtime};

  /// Add a property \p value keyed under \p key to resultHandle.
  /// Return an ExecutionStatus.
  auto addProperty = [&](Handle<> value, const char *key) {
    auto keySym = symbolForCStr(runtime, key);
    if (LLVM_UNLIKELY(keySym == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    return JSObject::defineNewOwnProperty(
        resultHandle,
        runtime,
        **keySym,
        PropertyFlags::defaultNewNamedPropertyFlags(),
        value);
  };

#ifdef HERMES_FACEBOOK_BUILD
  tmpHandle =
      HermesValue::encodeBoolValue(std::strstr(__FILE__, "hermes-snapshot"));
  if (LLVM_UNLIKELY(
          addProperty(tmpHandle, "Snapshot VM") ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
#endif

  tmpHandle = HermesValue::encodeDoubleValue(::hermes::hbc::BYTECODE_VERSION);
  if (LLVM_UNLIKELY(
          addProperty(tmpHandle, "Bytecode Version") ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  tmpHandle = HermesValue::encodeBoolValue(runtime.builtinsAreFrozen());
  if (LLVM_UNLIKELY(
          addProperty(tmpHandle, "Builtins Frozen") ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  tmpHandle = HermesValue::encodeNumberValue(runtime.getVMExperimentFlags());
  if (LLVM_UNLIKELY(
          addProperty(tmpHandle, "VM Experiments") ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  const char buildMode[] =
#ifdef HERMES_SLOW_DEBUG
      "SlowDebug"
#elif !defined(NDEBUG)
      "Debug"
#else
      "Release"
#endif
      ;
  auto buildModeRes = StringPrimitive::create(
      runtime, ASCIIRef(buildMode, sizeof(buildMode) - 1));
  if (LLVM_UNLIKELY(buildModeRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  tmpHandle = *buildModeRes;
  if (LLVM_UNLIKELY(
          addProperty(tmpHandle, "Build") == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  std::string gcKind = runtime.getHeap().getKindAsStr();
  auto gcKindRes = StringPrimitive::create(
      runtime, ASCIIRef(gcKind.c_str(), gcKind.length()));
  if (LLVM_UNLIKELY(gcKindRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  tmpHandle = *gcKindRes;
  if (LLVM_UNLIKELY(
          addProperty(tmpHandle, "GC") == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

#ifdef HERMES_RELEASE_VERSION
  auto relVerRes =
      StringPrimitive::create(runtime, createASCIIRef(HERMES_RELEASE_VERSION));
  if (LLVM_UNLIKELY(relVerRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  tmpHandle = *relVerRes;
  if (LLVM_UNLIKELY(
          addProperty(tmpHandle, "OSS Release Version") ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
#endif

  const char *cjsModuleMode = getCJSModuleModeDescription(runtime);
  auto cjsModuleModeRes =
      StringPrimitive::create(runtime, createASCIIRef(cjsModuleMode));
  if (LLVM_UNLIKELY(cjsModuleModeRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  tmpHandle = *cjsModuleModeRes;
  if (LLVM_UNLIKELY(
          addProperty(tmpHandle, "CommonJS Modules") ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  return resultHandle.getHermesValue();
}

#ifdef HERMESVM_PLATFORM_LOGGING
static void logGCStats(Runtime &runtime, const char *msg) {
  // The GC stats can exceed the android logcat length limit, of
  // 1024 bytes.  Break it up.
  std::string stats;
  {
    llvh::raw_string_ostream os(stats);
    runtime.printHeapStats(os);
  }
  auto copyRegionFrom = [&stats](size_t from) -> size_t {
    size_t rBrace = stats.find("},", from);
    if (rBrace == std::string::npos) {
      std::string portion = stats.substr(from);
      hermesLog("HermesVM", "%s", portion.c_str());
      return stats.size();
    }

    // Add 2 for the length of the search string, to get to the end.
    const size_t to = rBrace + 2;
    std::string portion = stats.substr(from, to - from);
    hermesLog("HermesVM", "%s", portion.c_str());
    return to;
  };

  hermesLog("HermesVM", "%s:", msg);
  for (size_t ind = 0; ind < stats.size(); ind = copyRegionFrom(ind))
    ;
}
#endif

CallResult<HermesValue>
hermesInternalTTIReached(void *, Runtime &runtime, NativeArgs args) {
  runtime.ttiReached();
#ifdef HERMESVM_LLVM_PROFILE_DUMP
  __llvm_profile_dump();
  throw jsi::JSINativeException("TTI reached; profiling done");
#endif
#ifdef HERMESVM_PLATFORM_LOGGING
  logGCStats(runtime, "TTI call");
#endif
  return HermesValue::encodeUndefinedValue();
}

CallResult<HermesValue>
hermesInternalTTRCReached(void *, Runtime &runtime, NativeArgs args) {
  // Currently does nothing, but could change in the future.
  return HermesValue::encodeUndefinedValue();
}

CallResult<HermesValue>
hermesInternalIsProxy(void *, Runtime &runtime, NativeArgs args) {
  Handle<JSObject> obj = args.dyncastArg<JSObject>(0);
  return HermesValue::encodeBoolValue(obj && obj->isProxyObject());
}

CallResult<HermesValue>
hermesInternalHasPromise(void *, Runtime &runtime, NativeArgs args) {
  return HermesValue::encodeBoolValue(runtime.hasES6Promise());
}

CallResult<HermesValue>
hermesInternalUseEngineQueue(void *, Runtime &runtime, NativeArgs args) {
  return HermesValue::encodeBoolValue(runtime.hasMicrotaskQueue());
}

/// \code
///   HermesInternal.enqueueJob = function (func) {}
/// \endcode
CallResult<HermesValue>
hermesInternalEnqueueJob(void *, Runtime &runtime, NativeArgs args) {
  auto callable = args.dyncastArg<Callable>(0);
  if (!callable) {
    return runtime.raiseTypeError(
        "Argument to HermesInternal.enqueueJob must be callable");
  }
  runtime.enqueueJob(callable.get());
  return HermesValue::encodeUndefinedValue();
}

/// \code
///   HermesInternal.drainJobs = function () {}
/// \endcode
/// Throw if the drainJobs throws.
CallResult<HermesValue>
hermesInternalDrainJobs(void *, Runtime &runtime, NativeArgs args) {
  auto drainRes = runtime.drainJobs();
  if (drainRes == ExecutionStatus::EXCEPTION) {
    // No need to rethrow since it's already throw.
    return ExecutionStatus::EXCEPTION;
  }
  return HermesValue::encodeUndefinedValue();
}

#ifdef HERMESVM_EXCEPTION_ON_OOM
/// Gets the current call stack as a JS String value.  Intended (only)
/// to allow testing of Runtime::callStack() from JS code.
CallResult<HermesValue>
hermesInternalGetCallStack(void *, Runtime &runtime, NativeArgs args) {
  std::string stack = runtime.getCallStackNoAlloc();
  return StringPrimitive::create(runtime, ASCIIRef(stack.data(), stack.size()));
}
#endif // HERMESVM_EXCEPTION_ON_OOM

/// \return the code block associated with \p callableHandle if it is a
/// (possibly bound) JS function, or nullptr otherwise.
static const CodeBlock *getLeafCodeBlock(
    Handle<Callable> callableHandle,
    Runtime &runtime) {
  const Callable *callable = callableHandle.get();
  while (auto *bound = dyn_vmcast<BoundFunction>(callable)) {
    callable = bound->getTarget(runtime);
  }
  if (auto *asFunction = dyn_vmcast<const JSFunction>(callable)) {
    return asFunction->getCodeBlock(runtime);
  }
  return nullptr;
}

/// \return the file name associated with \p codeBlock, if any.
/// This mirrors the way we print file names for code blocks in JSError.
static CallResult<HermesValue> getCodeBlockFileName(
    Runtime &runtime,
    const CodeBlock *codeBlock,
    OptValue<hbc::DebugSourceLocation> location) {
  RuntimeModule *runtimeModule = codeBlock->getRuntimeModule();
  if (location) {
    auto debugInfo = runtimeModule->getBytecode()->getDebugInfo();
    return StringPrimitive::createEfficient(
        runtime, debugInfo->getFilenameByID(location->filenameId));
  } else {
    llvh::StringRef sourceURL = runtimeModule->getSourceURL();
    if (!sourceURL.empty()) {
      return StringPrimitive::createEfficient(runtime, sourceURL);
    }
  }
  return HermesValue::encodeUndefinedValue();
}

/// \code
///   HermesInternal.getFunctionLocation function (func) {}
/// \endcode
/// Returns an object describing the source location of func.
/// The following properties may be present:
/// * fileName (string)
/// * lineNumber (number) - 1 based
/// * columnNumber (number) - 1 based
/// * segmentID (number) - 0 based
/// * virtualOffset (number) - 0 based
/// * isNative (boolean)
/// TypeError if func is not a function.
CallResult<HermesValue>
hermesInternalGetFunctionLocation(void *, Runtime &runtime, NativeArgs args) {
  GCScope gcScope(runtime);

  auto callable = args.dyncastArg<Callable>(0);
  if (!callable) {
    return runtime.raiseTypeError(
        "Argument to HermesInternal.getFunctionLocation must be callable");
  }
  auto resultHandle = runtime.makeHandle(JSObject::create(runtime));
  MutableHandle<> tmpHandle{runtime};

  auto codeBlock = getLeafCodeBlock(callable, runtime);
  bool isNative = !codeBlock;
  auto res = JSObject::defineOwnProperty(
      resultHandle,
      runtime,
      Predefined::getSymbolID(Predefined::isNative),
      DefinePropertyFlags::getDefaultNewPropertyFlags(),
      runtime.getBoolValue(isNative));
  assert(res != ExecutionStatus::EXCEPTION && "Failed to set isNative");
  (void)res;

  if (codeBlock) {
    OptValue<hbc::DebugSourceLocation> location =
        codeBlock->getSourceLocation();
    if (location) {
      tmpHandle = HermesValue::encodeNumberValue(location->line);
      res = JSObject::defineOwnProperty(
          resultHandle,
          runtime,
          Predefined::getSymbolID(Predefined::lineNumber),
          DefinePropertyFlags::getDefaultNewPropertyFlags(),
          tmpHandle);
      assert(res != ExecutionStatus::EXCEPTION && "Failed to set lineNumber");
      (void)res;

      tmpHandle = HermesValue::encodeNumberValue(location->column);
      res = JSObject::defineOwnProperty(
          resultHandle,
          runtime,
          Predefined::getSymbolID(Predefined::columnNumber),
          DefinePropertyFlags::getDefaultNewPropertyFlags(),
          tmpHandle);
      assert(res != ExecutionStatus::EXCEPTION && "Failed to set columnNumber");
      (void)res;
    } else {
      tmpHandle = HermesValue::encodeNumberValue(
          codeBlock->getRuntimeModule()->getBytecode()->getSegmentID());
      res = JSObject::defineOwnProperty(
          resultHandle,
          runtime,
          Predefined::getSymbolID(Predefined::segmentID),
          DefinePropertyFlags::getDefaultNewPropertyFlags(),
          tmpHandle);
      assert(res != ExecutionStatus::EXCEPTION && "Failed to set segmentID");
      (void)res;

      tmpHandle = HermesValue::encodeNumberValue(codeBlock->getVirtualOffset());
      res = JSObject::defineOwnProperty(
          resultHandle,
          runtime,
          Predefined::getSymbolID(Predefined::virtualOffset),
          DefinePropertyFlags::getDefaultNewPropertyFlags(),
          tmpHandle);
      assert(
          res != ExecutionStatus::EXCEPTION && "Failed to set virtualOffset");
      (void)res;
    }

    auto fileNameRes = getCodeBlockFileName(runtime, codeBlock, location);
    if (LLVM_UNLIKELY(fileNameRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    tmpHandle = *fileNameRes;
    res = JSObject::defineOwnProperty(
        resultHandle,
        runtime,
        Predefined::getSymbolID(Predefined::fileName),
        DefinePropertyFlags::getDefaultNewPropertyFlags(),
        tmpHandle);
    assert(res != ExecutionStatus::EXCEPTION && "Failed to set fileName");
    (void)res;
  }
  JSObject::preventExtensions(*resultHandle);
  return resultHandle.getHermesValue();
}

/// \code
///   HermesInternal.setPromiseRejectionTrackingHook = function (func) {}
/// \endcode
/// Register the function which can be used to *enable* Promise rejection
/// tracking when the user calls it.
/// For example, when using the npm `promise` polyfill:
/// \code
///   HermesInternal.setPromiseRejectionTrackingHook(
///     require('./rejection-tracking.js').enable
///   );
/// \endcode
CallResult<HermesValue> hermesInternalSetPromiseRejectionTrackingHook(
    void *,
    Runtime &runtime,
    NativeArgs args) {
  runtime.promiseRejectionTrackingHook_ = args.getArg(0);
  return HermesValue::encodeUndefinedValue();
}

/// \code
///   HermesInternal.enablePromiseRejectionTracker = function (opts) {}
/// \endcode
/// Enable promise rejection tracking with the given opts.
CallResult<HermesValue> hermesInternalEnablePromiseRejectionTracker(
    void *,
    Runtime &runtime,
    NativeArgs args) {
  auto opts = args.getArgHandle(0);
  auto func = Handle<Callable>::dyn_vmcast(
      Handle<>(&runtime.promiseRejectionTrackingHook_));
  if (!func) {
    return runtime.raiseTypeError(
        "Promise rejection tracking hook was not registered");
  }
  return Callable::executeCall1(
             func, runtime, Runtime::getUndefinedValue(), opts.getHermesValue())
      .toCallResultHermesValue();
}

// #! Pyoncord added
// HermesInternal.getStrings(func: () => any): string[]
CallResult<HermesValue>
hermesInternalGetStrings(void *, Runtime &runtime, NativeArgs args) {
  auto func = args.dyncastArg<JSFunction>(0);
  if (!func) {
    return runtime.raiseTypeError(
        "Can't call HermesInternal.getStrings() on non-function");
  }

  auto funcId = func->getCodeBlock(runtime)->getFunctionID();

  auto arrayResult = JSArray::create(runtime, 0, 0);
  if (LLVM_UNLIKELY(arrayResult == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  auto array = *arrayResult;

  StringVisitor visitor(
      func->getRuntimeModule(runtime)->getBytecodeSharedPtr(), array, runtime);
  visitor.visitInstructionsInFunction(funcId);

  return visitor.array_.getHermesValue();
}

Handle<JSObject> createHermesInternalObject(
    Runtime &runtime,
    const JSLibFlags &flags) {
  namespace P = Predefined;
  Handle<JSObject> intern = runtime.makeHandle(JSObject::create(runtime));
  GCScope gcScope{runtime};

  DefinePropertyFlags constantDPF =
      DefinePropertyFlags::getDefaultNewPropertyFlags();
  constantDPF.enumerable = 0;
  constantDPF.writable = 0;
  constantDPF.configurable = 0;

  auto defineInternMethod =
      [&](Predefined::Str symID, NativeFunctionPtr func, uint8_t count = 0) {
        (void)defineMethod(
            runtime,
            intern,
            Predefined::getSymbolID(symID),
            nullptr /* context */,
            func,
            count,
            constantDPF);
      };

  auto defineInternMethodAndSymbol =
      [&](const char *name, NativeFunctionPtr func, uint8_t count = 0) {
        ASCIIRef ref = createASCIIRef(name);
        Handle<SymbolID> symHandle = runtime.ignoreAllocationFailure(
            runtime.getIdentifierTable().getSymbolHandle(runtime, ref));
        (void)defineMethod(
            runtime,
            intern,
            *symHandle,
            nullptr /* context */,
            func,
            count,
            constantDPF);
      };

  // suppress unused-variable warning
  (void)defineInternMethodAndSymbol;

  // Make a copy of the original String.prototype.concat implementation that we
  // can use internally.
  // TODO: we can't make HermesInternal.concat a static builtin method now
  // because this method should be called with a meaningful `this`, but
  // CallBuiltin instruction does not support it.
  auto propRes = JSObject::getNamed_RJS(
      runtime.makeHandle<JSObject>(runtime.stringPrototype),
      runtime,
      Predefined::getSymbolID(Predefined::concat));
  assert(
      propRes != ExecutionStatus::EXCEPTION && !(*propRes)->isUndefined() &&
      "Failed to get String.prototype.concat.");
  auto putRes = JSObject::defineOwnProperty(
      intern,
      runtime,
      Predefined::getSymbolID(Predefined::concat),
      constantDPF,
      runtime.makeHandle(std::move(*propRes)));
  assert(
      putRes != ExecutionStatus::EXCEPTION && *putRes &&
      "Failed to set HermesInternal.concat.");
  (void)putRes;

  // HermesInternal functions that are known to be safe and are required to be
  // present by the VM internals even under a security-sensitive environment
  // where HermesInternal might be explicitly disabled.
  defineInternMethod(P::hasPromise, hermesInternalHasPromise);
  defineInternMethod(P::enqueueJob, hermesInternalEnqueueJob);
  defineInternMethod(
      P::setPromiseRejectionTrackingHook,
      hermesInternalSetPromiseRejectionTrackingHook);
  defineInternMethod(
      P::enablePromiseRejectionTracker,
      hermesInternalEnablePromiseRejectionTracker);
  defineInternMethod(P::useEngineQueue, hermesInternalUseEngineQueue);

  // All functions are known to be safe can be defined above this flag check.
  if (!flags.enableHermesInternal) {
    JSObject::preventExtensions(*intern);
    return intern;
  }

  // HermesInternal functions that are not necessarily required but are
  // generally considered harmless to be exposed by default.
  defineInternMethod(P::getEpilogues, hermesInternalGetEpilogues);
  defineInternMethod(
      P::getInstrumentedStats, hermesInternalGetInstrumentedStats);
  defineInternMethod(
      P::getRuntimeProperties, hermesInternalGetRuntimeProperties);
  defineInternMethod(P::ttiReached, hermesInternalTTIReached);
  defineInternMethod(P::ttrcReached, hermesInternalTTRCReached);
  defineInternMethod(P::getFunctionLocation, hermesInternalGetFunctionLocation);

  // #! Pyoncord added
  defineInternMethod(P::getStrings, hermesInternalGetStrings);

  // HermesInternal function that are only meant to be used for testing purpose.
  // They can change language semantics and are security risks.
  if (flags.enableHermesInternalTestMethods) {
    defineInternMethod(
        P::detachArrayBuffer, hermesInternalDetachArrayBuffer, 1);
    defineInternMethod(P::getWeakSize, hermesInternalGetWeakSize);
    defineInternMethod(
        P::copyDataProperties, hermesBuiltinCopyDataProperties, 3);
    defineInternMethodAndSymbol("isProxy", hermesInternalIsProxy);
    defineInternMethod(P::drainJobs, hermesInternalDrainJobs);
  }

#ifdef HERMESVM_EXCEPTION_ON_OOM
  defineInternMethodAndSymbol("getCallStack", hermesInternalGetCallStack, 0);
#endif // HERMESVM_EXCEPTION_ON_OOM

  JSObject::preventExtensions(*intern);

  return intern;
}

} // namespace vm
} // namespace hermes
