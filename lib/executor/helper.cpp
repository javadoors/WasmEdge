// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2022 Second State INC

#include "executor/executor.h"

#include "common/log.h"
#include "system/fault.h"

#include <cstdint>
#include <numeric>
#include <utility>
#include <vector>

namespace WasmEdge {
namespace Executor {

Expect<AST::InstrView::iterator>
Executor::enterFunction(Runtime::StoreManager &StoreMgr,
                        Runtime::StackManager &StackMgr,
                        const Runtime::Instance::FunctionInstance &Func,
                        const AST::InstrView::iterator From) {
  if (unlikely(StopToken.exchange(0, std::memory_order_relaxed))) {
    spdlog::error(ErrCode::Interrupted);
    return Unexpect(ErrCode::Interrupted);
  }
  // Get function type
  const auto &FuncType = Func.getFuncType();
  const uint32_t ArgsN = static_cast<uint32_t>(FuncType.getParamTypes().size());
  const uint32_t RetsN =
      static_cast<uint32_t>(FuncType.getReturnTypes().size());

  if (Func.isHostFunction()) {
    // Host function case: Push args and call function.
    auto &HostFunc = Func.getHostFunc();

    // Get memory instance from current frame.
    // It'll be nullptr if current frame is dummy frame or no memory instance
    // in current module.
    const auto *ModInst = StackMgr.getModule();
    Runtime::Instance::MemoryInstance *MemoryInst = nullptr;
    if (ModInst != nullptr) {
      if (auto Res = ModInst->getMemory(0); Res) {
        MemoryInst = *Res;
      }
    }

    if (Stat) {
      // Check host function cost.
      if (unlikely(!Stat->addCost(HostFunc.getCost()))) {
        spdlog::error(ErrCode::CostLimitExceeded);
        return Unexpect(ErrCode::CostLimitExceeded);
      }
      // Start recording time of running host function.
      Stat->stopRecordWasm();
      Stat->startRecordHost();
    }

    // Run host function.
    Span<ValVariant> Args = StackMgr.getTopSpan(ArgsN);
    std::vector<ValVariant> Rets(RetsN);
    auto Ret = HostFunc.run(MemoryInst, std::move(Args), Rets);

    if (Stat) {
      // Stop recording time of running host function.
      Stat->stopRecordHost();
      Stat->startRecordWasm();
    }

    if (!Ret) {
      if (Ret.error() == ErrCode::ExecutionFailed) {
        spdlog::error(Ret.error());
      }
      return Unexpect(Ret);
    }

    // Push returns back to stack.
    for (uint32_t I = 0; I < ArgsN; ++I) {
      ValVariant Val [[maybe_unused]] = StackMgr.pop();
    }
    for (auto &R : Rets) {
      StackMgr.push(std::move(R));
    }

    // For host function case, the continuation will be the next.
    return From;
  } else if (Func.isCompiledFunction()) {
    // Compiled function case: Push frame with locals and args.
    StackMgr.pushFrame(Func.getModule(), // Module address
                       From - 1,         // Return PC
                       0,                // No Arguments in stack
                       RetsN             // Returns num
    );

    Span<ValVariant> Args = StackMgr.getTopSpan(ArgsN);
    std::vector<ValVariant> Rets(RetsN);

    {
      CurrentStore = &StoreMgr;
      CurrentStack = &StackMgr;
      auto &ModInst = *Func.getModule();
      for (uint32_t I = 0; I < ModInst.getMemNum(); ++I) {
        auto MemoryPtr =
            reinterpret_cast<std::atomic<uint8_t *> *>(&ModInst.MemoryPtrs[I]);
        uint8_t *const DataPtr = (*ModInst.getMemory(I))->getDataPtr();
        std::atomic_store_explicit(MemoryPtr, DataPtr,
                                   std::memory_order_relaxed);
      }
      ExecutionContext.Memories = ModInst.MemoryPtrs.data();
      ExecutionContext.Globals = ModInst.GlobalPtrs.data();
    }

    {
      Fault FaultHandler;
      if (auto Err = PREPARE_FAULT(FaultHandler);
          unlikely(Err != ErrCode::Success)) {
        if (Err != ErrCode::Terminated) {
          spdlog::error(Err);
        }
        return Unexpect(Err);
      }
      auto &Wrapper = FuncType.getSymbol();
      Wrapper(&ExecutionContext, Func.getSymbol().get(), Args.data(),
              Rets.data());
    }

    for (uint32_t I = 0; I < Rets.size(); ++I) {
      StackMgr.push(Rets[I]);
    }

    StackMgr.popFrame();
    // For compiled function case, the continuation will be the next.
    return From;
  } else {
    const uint32_t LocalN = std::accumulate(
        Func.getLocals().begin(), Func.getLocals().end(), UINT32_C(0),
        [](uint32_t N, const auto &Pair) -> uint32_t {
          return N + Pair.first;
        });
    // Native function case: Push frame with locals and args.
    StackMgr.pushFrame(Func.getModule(), // Module address
                       From - 1,         // Return PC
                       ArgsN + LocalN,   // Arguments num + local num
                       RetsN             // Returns num
    );

    // Push local variables to stack.
    for (auto &Def : Func.getLocals()) {
      for (uint32_t i = 0; i < Def.first; i++) {
        StackMgr.push(ValueFromType(Def.second));
      }
    }

    // For native function case, the continuation will be the start of
    // function body.
    return Func.getInstrs().begin();
  }
}

Expect<void> Executor::branchToLabel(Runtime::StackManager &StackMgr,
                                     uint32_t EraseBegin, uint32_t EraseEnd,
                                     int32_t PCOffset,
                                     AST::InstrView::iterator &PC) noexcept {
  // Check stop token
  if (unlikely(StopToken.exchange(0, std::memory_order_relaxed))) {
    spdlog::error(ErrCode::Interrupted);
    return Unexpect(ErrCode::Interrupted);
  }

  StackMgr.stackErase(EraseBegin, EraseEnd);
  PC += PCOffset;
  return {};
}

Runtime::Instance::TableInstance *
Executor::getTabInstByIdx(Runtime::StackManager &StackMgr,
                          const uint32_t Idx) const {
  const auto *ModInst = StackMgr.getModule();
  // When top frame is dummy frame, cannot find instance.
  if (unlikely(ModInst == nullptr)) {
    return nullptr;
  }
  return ModInst->unsafeGetTable(Idx);
}

Runtime::Instance::MemoryInstance *
Executor::getMemInstByIdx(Runtime::StackManager &StackMgr,
                          const uint32_t Idx) const {
  const auto *ModInst = StackMgr.getModule();
  // When top frame is dummy frame, cannot find instance.
  if (unlikely(ModInst == nullptr)) {
    return nullptr;
  }
  return ModInst->unsafeGetMemory(Idx);
}

Runtime::Instance::GlobalInstance *
Executor::getGlobInstByIdx(Runtime::StackManager &StackMgr,
                           const uint32_t Idx) const {
  const auto *ModInst = StackMgr.getModule();
  // When top frame is dummy frame, cannot find instance.
  if (unlikely(ModInst == nullptr)) {
    return nullptr;
  }
  return ModInst->unsafeGetGlobal(Idx);
}

Runtime::Instance::ElementInstance *
Executor::getElemInstByIdx(Runtime::StackManager &StackMgr,
                           const uint32_t Idx) const {
  const auto *ModInst = StackMgr.getModule();
  // When top frame is dummy frame, cannot find instance.
  if (unlikely(ModInst == nullptr)) {
    return nullptr;
  }
  return ModInst->unsafeGetElem(Idx);
}

Runtime::Instance::DataInstance *
Executor::getDataInstByIdx(Runtime::StackManager &StackMgr,
                           const uint32_t Idx) const {
  const auto *ModInst = StackMgr.getModule();
  // When top frame is dummy frame, cannot find instance.
  if (unlikely(ModInst == nullptr)) {
    return nullptr;
  }
  return ModInst->unsafeGetData(Idx);
}

} // namespace Executor
} // namespace WasmEdge
